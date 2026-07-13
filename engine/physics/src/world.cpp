// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/physics/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "aabb_tree.hpp"
#include "islands.hpp"
#include "narrowphase.hpp"
#include "rime/core/containers/handle.hpp"
#include "rime/core/hash.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/shape.hpp"
#include "solver.hpp"

// The M7.1–M7.5 world: a data-oriented body pool, a semi-implicit Euler integrator, a dual
// dynamic-AABB-tree broadphase (M7.2), a GJK/EPA/clipping narrowphase with a persistent warm-start
// cache (M7.3), the sequential-impulse contact solver with its NGS position pass (M7.4), and the
// island partition + sleeping + job-system parallel step (M7.5). One step() runs the whole
// pipeline; it is deterministic to the bit for any worker-thread count (proven by world_hash()).
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
    std::vector<float> friction;      // Coulomb μ (M7.4 solver material)
    std::vector<float> restitution;   // bounciness e (M7.4 solver material)
    std::vector<float> sleep_timer;   // seconds spent below the sleep thresholds (M7.5)
    std::vector<std::uint8_t> asleep; // 1 ⇒ deactivated: skipped by integration + solve (M7.5)
    std::vector<ShapeDesc> shape;     // needed to recompute the world AABB after moving
    std::vector<std::int32_t> proxy;  // this body's node in its broadphase tree
    std::vector<std::uint32_t> dense_to_slot; // dense index → owning slot (for swap-remove fixup)

    core::Vec3 gravity{0.0f, -9.81f, 0.0f};

    AabbTree static_tree;
    AabbTree dynamic_tree;

    // Islands + the parallel step (M7.5). `jobs` is a BORROWED job system (the engine owns it; null
    // ⇒ solve islands sequentially — same result). `islands` is the reusable CSR partition rebuilt
    // each step; `islands_last_count` is its size, exposed as a determinism/behaviour witness.
    // `sleeping_enabled` gates deactivation (on by default). None of this is touched concurrently:
    // only the per-island solve runs on worker threads, and islands share no dynamic body.
    core::JobSystem* jobs = nullptr;
    bool sleeping_enabled = true;
    IslandSet islands;
    std::size_t islands_last_count = 0;

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
    p.sleep_timer.push_back(0.0f);
    p.asleep.push_back(0); // a freshly created body always starts awake
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
        p.sleep_timer[d] = p.sleep_timer[last];
        p.asleep[d] = p.asleep[last];
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
    p.sleep_timer.pop_back();
    p.asleep.pop_back();
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

    // The M7.5 tick keeps the M7.4 sequential-impulse pipeline but wraps it in ISLANDS and
    // SLEEPING. Its shape:
    //   integrate velocities (awake) → detect contacts → prepare constraints → PARTITION into
    //   islands → wake/skip islands → solve each island (job system) → update sleeping → commit the
    //   cache + refit moved proxies.
    // Two properties carry over from M7.4: WHERE position integration sits (symplectic — velocity
    // first, solver corrects it, then position follows, so a resting body's gravity is cancelled in
    // the same tick), and DETERMINISM (dense/canonical order, fixed iteration counts). M7.5 adds a
    // stronger determinism claim: the per-island solve is the ONLY parallel region, islands share
    // no dynamic body, and immovable bodies are read-only in the solver — so the tick is
    // bit-identical for any worker-thread count (ADR-0026; world_hash() is the witness).

    // ---- 1. Integrate velocities (not positions yet) for AWAKE dynamic bodies. Gravity is an
    // acceleration (mass-independent) so it enters velocity directly; damping is the implicit
    // 1/(1+c·dt) form, unconditionally stable. Asleep bodies are frozen — skipping them here is the
    // whole point of sleeping: a resting stack costs nothing. (Kinematic push-in is M7.6; the
    // gyroscopic ω×Iω term stays dropped, ADR-0026.)
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_dynamic(i) || p.asleep[i] != 0) {
            continue;
        }
        core::Vec3 v = p.linear_velocity[i] + p.gravity * (p.gravity_factor[i] * dt);
        v *= 1.0f / (1.0f + p.linear_damping[i] * dt);
        p.linear_velocity[i] = v;
        p.angular_velocity[i] *= 1.0f / (1.0f + p.angular_damping[i] * dt);
    }

    // ---- 2. Detect contacts: broadphase pairs → narrowphase manifolds, warm-started from last
    // tick's cache (committed post-solve at stage 8, so warm starts carry SOLVED impulses). Runs
    // for ALL bodies, asleep included: a new contact between a faller and a sleeping stack is
    // exactly how the stack learns to wake — stage 4 merges them into one island, stage 5
    // reactivates it.
    std::vector<Manifold> manifolds;
    p.build_contacts(manifolds);

    // ---- 3. Prepare the contact constraints (solver.hpp). prepare_contact_constraint reads the
    // post-gravity velocities to fix each restitution bias, so it must precede any warm-start
    // impulse (the M7.4 ordering, preserved). Pairs with no dynamic participant are skipped — an
    // impulse could never move either end.
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

    // ---- 4. Partition into ISLANDS (islands.hpp): connected components of the dynamic-body
    // contact graph. Every awake dynamic body lands in exactly one island (a contactless body is a
    // singleton); static/kinematic bodies are shared anchors, not island members.
    build_islands(n, p.motion, constraints, p.islands);
    p.islands_last_count = p.islands.island_count;
    const IslandSet& isl = p.islands;

    // Gather the constraints into island-contiguous order so each island solves a plain span (the
    // solver stays index-free). Within-island order is preserved, so an island's solve is
    // bit-identical to M7.4's flat solve restricted to that island's bodies — the flat solve's
    // other constraints never touched them, so interleaving them or not cannot change the result.
    std::vector<ContactConstraint> ordered(isl.constraints.size());
    for (std::size_t i = 0; i < isl.constraints.size(); ++i) {
        ordered[i] = constraints[isl.constraints[i]];
    }

    // ---- 5. Decide which islands are ACTIVE this tick. An island is active unless every one of
    // its bodies is still asleep; a single awake member (e.g. a body that just fell in and merged
    // at stage 4) reactivates the whole island, and reactivated bodies restart their sleep timer.
    // With sleeping disabled no body is ever asleep, so every island is active — same code path.
    std::vector<std::uint8_t> active(isl.island_count, 1);
    for (std::size_t k = 0; k < isl.island_count; ++k) {
        bool any_awake = false;
        for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
            if (p.asleep[isl.bodies[bi]] == 0) {
                any_awake = true;
                break;
            }
        }
        active[k] = any_awake ? std::uint8_t{1} : std::uint8_t{0};
        if (any_awake) {
            for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
                const std::uint32_t b = isl.bodies[bi];
                if (p.asleep[b] != 0) { // a neighbour is moving — rejoin the simulation
                    p.asleep[b] = 0;
                    p.sleep_timer[b] = 0.0f;
                }
            }
        }
    }

    // ---- 6. Solve the ACTIVE islands. Per island, the exact M7.4 sequence — warm-start, fixed
    // velocity iterations, store impulses, integrate positions with the post-solve velocities, then
    // the fixed NGS position iterations — runs strictly sequentially. Islands write disjoint
    // dynamic bodies (immovable anchors are read-only), so the job system runs them in parallel
    // with no locks and the result is identical to the sequential loop, bit for bit, at any thread
    // count.
    const auto solve_island = [&](std::size_t k) {
        if (active[k] == 0) {
            return; // wholly asleep — its bodies stay frozen
        }
        const std::uint32_t cb = isl.constraint_offsets[k];
        const std::uint32_t ce = isl.constraint_offsets[k + 1];
        const std::span<ContactConstraint> cs{ordered.data() + cb, ce - cb};

        warm_start(bodies, cs);
        solve_velocities(bodies, cs, kVelocityIterations);
        store_impulses(cs, manifolds); // writes only this island's manifold indices — disjoint

        // Integrate this island's dynamic bodies with their post-solve velocities. Orientation
        // integrates by q̇ = ½·ω·q then renormalizes (docs/math/rigid-body-dynamics.md §3).
        for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
            const std::uint32_t i = isl.bodies[bi];
            p.position[i] += p.linear_velocity[i] * dt;
            const core::Vec3 w = p.angular_velocity[i];
            const core::Quat q = p.orientation[i];
            const core::Quat omega{w.x, w.y, w.z, 0.0f};
            p.orientation[i] = core::normalize(q + (omega * q) * (0.5f * dt));
        }

        // NGS position pass: recover residual penetration by adjusting poses only — never
        // velocities (the deliberate not-Baumgarte choice; solver.hpp, ADR-0026).
        solve_positions(bodies, cs, kPositionIterations);
    };

    if (p.jobs != nullptr && isl.island_count > 1) {
        // Over-decompose to ~4 chunks per participant so work-stealing balances the (wildly uneven)
        // island sizes. Any chunking is correct — islands are independent — so the RESULT never
        // depends on the chunk size or the worker count, only the timing does.
        const std::size_t participants = p.jobs->participant_count();
        const std::size_t chunk = std::max<std::size_t>(
            1, (isl.island_count + participants * 4 - 1) / (participants * 4));
        p.jobs->parallel_for(isl.island_count, chunk, solve_island);
    } else {
        for (std::size_t k = 0; k < isl.island_count; ++k) {
            solve_island(k);
        }
    }

    // ---- 7. Update SLEEPING, measured on the POST-solve velocities (a resting body's pre-solve
    // velocity still holds the g·dt its contact just cancelled, so only now is it near rest). Each
    // active island's bodies accrue sleep time while below the thresholds and reset the instant
    // they exceed them; once EVERY member has rested past kTimeToSleep the whole island sleeps —
    // velocities zeroed, and from next tick skipped. Sequential and deterministic, so sleeping
    // never perturbs the cross-thread world hash.
    if (p.sleeping_enabled) {
        constexpr float lin2 = kLinearSleepThreshold * kLinearSleepThreshold;
        constexpr float ang2 = kAngularSleepThreshold * kAngularSleepThreshold;
        for (std::size_t k = 0; k < isl.island_count; ++k) {
            if (active[k] == 0) {
                continue;
            }
            bool island_can_sleep = true;
            for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
                const std::uint32_t i = isl.bodies[bi];
                const bool sleepy = core::dot(p.linear_velocity[i], p.linear_velocity[i]) < lin2 &&
                                    core::dot(p.angular_velocity[i], p.angular_velocity[i]) < ang2;
                p.sleep_timer[i] = sleepy ? p.sleep_timer[i] + dt : 0.0f;
                if (p.sleep_timer[i] < kTimeToSleep) {
                    island_can_sleep = false;
                }
            }
            if (island_can_sleep) {
                for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
                    const std::uint32_t i = isl.bodies[bi];
                    p.asleep[i] = 1;
                    p.linear_velocity[i] = core::Vec3{0.0f, 0.0f, 0.0f};
                    p.angular_velocity[i] = core::Vec3{0.0f, 0.0f, 0.0f};
                }
            }
        }
    }

    // ---- 8. Commit the contact cache from the SOLVED manifolds (closing the warm-start loop),
    // then refit the broadphase proxy of every body that actually moved — the dynamic members of
    // ACTIVE islands (which includes any body that only just went to sleep this tick). Asleep
    // islands did not move, so their proxies and the tree are left untouched; move_proxy mutates
    // the shared tree, so this stays sequential.
    p.commit_contact_cache(manifolds);
    for (std::size_t k = 0; k < isl.island_count; ++k) {
        if (active[k] == 0) {
            continue;
        }
        for (std::uint32_t bi = isl.body_offsets[k]; bi < isl.body_offsets[k + 1]; ++bi) {
            const std::uint32_t i = isl.bodies[bi];
            const Aabb tight = compute_aabb(p.shape[i], p.position[i], p.orientation[i]);
            p.dynamic_tree.move_proxy(p.proxy[i], tight);
        }
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

void PhysicsWorld::set_job_system(core::JobSystem* jobs) noexcept {
    // Borrowed, not owned — the engine constructs one job system and hands it to every subsystem.
    // Null restores the sequential island solve (identical result, just single-threaded).
    impl_->jobs = jobs;
}

void PhysicsWorld::set_sleeping_enabled(bool enabled) noexcept {
    impl_->sleeping_enabled = enabled;
    if (!enabled) {
        // Wake everything now so step() never skips a body while sleeping is off (and a body frozen
        // by an earlier sleep resumes integrating on the very next tick).
        std::fill(impl_->asleep.begin(), impl_->asleep.end(), std::uint8_t{0});
        std::fill(impl_->sleep_timer.begin(), impl_->sleep_timer.end(), 0.0f);
    }
}

bool PhysicsWorld::is_asleep(BodyId id) const noexcept {
    const std::uint32_t d = impl_->dense_of(id);
    return d != core::kInvalidSlotIndex && impl_->asleep[d] != 0;
}

void PhysicsWorld::wake_body(BodyId id) noexcept {
    const std::uint32_t d = impl_->dense_of(id);
    if (d == core::kInvalidSlotIndex) {
        return; // stale/unknown — safe no-op
    }
    impl_->asleep[d] = 0;
    impl_->sleep_timer[d] = 0.0f;
    // The rest of its island reactivates on the next step(): stage 5 sees an awake member and wakes
    // the others. (Waking the whole island here would need the island partition, which step()
    // owns.)
}

std::size_t PhysicsWorld::islands_last() const noexcept {
    return impl_->islands_last_count;
}

std::uint64_t PhysicsWorld::world_hash() const noexcept {
    // FNV-1a (core/hash.hpp) over the full motion state of every body, in dense order — a fast
    // exact equality fingerprint. Dense order is reproducible run to run (bodies are appended, and
    // only ever swap-removed by destroy_body, never reordered by a step), so two simulations that
    // issued the same calls hash identically iff every float matches bit for bit. That is exactly
    // the check the parallel step must pass at any worker count, and the hook netcode / replay
    // determinism reuses. Packed into a local array so the hash never depends on Vec3/Quat
    // alignment padding.
    const Impl& p = *impl_;
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    for (std::size_t i = 0; i < p.count(); ++i) {
        const std::array<float, 13> state = {
            p.position[i].x,
            p.position[i].y,
            p.position[i].z,
            p.orientation[i].x,
            p.orientation[i].y,
            p.orientation[i].z,
            p.orientation[i].w,
            p.linear_velocity[i].x,
            p.linear_velocity[i].y,
            p.linear_velocity[i].z,
            p.angular_velocity[i].x,
            p.angular_velocity[i].y,
            p.angular_velocity[i].z,
        };
        h = core::fnv1a_64(std::as_bytes(std::span<const float>{state}), h);
    }
    return h;
}

} // namespace rime::physics
