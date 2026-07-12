// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/physics/world.hpp"

#include <cstdint>
#include <vector>

#include "rime/core/containers/handle.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/physics/shape.hpp"

// The M7.1 world: a data-oriented body pool + a semi-implicit Euler integrator. No collision yet —
// this brick proves the storage (generational, churn-safe) and the integration math (ballistic
// trajectories, deterministic) that everything above is built on.
namespace rime::physics {
namespace {

// 1/x, but 0 for a non-positive x — the "infinite mass/inertia" convention (static/kinematic bodies
// and degenerate shapes get zero inverse mass, so a force produces no acceleration).
[[nodiscard]] float inv_or_zero(float x) noexcept {
    return x > 0.0f ? 1.0f / x : 0.0f;
}

} // namespace

// The body pool. Bodies live in packed, parallel SoA arrays indexed by a *dense* index in
// [0, count); a generational slot table maps a stable BodyId to the current dense index. Destroying
// a body swap-removes it (move the last body into the hole) so the arrays stay dense and
// cache-friendly, and bumps the freed slot's generation so any lingering BodyId is detected as
// stale. This is exactly the churn pattern M8's detach storm needs: create/destroy is O(1) and
// never invalidates the *other* bodies' ids. Iteration is always in dense-index order — one of the
// levers that makes the step deterministic.
struct PhysicsWorld::Impl {
    struct Slot {
        std::uint32_t dense = core::kInvalidSlotIndex; // kInvalidSlotIndex ⇒ free
        std::uint32_t generation = 0;
    };

    std::vector<Slot> slots; // indexed by BodyId::index
    std::vector<std::uint32_t> free_slots;

    // Dense SoA state (all the same length == body count).
    std::vector<core::Vec3> position;
    std::vector<core::Quat> orientation;
    std::vector<core::Vec3> linear_velocity;
    std::vector<core::Vec3> angular_velocity;
    std::vector<float> inv_mass;         // 0 for static/kinematic
    std::vector<core::Vec3> inv_inertia; // body-space diagonal inverse inertia (M7.4 solver)
    std::vector<std::uint8_t> motion;
    std::vector<float> linear_damping;
    std::vector<float> angular_damping;
    std::vector<float> gravity_factor;
    std::vector<std::uint32_t> dense_to_slot; // dense index → owning slot (for swap-remove fixup)

    core::Vec3 gravity{0.0f, -9.81f, 0.0f};

    [[nodiscard]] std::size_t count() const noexcept { return position.size(); }

    // Resolve a BodyId to its dense index, or kInvalidSlotIndex if the id is stale/unknown.
    [[nodiscard]] std::uint32_t dense_of(BodyId id) const noexcept {
        if (id.index >= slots.size()) {
            return core::kInvalidSlotIndex;
        }
        const Slot& s = slots[id.index];
        if (s.generation != id.generation || s.dense == core::kInvalidSlotIndex) {
            return core::kInvalidSlotIndex;
        }
        return s.dense;
    }
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() = default;

BodyId PhysicsWorld::create_body(const BodyDesc& d) {
    Impl& p = *impl_;

    // Claim a slot (reuse a freed one to keep the table compact; its generation was already
    // bumped).
    std::uint32_t slot;
    if (!p.free_slots.empty()) {
        slot = p.free_slots.back();
        p.free_slots.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(p.slots.size());
        p.slots.push_back(Impl::Slot{});
    }

    const bool dynamic = d.motion == MotionType::Dynamic;
    const MassProperties mp = compute_mass_properties(d.shape, d.mass > 0.0f ? d.mass : 1.0f);
    const float im = dynamic ? inv_or_zero(mp.mass) : 0.0f;
    const core::Vec3 ii = dynamic ? core::Vec3{inv_or_zero(mp.inertia_diagonal.x),
                                               inv_or_zero(mp.inertia_diagonal.y),
                                               inv_or_zero(mp.inertia_diagonal.z)}
                                  : core::Vec3{0.0f, 0.0f, 0.0f};

    const auto dense = static_cast<std::uint32_t>(p.count());
    p.position.push_back(d.position);
    p.orientation.push_back(core::normalize(d.orientation));
    p.linear_velocity.push_back(d.linear_velocity);
    p.angular_velocity.push_back(d.angular_velocity);
    p.inv_mass.push_back(im);
    p.inv_inertia.push_back(ii);
    p.motion.push_back(static_cast<std::uint8_t>(d.motion));
    p.linear_damping.push_back(d.linear_damping);
    p.angular_damping.push_back(d.angular_damping);
    p.gravity_factor.push_back(d.gravity_factor);
    p.dense_to_slot.push_back(slot);

    p.slots[slot].dense = dense;
    return BodyId{slot, p.slots[slot].generation};
}

void PhysicsWorld::destroy_body(BodyId id) {
    Impl& p = *impl_;
    const std::uint32_t d = p.dense_of(id);
    if (d == core::kInvalidSlotIndex) {
        return; // stale/unknown — safe no-op
    }
    const auto last = static_cast<std::uint32_t>(p.count() - 1);
    if (d != last) {
        // Swap-remove: move the last body into the hole so the arrays stay dense.
        p.position[d] = p.position[last];
        p.orientation[d] = p.orientation[last];
        p.linear_velocity[d] = p.linear_velocity[last];
        p.angular_velocity[d] = p.angular_velocity[last];
        p.inv_mass[d] = p.inv_mass[last];
        p.inv_inertia[d] = p.inv_inertia[last];
        p.motion[d] = p.motion[last];
        p.linear_damping[d] = p.linear_damping[last];
        p.angular_damping[d] = p.angular_damping[last];
        p.gravity_factor[d] = p.gravity_factor[last];
        const std::uint32_t moved_slot = p.dense_to_slot[last];
        p.dense_to_slot[d] = moved_slot;
        p.slots[moved_slot].dense = d; // the moved body now lives at d
    }
    p.position.pop_back();
    p.orientation.pop_back();
    p.linear_velocity.pop_back();
    p.angular_velocity.pop_back();
    p.inv_mass.pop_back();
    p.inv_inertia.pop_back();
    p.motion.pop_back();
    p.linear_damping.pop_back();
    p.angular_damping.pop_back();
    p.gravity_factor.pop_back();
    p.dense_to_slot.pop_back();

    // Free the slot and bump its generation so old ids referencing it read as dead.
    p.slots[id.index].dense = core::kInvalidSlotIndex;
    ++p.slots[id.index].generation;
    p.free_slots.push_back(id.index);
}

bool PhysicsWorld::get_body_state(BodyId id, BodyState& out) const {
    const std::uint32_t d = impl_->dense_of(id);
    if (d == core::kInvalidSlotIndex) {
        return false;
    }
    out.position = impl_->position[d];
    out.orientation = impl_->orientation[d];
    out.linear_velocity = impl_->linear_velocity[d];
    out.angular_velocity = impl_->angular_velocity[d];
    return true;
}

bool PhysicsWorld::is_alive(BodyId id) const noexcept {
    return impl_->dense_of(id) != core::kInvalidSlotIndex;
}

std::size_t PhysicsWorld::body_count() const noexcept {
    return impl_->count();
}

void PhysicsWorld::set_gravity(core::Vec3 g) noexcept {
    impl_->gravity = g;
}

core::Vec3 PhysicsWorld::gravity() const noexcept {
    return impl_->gravity;
}

void PhysicsWorld::step(float dt) {
    Impl& p = *impl_;
    const std::size_t n = p.count();

    // Semi-implicit (symplectic) Euler: advance velocity first, THEN position with the new
    // velocity. Unlike explicit Euler this is energy-stable for oscillatory systems — the reason it
    // is the standard game-physics integrator (derivation: docs/math/rigid-body-dynamics.md). Only
    // dynamic bodies integrate; static/kinematic bodies hold still (kinematic motion is pushed in
    // at M7.6). Iteration is in dense-index order, so the result is independent of anything but the
    // inputs.
    for (std::size_t i = 0; i < n; ++i) {
        if (p.motion[i] != static_cast<std::uint8_t>(MotionType::Dynamic)) {
            continue;
        }

        // Linear: gravity is an acceleration (mass-independent), so it enters the velocity
        // directly. External forces (÷ mass) arrive at M7.3. Damping is the implicit 1/(1+c·dt)
        // form, which is unconditionally stable (never flips the velocity's sign for large c·dt).
        core::Vec3 v = p.linear_velocity[i] + p.gravity * (p.gravity_factor[i] * dt);
        v *= 1.0f / (1.0f + p.linear_damping[i] * dt);
        p.linear_velocity[i] = v;
        p.position[i] += v * dt;

        // Angular: no torque in M7.1, so angular velocity only decays by damping. Integrate the
        // orientation quaternion by q̇ = ½·ω·q (ω as the pure quaternion (ωₓ,ω_y,ω_z,0)), then
        // renormalize to stay on the unit sphere. (Full ω̇ = I⁻¹(τ − ω×Iω) lands with the solver.)
        core::Vec3 w = p.angular_velocity[i] * (1.0f / (1.0f + p.angular_damping[i] * dt));
        p.angular_velocity[i] = w;
        const core::Quat q = p.orientation[i];
        const core::Quat omega{w.x, w.y, w.z, 0.0f};
        const core::Quat dq = (omega * q) * (0.5f * dt);
        p.orientation[i] = core::normalize(q + dq);
    }
}

} // namespace rime::physics
