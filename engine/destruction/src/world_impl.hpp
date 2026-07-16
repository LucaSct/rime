// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "rime/core/containers/event_channel.hpp"
#include "rime/destruction/events.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/shape.hpp"

// The DestructionWorld internals, shared by the module's translation units — world.cpp (load,
// stand, bind — M8.2) and damage.cpp (damage, connectivity, the fracture body swap — M8.3).
// PRIVATE under src/, invisible above the seam, exactly like the physics module's internal
// headers. Plain vectors and indices throughout: every table is iterated in ascending index order
// on the damage path, which is half of the determinism story (the other half is the canonical op
// sort in damage.cpp) — no unordered container's iteration order can leak into an outcome.
namespace rime::destruction {

struct DestructionWorld::Impl {
    // A registered pattern: the physics compound that IS the intact shape, the per-part hull ids /
    // COMs / volumes / AABBs (kept for the m8.3 fracture body-swap, which re-registers subsets,
    // and for radius-damage queries), and the cooked connectivity + damage material.
    struct Pattern {
        physics::CompoundId compound{};
        std::uint32_t part_count = 0;
        std::vector<physics::HullId> hulls;
        std::vector<core::Vec3> part_com;
        std::vector<float> part_volume;        // m³ — debris mass under the uniform-density model
        std::vector<core::Vec3> part_aabb_min; // cooked part bounds, destructible frame — the
        std::vector<core::Vec3> part_aabb_max; // radius-damage overlap set (ADR-0029 §3)
        std::vector<assets::DestructibleBond> bonds;
        std::vector<std::uint32_t> anchors;
        core::Vec3 compound_centroid{0.0f, 0.0f, 0.0f}; // intact compound's COM, authored frame
        core::Vec3 half_extents{0.0f, 0.0f, 0.0f};
        float damage_threshold = 0.0f;
        float damage_scale = 0.0f;
    };

    // A standing instance: its pattern, the compound body currently standing for it, where it was
    // placed, and the per-part state the damage path mutates. `alive[p]` means "part p is still
    // standing in this instance's compound" — it drops to 0 when the part ERODES (health reached
    // zero) or DETACHES (left as debris; health keeps its remaining value). `child_to_part` is the
    // ADR-0029 §4 remap: row c = the part id of compound child c of the CURRENT body, rebuilt at
    // every body swap as the standing part ids in ascending order.
    struct Instance {
        PatternId pattern{};
        physics::BodyId body{};
        // The compound the CURRENT standing body uses: the shared pattern compound at spawn, then a
        // freshly re-registered remainder after each fracture swap. M8.5 reads it to free the
        // PREVIOUS remainder on the next swap — but never the shared pattern compound (see
        // damage.cpp).
        physics::CompoundId compound{};
        core::Transform placement{};
        std::vector<float> health;
        std::vector<std::uint8_t> alive;
        std::vector<std::uint32_t> child_to_part;
    };

    // One queued apply_damage call, exactly as the caller passed it. Arrival order is deliberately
    // NOT meaningful — update() erases it with the canonical op sort (ADR-0029 §3).
    struct DamageCall {
        std::uint32_t instance = 0;
        core::Vec3 point{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        float amount = 0.0f;
        core::Vec3 impulse{0.0f, 0.0f, 0.0f};
    };

    // One damage operation against one part — the normalized currency both damage sources
    // (explicit apply_damage calls and contact events) reduce to before anything is applied.
    // `impulse` is the world-space push the op carries into whatever debris body its part detaches
    // with; contact ops push at their contact `point` (the real surface point — the lever arm is
    // part of the look), explicit ops push through the COM (`central` — a blast centre is usually
    // outside the body, and an invented lever arm there would add spin the caller never asked
    // for).
    struct DamageOp {
        std::uint32_t instance = 0;
        std::uint32_t part = 0;
        float amount = 0.0f;
        core::Vec3 impulse{0.0f, 0.0f, 0.0f};
        core::Vec3 point{0.0f, 0.0f, 0.0f};
        bool central = false;
        // Set when the op actually damaged a then-standing part. Only applied ops kick debris (a
        // killed part flies off with the very impulse that felled it, ADR-0029 §2); an op that
        // landed after its part was already gone is spent, and never doubles a chunk's momentum.
        bool applied = false;
    };

    // The canonical sort key for EXPLICIT ops (ADR-0029 §3: "sorted by (instance, part, op
    // bytes)") — defined in damage.cpp with the rest of the damage path.
    [[nodiscard]] static std::array<std::uint32_t, 9> op_key(const DamageOp& o) noexcept;

    // The support solve + the fracture body swap for ONE damaged instance (ADR-0029 §2) — the
    // heart of M8.3, defined in damage.cpp. Called only when the instance's membership changed
    // this tick (a part died). `ops` is the tick's full applied-op sequence, already canonical —
    // islands read their kick-off impulses from it.
    void fracture_instance(std::uint32_t instance_index,
                           std::span<const DamageOp> ops,
                           physics::PhysicsWorld& world);

    // A debris body's lifetime phase (M8.5). Falling = live and moving; Settled = it came to rest
    // (a physics Slept), now lingering; Frozen = reclaimed — body destroyed and any owned compound
    // unregistered, the roster record kept so debris ids never shift. Only used when the lifecycle
    // is enabled; a debris otherwise stays Falling forever (append-only, the M8.3/8.4 behaviour).
    enum class DebrisPhase : std::uint8_t { Falling = 0, Settled = 1, Frozen = 2 };

    // One detached island, now a free dynamic body. Member part ids live in the shared CSR pool
    // `debris_part_pool` (rows [first_part, first_part + part_count), ascending) — flat storage so
    // the roster stays SoA-cheap however many islands a collapse produces.
    struct Debris {
        physics::BodyId body{};
        std::uint32_t instance = 0;
        std::uint32_t first_part = 0;
        std::uint32_t part_count = 0;
        // The runtime compound a MULTI-part island owns — freed when this debris is frozen (M8.5).
        // Invalid for a single-part (k=1) hull debris, which shares the pattern's hull: there is
        // nothing to free (unregistering that shared hull would dangle every sibling that uses it).
        physics::CompoundId compound{};
        DebrisPhase phase = DebrisPhase::Falling;
        std::uint64_t settled_tick = 0; // the update() tick it came to rest — age = tick - this
    };

    std::vector<Pattern> patterns;
    std::vector<Instance> instances;
    std::vector<DamageCall> pending_damage;
    std::vector<Debris> debris;
    std::vector<std::uint32_t> debris_part_pool;

    // M8.5 lifecycle state: the budget policy (default-off ⇒ nothing is ever reclaimed, so 8.2–8.4
    // scenes stay byte-identical) and the update() tick counter that debris age is measured in.
    LifecycleConfig lifecycle{};
    std::uint64_t tick = 0;

    // The M8.5 debris lifetime & budget pass (lifecycle.cpp): reclaim settled debris past their
    // linger and hold the live debris population under the cap. Runs in update()'s sequential tail,
    // only when lifecycle.enabled — so it can never perturb the parallel step's cross-worker hash.
    void enforce_debris_budget(physics::PhysicsWorld& world);
    // Freeze debris #d: destroy its body, unregister its owned compound (a k>1 island only), keep
    // the roster record. Body destroyed FIRST so the compound is unreferenced before we free it.
    // Idempotent — freezing an already-frozen debris is a no-op.
    void freeze_debris(std::size_t debris_index, physics::PhysicsWorld& world);
    // How many debris bodies are still live (not yet frozen) — the quantity the cap bounds.
    [[nodiscard]] std::size_t live_debris_count() const noexcept;
    // A debris's size proxy for the eviction score: Σ its member parts' cooked volumes.
    [[nodiscard]] float debris_size(const Debris& d) const noexcept;

    // The M8.4 event fan-out: update() pushes PartDamaged/PartDied/IslandDetached/DebrisSettled
    // here and publish()es once, at the tick's end; consumers read events.view() until the next
    // update(). Double-buffered so a consumer can hold the span across the whole post-tick fan-out.
    core::EventChannel<DestructionEvent> events;

    // BodyId → instance index, for turning a contact event's body back into a destructible — a
    // SORTED vector (by body slot index) + binary search, not a hash map: lookups are log n, and
    // there is no iteration-order hazard to even think about. Holds only the instances' standing
    // compound bodies (debris deliberately not damageable in v1 — deferred to m8.5 with lifetime);
    // maintained by spawn and by every body swap. Slot indices are unique among LIVE bodies, so
    // the index is the key and the stored generation guards against stale lookups.
    std::vector<std::pair<physics::BodyId, std::uint32_t>> body_to_instance;

    void map_insert(physics::BodyId body, std::uint32_t instance) {
        const auto pos = std::lower_bound(body_to_instance.begin(),
                                          body_to_instance.end(),
                                          body.index,
                                          [](const std::pair<physics::BodyId, std::uint32_t>& e,
                                             std::uint32_t idx) { return e.first.index < idx; });
        body_to_instance.insert(pos, {body, instance});
    }

    void map_erase(physics::BodyId body) {
        const auto pos = std::lower_bound(body_to_instance.begin(),
                                          body_to_instance.end(),
                                          body.index,
                                          [](const std::pair<physics::BodyId, std::uint32_t>& e,
                                             std::uint32_t idx) { return e.first.index < idx; });
        if (pos != body_to_instance.end() && pos->first == body) {
            body_to_instance.erase(pos);
        }
    }

    // The instance whose standing body this is, or kInvalidPartIndex-style miss (0xFFFFFFFF) for
    // anything else (debris, the ground, a stale id from before a swap).
    [[nodiscard]] std::uint32_t instance_of(physics::BodyId body) const noexcept {
        const auto pos = std::lower_bound(body_to_instance.begin(),
                                          body_to_instance.end(),
                                          body.index,
                                          [](const std::pair<physics::BodyId, std::uint32_t>& e,
                                             std::uint32_t idx) { return e.first.index < idx; });
        if (pos != body_to_instance.end() && pos->first == body) {
            return pos->second;
        }
        return 0xFFFFFFFFu;
    }
};

} // namespace rime::destruction
