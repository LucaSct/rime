// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "rime/assets/destructible_asset.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/events.hpp"
#include "rime/destruction/ids.hpp"
#include "rime/physics/body.hpp"

// The physics world is BORROWED — destruction registers geometry into it and creates the bodies
// that stand for destructibles, but never owns it (the engine owns the one PhysicsWorld).
// Forward-declared so this seam header stays free of the physics internals; only world.cpp pulls
// the full seam in.
namespace rime::physics {
class PhysicsWorld;
}

// DestructionWorld — the runtime home of destructibles (M8.2/M8.3, ADR-0029). It turns a cooked
// `assets::DestructibleAsset` (the m8.1 fracture pattern) into standing physics: a **pattern** is
// registered ONCE (each part a convex hull, the whole a compound — ADR-0027/0028), and
// **instances** are stamped out of it cheaply, each ONE static compound body plus per-part runtime
// state (health, alive bits). M8.3 adds the breaking half: damage (contact impulse + explicit
// `apply_damage` ops) erodes parts, a support solve over the live bond graph finds parts no longer
// held by an anchor, and those detach via the fracture BODY SWAP into real dynamic debris bodies —
// all in `update()`, all deterministically (the M11 replay contract; `state_hash` is the witness).
// The event fan-out is m8.4, budgets/lifetime are m8.5, and per-part render leaves land with the
// m8.6 sample (a body's `part_placement` is the hook).
//
// Destruction OWNS its physics bodies directly (create/destroy on the PhysicsWorld), not through
// PhysicsSync — the ECS `Collider` cannot name a hull/compound id (ADR-0029 §6). The module is
// removable (guardrail 2): nothing below it depends on it.
namespace rime::destruction {

// The handle types (PatternId, InstanceId) and kInvalidPartIndex live in ids.hpp so the event
// payloads can share them without a circular include; they are part of this seam's vocabulary.

class DestructionWorld {
public:
    DestructionWorld();
    ~DestructionWorld();

    // Non-copyable: it owns a slice of a simulation (registered patterns, live instances).
    DestructionWorld(const DestructionWorld&) = delete;
    DestructionWorld& operator=(const DestructionWorld&) = delete;

    // Register a cooked pattern into `world` — the cold path that pays for everything spawning
    // needs: `register_hull` per part, then `register_compound` over the parts (each child the
    // part's hull at its cooked COM, identity-rotated — the destructible frame). Returns the id
    // instances reference. The NULL id (`is_valid() == false`) if any part's hull or the compound
    // is rejected by the physics validator — a malformed cook (the m8.1 oracle proves a good cook
    // passes every check). Call once per distinct destructible; `spawn` then costs one body + two
    // small vectors. Not safe to call concurrently with a step of `world` (it mutates the
    // hull/compound stores).
    [[nodiscard]] PatternId register_pattern(const assets::DestructibleAsset& asset,
                                             physics::PhysicsWorld& world);

    // Stand an instance of `pattern` at `placement` — ONE static compound body (ADR-0029 §1, the
    // intact wall) plus per-part state: every part alive, full (1.0) health. Returns the
    // InstanceId, or the null id for an unknown pattern. The body is `MotionType::Static`, so a
    // bound-but-untouched wall costs the simulation nothing (it is never awake) — the m8.2 "≈
    // static baseline" property.
    [[nodiscard]] InstanceId
    spawn(PatternId pattern, const core::Transform& placement, physics::PhysicsWorld& world);

    // --- damage → connectivity → fracture (M8.3, ADR-0029 §2–§4) ---------------------------------

    // Queue explicit radius damage against an instance — the explosion / scripted-hit shape (the
    // hitscan shape is the same call with a small radius at the RayHit::child part's position).
    // Nothing happens until the next update(): ops are collected, canonically sorted, and applied
    // there, so the ARRIVAL order of same-tick calls can never change the outcome (the ADR-0029 §3
    // determinism rule). `amount` is health damage at the blast centre, fading LINEARLY to zero at
    // `radius` (v1's falloff, measured against each cooked part AABB carried through the instance
    // placement); `impulse` is the world-space push (kg·m/s, also falloff-scaled) a part carries
    // into the debris body it leaves with — whether it detaches as an orphaned island or is struck
    // dead outright (a killed part flies off as its own chunk, ADR-0029 §2); a part left standing
    // absorbs its share. An unknown/stale instance is a safe no-op.
    void apply_damage(InstanceId instance,
                      core::Vec3 point,
                      float radius,
                      float amount,
                      core::Vec3 impulse);

    // The per-tick destruction update — call ONCE per simulation tick, AFTER world.step(), in the
    // sequential tail (ADR-0029 §8; it mutates the body population, which is never legal during a
    // step). The pipeline, every stage in canonical order so the whole thing is a pure function of
    // its inputs (the M11 replay contract):
    //   1. drain world.contact_events() — a region's normal_impulse above the pattern's cooked
    //      damage_threshold erodes the struck part (child index → part id via the instance's remap
    //      table); the threshold is what fences the resting m·g·dt case (a Persisted contact of a
    //      standing wall must not grind it down);
    //   2. apply the queued apply_damage ops (sorted by instance, part, op bytes);
    //   3. per damaged instance: the union-find support solve over live bonds from the alive
    //      anchors; unsupported parts form islands;
    //   4. the fracture BODY SWAP (§2): if the instance's membership changed, destroy its compound
    //      body, re-register the anchored remainder as a fresh static compound, and spawn each
    //      island as a dynamic body (one part → its hull; several → a runtime dynamic compound)
    //      with inherited velocity plus the damage impulses that hit its members.
    // Consumes the pending damage queue and this tick's contact events (calling it twice per step
    // would count the same events twice — don't).
    void update(physics::PhysicsWorld& world);

    // --- read-back -------------------------------------------------------------------------------
    [[nodiscard]] std::size_t pattern_count() const noexcept;
    [[nodiscard]] std::size_t instance_count() const noexcept;

    // The physics body currently standing for this instance — the intact compound at spawn, the
    // re-registered anchored remainder after a fracture. Null for an unknown id, and null once the
    // instance has FULLY collapsed (no anchored part left standing — there is nothing to stand).
    [[nodiscard]] physics::BodyId body_of(InstanceId instance) const noexcept;

    // The part count of a pattern / an instance (0 for an unknown id).
    [[nodiscard]] std::uint32_t part_count(PatternId pattern) const noexcept;
    [[nodiscard]] std::uint32_t instance_part_count(InstanceId instance) const noexcept;

    // Per-part runtime state. `part_alive` means "still standing in the instance's compound":
    // false once the part LEFT the wall — either struck dead (health hit zero, so it flies off as
    // its own debris chunk carrying the killing impulse, ADR-0029 §2) or DETACHED as an orphaned
    // island. Its identity lives on in the debris roster below; a killed part's health reads 0, a
    // detached part's freezes at the value it left with. False / 0 for an unknown instance or part
    // index.
    [[nodiscard]] bool part_alive(InstanceId instance, std::uint32_t part) const noexcept;
    [[nodiscard]] float part_health(InstanceId instance, std::uint32_t part) const noexcept;

    // The child-index → part-id remap of the instance's CURRENT compound body (ADR-0029 §4 — the
    // M11.4 addressing contract). Because fracture re-registers the compound, a part's child index
    // changes across its life; this is the one true mapping, and it is derivable — its rows are
    // the standing part ids in ascending order. A ContactEvent::child_a/child_b or RayHit::child
    // on this instance's body goes through here to name the part. kInvalidPartIndex for an unknown
    // instance or an out-of-range child.
    [[nodiscard]] std::uint32_t part_from_child(InstanceId instance,
                                                std::uint16_t child) const noexcept;

    // The pattern's connectivity, for m8.3's support solve and for tooling: `bonds` (a<b pairs with
    // a shared-area strength) and `anchors` (part indices pinned to the world). Empty for an
    // unknown id.
    [[nodiscard]] std::span<const assets::DestructibleBond> bonds(PatternId pattern) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> anchors(PatternId pattern) const noexcept;

    // Where part `part` sits in the world: its cooked COM carried through the instance placement,
    // with the instance's rotation (an intact part does not rotate relative to its destructible).
    // This is what a per-part render leaf draws at (m8.6). Identity for an unknown instance/part.
    [[nodiscard]] core::Transform part_placement(InstanceId instance,
                                                 std::uint32_t part) const noexcept;

    // --- debris read-back (M8.3) ------------------------------------------------------------

    // The debris bodies fracture has produced, in CREATION order — a canonical order (instances
    // ascending, islands by lowest member part), so index i names the same island on every run
    // (the composition half of the M11 witness). Append-only in v1: lifetime/despawn is m8.5, so
    // a settled piece stays on the roster.
    [[nodiscard]] std::size_t debris_count() const noexcept;

    // Debris #i's physics body / source instance / member part ids (cook order, ascending). A
    // one-part island is a hull body; a multi-part island is a runtime dynamic compound (ADR-0029
    // §2). Null / empty for an out-of-range index.
    [[nodiscard]] physics::BodyId debris_body(std::size_t debris) const noexcept;
    [[nodiscard]] InstanceId debris_source(std::size_t debris) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> debris_parts(std::size_t debris) const noexcept;

    // --- event fan-out (M8.4, ADR-0029 §7) ------------------------------------------------------

    // The destruction events the most recent update() produced — PartDamaged / PartDied /
    // IslandDetached / DebrisSettled — in a canonical, replay-stable order, as a span valid until
    // the next update(). This is the fan-out seam: the VFX dust stub, the engine/audio null
    // backend, and gameplay each read this one immutable span (remove any consumer and the others
    // are byte-identical — guardrail 2). Empty after an update() that broke nothing (the channel is
    // clean every quiet tick). See rime/destruction/events.hpp for the payload.
    [[nodiscard]] std::span<const DestructionEvent> events() const noexcept;

    // A 64-bit fingerprint of ALL destruction state, in canonical order: every instance's body id,
    // per-part alive bits and health, and every debris body's identity + composition. The M8 half
    // of the M11 determinism witness — two runs that fed identical inputs must match on BOTH
    // state_hash() and PhysicsWorld::world_hash(), for any physics worker count (destruction runs
    // in the sequential tail, so worker count cannot touch it — this hash is the proof). FNV-1a,
    // field by field (never over padded structs), like world_hash.
    [[nodiscard]] std::uint64_t state_hash() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::destruction
