// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "rime/assets/destructible_asset.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/damage_op.hpp"
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

// Debris lifetime & budget knobs (M8.5, ADR-0029 §8). A default-constructed config is DISABLED, so
// a DestructionWorld behaves exactly as it did through M8.4 — debris and re-registered remainder
// compounds accumulate forever, state byte-for-byte unchanged — until a caller opts in. Enabled, it
// reclaims settled debris after a linger and caps the live debris-body population, which is what
// keeps the physics hull/compound/body stores BOUNDED under continuous refracture (it leans on the
// M8.5 physics half: unregister_hull/unregister_compound). Everything it does is deterministic, so
// two runs that share inputs AND this config reclaim the same bodies (the M11 replay contract).
struct LifecycleConfig {
    // The master switch. Off ⇒ nothing is ever reclaimed (the M8.3/8.4 append-only behaviour);
    // on ⇒ bodies are destroyed and their owned compounds unregistered as debris age out.
    bool enabled = false;

    // How many update() ticks a debris lingers after coming to rest (a physics Slept) before it is
    // frozen — its body destroyed, its record kept. At 60 Hz, 120 ≈ two seconds of settled rubble.
    std::uint32_t freeze_delay_ticks = 120;

    // The cap on LIVE (not-yet-frozen) debris bodies. When exceeded, settled debris are frozen
    // early — largest-and-oldest first — until the population is back under the cap. A still-moving
    // (unsettled) piece is never evicted, so under a burst the cap is briefly soft and catches up.
    std::uint32_t max_live_debris = 128;

    // The VISUAL budget (m13.2b — ADR-0032 C6): how many debris rows may still be considered
    // visible. Past it, the OLDEST frozen debris are RETIRED — marked, and announced on the C2
    // channel as `DebrisEvicted` carrying the extent they last occupied — so a consumer can drop
    // the render leaf, the SDF stamp and the shadow-caster entry that outlive the physics body.
    //
    // DELIBERATELY LARGER THAN `max_live_debris`, because that is the whole point: rubble should
    // stay visible long after it stops simulating (the cheap, static, "the wreckage is still
    // there" look) and still be ultimately bounded. 512 against a 128-body live cap gives roughly
    // four generations of settled rubble on screen.
    //
    // 0 means UNBOUNDED — the pre-C6 behaviour, where the visual population grows forever. Kept as
    // a spelling so the leak can be reproduced deliberately, which is what the proof does.
    std::uint32_t max_visual_debris = 512;
};

// Who decides what happens to an instance (m11.4, ADR-0033 §1 + A1).
//
// `Local` is the M8 behaviour and the default: this peer owns the instance, so both damage sources
// feed it — explicit `apply_damage` calls and the contact→damage conversion drained from the
// solver's own impulses.
//
// `Remote` marks a MIRROR of an instance some other peer is authoritative for. Its state changes
// only through `apply_remote_ops` (and the state-application seam below). Both local damage sources
// are refused for it: `apply_damage` becomes a no-op, and the contact drain skips it. That second
// half is the one that matters and the one that is easy to get wrong — a mirror's debris still
// rests against it, still generates real contact impulses in the client's own solver, and
// converting those locally would erode the part a second time, on top of the server's
// already-replicated erosion. The divergence would be silent, cumulative, and worst exactly where
// the destruction is most interesting.
enum class Authority : std::uint8_t { Local = 0, Remote = 1 };

class DestructionWorld {
public:
    DestructionWorld();
    ~DestructionWorld();

    // Non-copyable: it owns a slice of a simulation (registered patterns, live instances).
    DestructionWorld(const DestructionWorld&) = delete;
    DestructionWorld& operator=(const DestructionWorld&) = delete;

    // Install the debris lifetime & budget policy (M8.5, ADR-0029 §8). Off by default; turn it on
    // to keep the physics stores bounded under sustained refracture (see LifecycleConfig). Takes
    // effect from the next update(); changing it does not retroactively reclaim already-standing
    // debris.
    void configure_lifecycle(const LifecycleConfig& config);

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

    // --- replication (m11.4, ADR-0033 A1/A3) -----------------------------------------------------

    // Declare who owns `instance` (see Authority above). Default `Local`; an unknown id is a safe
    // no-op. Set this BEFORE the instance takes any damage — flipping it mid-life is legal but only
    // changes what happens from the next update() on, and does not retroactively un-apply anything.
    void set_authority(InstanceId instance, Authority authority);

    [[nodiscard]] Authority authority_of(InstanceId instance) const noexcept;

    // The damage ops the most recent update() COMMITTED, in the canonical order they were applied —
    // explicit ops (already expanded per-part, falloff resolved) followed by contact-derived ops.
    // This is the artifact ADR-0033 A1 replicates: the server publishes this span, clients feed it
    // back through apply_remote_ops. Valid until the next update(), like events().
    //
    // It is the FULL committed list, including ops that landed on an already-dead part and were
    // absorbed. Filtering those out here would be a false economy: whether an op is absorbed
    // depends on the state it lands against, so a receiver that was told only about the ops that
    // "mattered" on the sender could not reproduce the sender's accumulation. The op list is the
    // input to the deterministic function, and inputs are replicated whole.
    [[nodiscard]] std::span<const DamageOp> committed_ops() const noexcept;

    // Queue server-authoritative ops for the next update(). Applied in the order given, ahead of
    // any locally-sourced ops — position is arithmetically free, because health accumulates per
    // (instance, part) and a given instance is either Local or Remote, never both, so remote and
    // local ops can never interleave within one part's run.
    //
    // Ops naming an unknown instance are dropped. Ops naming a Local instance are dropped too: that
    // is a caller bug (the server does not own this instance), and applying them would let a
    // mirror's damage bleed into state this peer is itself authoritative for.
    void apply_remote_ops(std::span<const DamageOp> ops);

    // --- the state-application seam (ADR-0033 A3) ------------------------------------------------
    //
    // Before m11.4 `apply_damage` was the ONLY mutator on this path, so a client that joined late —
    // or that the server had to correct — could be handed the event stream but never the state it
    // implies ("these parts are gone, this debris is here"). These two calls close that hole. They
    // are the uncommon path: ordinary play replays ops, and these run on late-join and on detected
    // drift.

    // Force `dead_parts` out of `instance` and replay the ordinary fracture body-swap over the
    // result — the SAME code path a natural fracture takes, deliberately, so the alive bits, the
    // re-registered remainder compound, the debris roster and its canonical creation order all end
    // up exactly as they would have. A correction that reached the same visible state by a
    // different route would leave the two peers agreeing on what you can see and disagreeing on the
    // tables underneath it, which is worse than not correcting at all.
    //
    // `healths` is parallel to `dead_parts` and carries the health each part should freeze at (a
    // struck-dead part reads 0; a part that merely detached with an island keeps what it left
    // with). Pass an empty span to zero them all. Parts already dead are skipped, so this is
    // idempotent — applying the same detach set twice does nothing the second time. Out-of-range
    // part ids and unknown instances are dropped. Emits no events: this is a correction, not a
    // thing that happened, and firing dust and audio for a late-join's worth of already-old
    // destruction is exactly the artifact that would give the correction away.
    void apply_detach_set(InstanceId instance,
                          std::span<const std::uint32_t> dead_parts,
                          std::span<const float> healths,
                          physics::PhysicsWorld& world);

    // Overwrite debris #d's motion state — the kinematics of last resort, for when the replicated
    // transform and the local sim have drifted past what interpolation can hide. A frozen or
    // out-of-range debris is a safe no-op. Deliberately narrow: it moves a body, it never changes
    // the roster, because debris IDENTITY and composition are derived from the op stream and must
    // stay that way (m11.4b replicates the transforms; it does not get to invent a chunk).
    void set_debris_state(std::size_t debris,
                          const physics::BodyState& state,
                          physics::PhysicsWorld& world);

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

    // Has debris #i been RETIRED by the visual budget (m13.2b)? A retired row keeps its index —
    // the roster is append-only and `destruction_net` indexes it directly — but a consumer should
    // treat it as gone: no render leaf, no SDF stamp, no shadow caster.
    //
    // Distinct from "frozen", and the distinction matters: a frozen debris has no physics body but
    // is still SEEN, which is exactly the state ADR-0029 §8 created on purpose. `debris_body()`
    // returns null for both, so a consumer that only checked the body could not tell the two apart
    // — and the one that matters visually is this one.
    [[nodiscard]] bool debris_retired(std::size_t debris) const noexcept;

    // How many debris rows are still visible — not retired. The ledger number ADR-0035 §2a's C6
    // ruling wants bounded: it must plateau at `max_visual_debris` across a long run rather than
    // climbing with the roster.
    [[nodiscard]] std::size_t visual_debris_count() const noexcept;
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
