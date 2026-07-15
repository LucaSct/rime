// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "rime/assets/destructible_asset.hpp"
#include "rime/core/containers/handle.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/physics/body.hpp"

// The physics world is BORROWED — destruction registers geometry into it and creates the bodies
// that stand for destructibles, but never owns it (the engine owns the one PhysicsWorld).
// Forward-declared so this seam header stays free of the physics internals; only world.cpp pulls
// the full seam in.
namespace rime::physics {
class PhysicsWorld;
}

// DestructionWorld — the runtime home of destructibles (M8.2, ADR-0029). It turns a cooked
// `assets::DestructibleAsset` (the m8.1 fracture pattern) into standing physics: a **pattern** is
// registered ONCE (each part a convex hull, the whole a compound — ADR-0027/0028), and
// **instances** are stamped out of it cheaply, each ONE static compound body plus per-part runtime
// state (health, alive bits) — the substrate m8.3's damage/connectivity/fracture writes into. This
// is the "load, stand, bind" half of destruction; damage and the fracture body-swap are m8.3,
// budgets/lifetime are m8.5, and per-part render leaves land with the m8.6 sample (a body's
// `part_placement` is the hook).
//
// Destruction OWNS its physics bodies directly (create/destroy on the PhysicsWorld), not through
// PhysicsSync — the ECS `Collider` cannot name a hull/compound id (ADR-0029 §6). The module is
// removable (guardrail 2): nothing below it depends on it.
namespace rime::destruction {

// A registered fracture pattern's id — one per distinct destructible asset, the shape economy
// (register once, spawn many). Append-only in v1 (unregister is m8.5), so the generation stays 0.
struct PatternTag {};

using PatternId = core::Handle<PatternTag>;

// A standing instance's id. Append-only in v1 (despawn is m8.5).
struct InstanceTag {};

using InstanceId = core::Handle<InstanceTag>;

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

    // --- read-back -------------------------------------------------------------------------------
    [[nodiscard]] std::size_t pattern_count() const noexcept;
    [[nodiscard]] std::size_t instance_count() const noexcept;

    // The physics body standing for this instance's intact shape, or a null BodyId for an unknown
    // id.
    [[nodiscard]] physics::BodyId body_of(InstanceId instance) const noexcept;

    // The part count of a pattern / an instance (0 for an unknown id).
    [[nodiscard]] std::uint32_t part_count(PatternId pattern) const noexcept;
    [[nodiscard]] std::uint32_t instance_part_count(InstanceId instance) const noexcept;

    // Per-part runtime state — the damage substrate m8.3 fills in. On m8.2 every part is alive with
    // health 1.0; here so the state model and its accessors are settled before the damage logic
    // stands on them. False / 0 for an unknown instance or part index.
    [[nodiscard]] bool part_alive(InstanceId instance, std::uint32_t part) const noexcept;
    [[nodiscard]] float part_health(InstanceId instance, std::uint32_t part) const noexcept;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::destruction
