// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/physics/world.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "aabb_tree.hpp"
#include "narrowphase.hpp"
#include "rime/core/containers/handle.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/shape.hpp"
#include "solver.hpp"

// The M7.1–M7.4 world: a data-oriented body pool, a semi-implicit Euler integrator, a dual
// dynamic-AABB-tree broadphase (M7.2), a GJK/EPA/clipping narrowphase with a persistent warm-start
// cache (M7.3), and the sequential-impulse contact solver with its NGS position pass (M7.4). One
// step() runs the whole pipeline; islands and the parallel step arrive at M7.5.
namespace rime::physics {
namespace {

// 1/x, but 0 for a non-positive x — the "infinite mass/inertia" convention (static/kinematic bodies
// and degenerate shapes get zero inverse mass, so a force produces no acceleration).
[[nodiscard]] float inv_or_zero(float x) noexcept {
    return x > 0.0f ? 1.0f / x : 0.0f;
}

[[nodiscard]] bool is_static(std::uint8_t motion) noexcept {
    return motion == static_cast<std::uint8_t>(MotionType::Static);
}

} // namespace

// The body pool + broadphase. Bodies live in packed, parallel SoA arrays indexed by a *dense* index
// in [0, count); a generational slot table maps a stable BodyId to the current dense index.
// Destroying a body swap-removes it so the arrays stay dense, and bumps the freed slot's generation
// so a stale BodyId is detected. Each body also owns a broadphase proxy in one of two AABB trees: a
// `static_tree` (bodies that never move) and a `dynamic_tree` (dynamic/kinematic). Splitting them
// means the static world is built once and only movers are ever re-inserted — and it makes "both
// static" pairs (which we don't want) impossible by construction.
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
    std::vector<float> friction;              // Coulomb μ (M7.4 solver material)
    std::vector<float> restitution;           // bounciness e (M7.4 solver material)
    std::vector<ShapeDesc> shape;             // needed to recompute the world AABB after moving
    std::vector<std::int32_t> proxy;          // this body's node in its broadphase tree
    std::vector<std::uint32_t> dense_to_slot; // dense index → owning slot (for swap-remove fixup)

    core::Vec3 gravity{0.0f, -9.81f, 0.0f};

    AabbTree static_tree;
    AabbTree dynamic_tree;

    // Persistent contact cache (M7.3): last tick's manifolds keyed by canonical pair id, so a
    // surviving contact's accumulated impulses carry forward by matching feature id (warm
    // starting). Ordering matters (M7.4): step() commits this cache from the manifolds AFTER the
    // velocity solve wrote its converged impulses into them — committing pre-solve manifolds
    // would persist last tick's warm-start copies forever and the solver would always start
    // cold. The solverless public compute_contacts() commits what it warm-started (zeros on a
    // fresh world), which keeps the narrowphase tests' cache semantics unchanged.
    // `warm_started_last` records the last contact build's feature-id match count — the
    // narrowphase test's witness, and from M7.4 the warm-start hit rate.
    mutable std::unordered_map<std::uint64_t, ManifoldCacheEntry> contact_cache;
    mutable std::uint32_t warm_started_last = 0;

    [[nodiscard]] std::size_t count() const noexcept { return position.size(); }

    // The collision half of a tick, shared by step() and the public compute_contacts() (defined
    // below, after the struct). Split in two so step() can run the solver in between: build the
    // warm-started manifolds, solve (mutating their impulses), then commit the cache from the
    // SOLVED manifolds.
    void compute_pairs(std::vector<Pair>& out) const;
    void build_contacts(std::vector<Manifold>& out) const;
    void commit_contact_cache(const std::vector<Manifold>& manifolds) const;

    [[nodiscard]] AabbTree& tree_for(std::uint8_t m) noexcept {
        return is_static(m) ? static_tree : dynamic_tree;
    }

    [[nodiscard]] const AabbTree& tree_for(std::uint8_t m) const noexcept {
        return is_static(m) ? static_tree : dynamic_tree;
    }

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

void PhysicsWorld::Impl::compute_pairs(std::vector<Pair>& out) const {
    out.clear();

    // Every non-static body is a "mover": query both trees with its fat box. A pair (a,b) is packed
    // as (min_slot << 32 | max_slot) — a key that sorts by (a,b) — so sorting then de-duplicating
    // yields the canonical, deterministic pair set. Dynamic–dynamic pairs are found twice (each end
    // queries) and collapse to one; both-static pairs never appear because static bodies never
    // query.
    std::vector<std::uint64_t> keys;
    const std::size_t n = count();
    for (std::size_t i = 0; i < n; ++i) {
        if (is_static(motion[i])) {
            continue;
        }
        const std::uint32_t self = dense_to_slot[i];
        const Aabb& fat = dynamic_tree.proxy_aabb(proxy[i]);
        const auto add = [&](std::uint32_t other) {
            if (other == self) {
                return;
            }
            const std::uint32_t a = std::min(self, other);
            const std::uint32_t b = std::max(self, other);
            keys.push_back((static_cast<std::uint64_t>(a) << 32) | b);
        };
        dynamic_tree.query(fat, add);
        static_tree.query(fat, add);
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    out.reserve(keys.size());
    for (const std::uint64_t key : keys) {
        const auto a = static_cast<std::uint32_t>(key >> 32);
        const auto b = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
        out.push_back(Pair{BodyId{a, slots[a].generation}, BodyId{b, slots[b].generation}});
    }
}

void PhysicsWorld::Impl::build_contacts(std::vector<Manifold>& out) const {
    out.clear();

    // Broadphase first: the candidate pairs are canonical (a.index < b.index), deterministically
    // sorted, and already exclude both-static pairs. A fat-AABB overlap is only a *candidate* — the
    // narrowphase below confirms (or rejects) each with exact geometry.
    std::vector<Pair> pairs;
    compute_pairs(pairs);

    std::uint32_t warm = 0;
    for (const Pair& pr : pairs) {
        const std::uint32_t da = dense_of(pr.a);
        const std::uint32_t db = dense_of(pr.b);
        if (da == core::kInvalidSlotIndex || db == core::kInvalidSlotIndex) {
            continue; // impossible for a pair compute_pairs just returned, but stay defensive
        }

        Manifold m;
        m.a = pr.a;
        m.b = pr.b;
        // collide_shapes canonicalizes by ShapeType internally and flips the normal to match, so
        // passing the bodies in slot order yields a normal oriented from a toward b (contact.hpp).
        if (!collide_shapes(shape[da],
                            position[da],
                            orientation[da],
                            shape[db],
                            position[db],
                            orientation[db],
                            m) ||
            m.count == 0) {
            continue; // fat boxes overlapped but the exact shapes do not — a broadphase miss
        }

        // Warm-start from last tick's cache: match points by feature id and carry their
        // accumulated impulses into the fresh manifold. The stored generations guard against a
        // recycled slot inheriting a dead pair's impulses.
        const std::uint64_t key =
            (static_cast<std::uint64_t>(pr.a.index) << 32) | static_cast<std::uint64_t>(pr.b.index);
        const auto it = contact_cache.find(key);
        if (it != contact_cache.end() && it->second.gen_a == pr.a.generation &&
            it->second.gen_b == pr.b.generation) {
            warm += warm_start_from(it->second, m);
        }
        out.push_back(m);
    }
    warm_started_last = warm;
}

void PhysicsWorld::Impl::commit_contact_cache(const std::vector<Manifold>& manifolds) const {
    // Rebuild the persistent cache from this tick's manifolds — in step() they are the SOLVED
    // manifolds, so the impulses that carry to next tick are the converged ones (the whole point
    // of warm starting). Keyed by the broadphase pair key ((slot_a << 32) | slot_b). A flat hash
    // map is plenty here — lookups are per-pair, nothing ever iterates it, so its unordered-ness
    // cannot leak into simulation order (the determinism contract).
    std::unordered_map<std::uint64_t, ManifoldCacheEntry> next;
    next.reserve(manifolds.size());
    for (const Manifold& m : manifolds) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(m.a.index) << 32) | static_cast<std::uint64_t>(m.b.index);
        next.emplace(key, make_cache_entry(key, m.a.generation, m.b.generation, m));
    }
    contact_cache.swap(next);
}

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
    const core::Quat orient = core::normalize(d.orientation);

    // Insert the broadphase proxy tagged with the stable slot id, so a query returns slot ids
    // directly (mappable back to a BodyId without touching the dense arrays).
    const Aabb tight = compute_aabb(d.shape, d.position, orient);
    const auto motion8 = static_cast<std::uint8_t>(d.motion);
    const std::int32_t proxy = p.tree_for(motion8).create_proxy(tight, slot);

    const auto dense = static_cast<std::uint32_t>(p.count());
    p.position.push_back(d.position);
    p.orientation.push_back(orient);
    p.linear_velocity.push_back(d.linear_velocity);
    p.angular_velocity.push_back(d.angular_velocity);
    p.inv_mass.push_back(im);
    p.inv_inertia.push_back(ii);
    p.motion.push_back(motion8);
    p.linear_damping.push_back(d.linear_damping);
    p.angular_damping.push_back(d.angular_damping);
    p.gravity_factor.push_back(d.gravity_factor);
    p.friction.push_back(d.friction);
    p.restitution.push_back(d.restitution);
    p.shape.push_back(d.shape);
    p.proxy.push_back(proxy);
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

    // Drop this body's broadphase proxy first (its tree is chosen by the current motion type).
    p.tree_for(p.motion[d]).destroy_proxy(p.proxy[d]);

    const auto last = static_cast<std::uint32_t>(p.count() - 1);
    if (d != last) {
        // Swap-remove: move the last body into the hole so the arrays stay dense. The moved body's
        // proxy node is untouched (it is keyed by the stable slot id), just relocated in the array.
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
        p.friction[d] = p.friction[last];
        p.restitution[d] = p.restitution[last];
        p.shape[d] = p.shape[last];
        p.proxy[d] = p.proxy[last];
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
    p.friction.pop_back();
    p.restitution.pop_back();
    p.shape.pop_back();
    p.proxy.pop_back();
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
    const auto is_dynamic = [&](std::size_t i) {
        return p.motion[i] == static_cast<std::uint8_t>(MotionType::Dynamic);
    };

    // The M7.4 tick is the classic sequential-impulse pipeline. Its one non-obvious property is
    // WHERE the position integration sits: semi-implicit (symplectic) Euler advances velocity
    // first, the solver then corrects exactly those velocities, and only afterwards do positions
    // integrate — so a resting body's gravity is cancelled by its contact impulse in the same tick
    // it was applied, and the body never accumulates downward motion it must later undo. Every
    // loop below runs in dense-index or canonical-pair order with fixed iteration counts: the
    // tick is a pure function of its inputs (the determinism contract, ADR-0026).

    // ---- 1. Integrate velocities (not positions yet). Gravity is an acceleration
    // (mass-independent) so it enters the velocity directly; damping is the implicit 1/(1+c·dt)
    // form, unconditionally stable (never flips a velocity's sign for large c·dt). Static bodies
    // hold still; kinematic motion is pushed in at M7.6. The gyroscopic term ω×Iω stays dropped
    // in v1 (ADR-0026; explicit integration of it is famously unstable).
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_dynamic(i)) {
            continue;
        }
        core::Vec3 v = p.linear_velocity[i] + p.gravity * (p.gravity_factor[i] * dt);
        v *= 1.0f / (1.0f + p.linear_damping[i] * dt);
        p.linear_velocity[i] = v;
        p.angular_velocity[i] *= 1.0f / (1.0f + p.angular_damping[i] * dt);
    }

    // ---- 2. Detect contacts: broadphase pairs → narrowphase manifolds, warm-started from last
    // tick's cache. The cache is deliberately NOT committed here — warm starting only works if
    // what carries forward are the SOLVED impulses, so the commit waits until after the velocity
    // solve (stage 7).
    std::vector<Manifold> manifolds;
    p.build_contacts(manifolds);

    // ---- 3. Prepare + warm-start the contact constraints (solver.hpp). Pairs with no dynamic
    // participant (kinematic vs kinematic/static) are skipped — an impulse could never move
    // either end, so there is nothing to solve.
    SolverBodies bodies{p.position.data(),
                        p.orientation.data(),
                        p.linear_velocity.data(),
                        p.angular_velocity.data(),
                        p.inv_mass.data(),
                        p.inv_inertia.data(),
                        p.friction.data(),
                        p.restitution.data()};
    std::vector<ContactConstraint> constraints;
    constraints.reserve(manifolds.size());
    for (std::size_t mi = 0; mi < manifolds.size(); ++mi) {
        const Manifold& m = manifolds[mi];
        const std::uint32_t da = p.dense_of(m.a);
        const std::uint32_t db = p.dense_of(m.b);
        if (da == core::kInvalidSlotIndex || db == core::kInvalidSlotIndex) {
            continue; // defensive: build_contacts only emits live pairs
        }
        if (p.inv_mass[da] + p.inv_mass[db] <= 0.0f) {
            continue; // no dynamic member — immovable pair, nothing solvable
        }
        constraints.push_back(
            prepare_contact_constraint(bodies, m, da, db, static_cast<std::uint32_t>(mi)));
    }
    warm_start(bodies, constraints);

    // ---- 4. Velocity solve: fixed sequential-impulse sweeps, then persist the converged
    // accumulators into the manifolds (the cache commit below carries them to next tick).
    solve_velocities(bodies, constraints, kVelocityIterations);
    store_impulses(constraints, manifolds);

    // ---- 5. Integrate positions with the POST-solve velocities (the symplectic pairing).
    // Orientation integrates by q̇ = ½·ω·q (ω as the pure quaternion (ωₓ,ω_y,ω_z,0)) then
    // renormalizes to stay on the unit sphere (docs/math/rigid-body-dynamics.md §3).
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_dynamic(i)) {
            continue;
        }
        p.position[i] += p.linear_velocity[i] * dt;
        const core::Vec3 w = p.angular_velocity[i];
        const core::Quat q = p.orientation[i];
        const core::Quat omega{w.x, w.y, w.z, 0.0f};
        p.orientation[i] = core::normalize(q + (omega * q) * (0.5f * dt));
    }

    // ---- 6. NGS position pass: recover residual penetration by adjusting poses only — never
    // velocities. That restriction is the load-bearing design choice (NGS, not Baumgarte): no
    // kinetic energy enters, so deep overlaps resolve without launching anything (solver.hpp and
    // docs/math/sequential-impulse.md carry the full argument).
    solve_positions(bodies, constraints, kPositionIterations);

    // ---- 7. Commit the contact cache from the SOLVED manifolds, and re-bound each mover's
    // broadphase proxy around its final pose (a no-op while the body stays inside its fat AABB,
    // which is the common case — that is the whole point of the fat margin).
    p.commit_contact_cache(manifolds);
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_dynamic(i)) {
            continue;
        }
        const Aabb tight = compute_aabb(p.shape[i], p.position[i], p.orientation[i]);
        p.dynamic_tree.move_proxy(p.proxy[i], tight);
    }
}

void PhysicsWorld::compute_pairs(std::vector<Pair>& out) const {
    impl_->compute_pairs(out);
}

bool PhysicsWorld::broadphase_aabb(BodyId id, Aabb& out) const {
    const std::uint32_t d = impl_->dense_of(id);
    if (d == core::kInvalidSlotIndex) {
        return false;
    }
    out = impl_->tree_for(impl_->motion[d]).proxy_aabb(impl_->proxy[d]);
    return true;
}

bool PhysicsWorld::validate_broadphase() const {
    return impl_->dynamic_tree.validate() && impl_->static_tree.validate();
}

void PhysicsWorld::compute_contacts(std::vector<Manifold>& out) const {
    // The test/inspection seam: same collision path as step(), but with no solver in between the
    // build and the commit — the cache carries forward whatever the manifolds were warm-started
    // with (zeros on a fresh world, or the last step()'s solved impulses, unchanged either way).
    impl_->build_contacts(out);
    impl_->commit_contact_cache(out);
}

std::uint32_t PhysicsWorld::contacts_warm_started_last() const noexcept {
    return impl_->warm_started_last;
}

} // namespace rime::physics
