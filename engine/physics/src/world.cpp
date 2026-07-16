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
#include "compound.hpp"
#include "hull.hpp"
#include "islands.hpp"
#include "narrowphase.hpp"
#include "rime/core/containers/handle.hpp"
#include "rime/core/hash.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/shape.hpp"
#include "scene_query.hpp"
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

// Slop on the per-child AABB cull inside a compound pair (M7.12): a child pair is only handed to
// the exact narrowphase if the children's tight bounds overlap within this margin. One millimetre
// comfortably covers the NGS resting slop (5 mm keeps resting shapes genuinely overlapped, so
// this is belt-and-braces against a kissing contact being culled by a rounding ulp) while still
// culling essentially every genuinely separated child pair.
constexpr float kChildCullMargin = 1e-3f;

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
    std::vector<core::Vec3> inv_inertia; // inverse principal moments (M7.4 solver)
    // Rotation from the shape's principal-axis frame to the body frame (M7.11): identity for the
    // primitives, a hull's registration-time eigendecomposition otherwise. Composed with the
    // orientation wherever the inverse inertia is applied (solver.hpp).
    std::vector<core::Quat> inertia_principal;
    std::vector<std::uint8_t> motion;
    std::vector<float> linear_damping;
    std::vector<float> angular_damping;
    std::vector<float> gravity_factor;
    std::vector<float> friction;      // Coulomb μ (M7.4 solver material)
    std::vector<float> restitution;   // bounciness e (M7.4 solver material)
    std::vector<std::uint8_t> ccd;    // 1 ⇒ continuous collision: speculative contacts (M7.10)
    std::vector<float> sleep_timer;   // seconds spent below the sleep thresholds (M7.5)
    std::vector<std::uint8_t> asleep; // 1 ⇒ deactivated: skipped by integration + solve (M7.5)
    std::vector<ShapeDesc> shape;     // needed to recompute the world AABB after moving
    std::vector<std::int32_t> proxy;  // this body's node in its broadphase tree
    std::vector<std::uint32_t> dense_to_slot; // dense index → owning slot (for swap-remove fixup)

    core::Vec3 gravity{0.0f, -9.81f, 0.0f};

    // The hull store (M7.11, ADR-0027; unregister added M8.5). Geometry registered once via
    // register_hull, referenced by HullId from bodies' ShapeDescs AND from compound children. The
    // store is now a GENERATIONAL SLOT table (mirroring the body slots above): a freed slot bumps
    // its generation so stale ids read dead, and register_hull reuses freed slots LIFO so ids stay
    // a pure function of the call sequence (determinism). `hull_refs[i]` counts live references
    // (bodies + compound children); unregister_hull refuses while it is non-zero —
    // reject-if-referenced, so a hull a live body or compound names can never be pulled out from
    // under it (which is exactly why compound_child_hull can resolve by index alone). Parallel
    // arrays keep `hulls` contiguous for the hot-path span the narrowphase takes.
    std::vector<ConvexHull> hulls;
    std::vector<std::uint32_t> hull_generation; // per slot; bumped on unregister
    std::vector<std::uint8_t> hull_live;        // 1 = registered, 0 = freed slot awaiting reuse
    std::vector<std::uint32_t> hull_refs;       // live references (bodies + compound children)
    std::vector<std::uint32_t> hull_free_list;  // freed slots, LIFO — deterministic reuse

    // The compound store (M7.12, ADR-0028; unregister added M8.5): the same generational-slot
    // contract as the hull store. `compound_refs[i]` counts live BODIES using compound i (a
    // compound never nests another in v1); unregister_compound refuses while non-zero and, when it
    // does free a compound, releases that compound's reference on each of its child hulls.
    std::vector<CompoundShape> compounds;
    std::vector<std::uint32_t> compound_generation;
    std::vector<std::uint8_t> compound_live;
    std::vector<std::uint32_t> compound_refs;
    std::vector<std::uint32_t> compound_free_list;

    AabbTree static_tree;
    AabbTree dynamic_tree;

    // Islands + the parallel step (M7.5). `jobs` is a BORROWED job system (the engine owns it; null
    // ⇒ solve islands sequentially — same result). `islands` is the reusable CSR partition rebuilt
    // each step; its size is one of the witnesses recorded into `last_stats` below (M7.13).
    // `sleeping_enabled` gates deactivation (on by default). None of this is touched concurrently:
    // only the per-island solve runs on worker threads, and islands share no dynamic body.
    core::JobSystem* jobs = nullptr;
    bool sleeping_enabled = true;
    IslandSet islands;

    // The most recent step()'s instrument panel (M7.13, WorldStats). Populated in the sequential
    // tail as a pure read of the just-computed tick — so, like the event emission, it never touches
    // body state and never perturbs the world hash. islands_last() and contacts_warm_started_last()
    // read straight out of here (the numbers they always returned are now fields of one struct).
    WorldStats last_stats{};

    // Persistent contact cache (M7.3): last tick's manifolds keyed by contact REGION — the
    // canonical pair id plus, since M7.12, the compound child sub-pair (0 for plain pairs, whose
    // behaviour is unchanged) — so a surviving contact's accumulated impulses carry forward by
    // matching feature id (warm starting). Ordering matters (M7.4): step() commits this cache
    // from the manifolds AFTER the velocity solve wrote its converged impulses into them —
    // committing pre-solve manifolds would persist last tick's warm-start copies forever and the
    // solver would always start cold. The solverless public compute_contacts() commits what it
    // warm-started (zeros on a fresh world), which keeps the narrowphase tests' cache semantics
    // unchanged. `warm_started_last` records the last contact build's feature-id match count —
    // the narrowphase test's witness, and from M7.4 the warm-start hit rate.
    mutable std::unordered_map<ContactCacheKey, ManifoldCacheEntry, ContactCacheKeyHash>
        contact_cache;
    mutable std::uint32_t warm_started_last = 0;
    // The last contact build's broadphase candidate-pair count (M7.13 stats). Set alongside
    // warm_started_last in build_contacts, so both the step() and compute_contacts() paths keep it
    // current; step()'s tail copies it into last_stats.broadphase_pairs.
    mutable std::uint32_t broadphase_pairs_last = 0;

    // Simulation events (M7.9, events.hpp). step() emits contact and sleep events as a pure READ of
    // the just-solved state, in the sequential tail — so events never write body state, never
    // perturb the world hash, and stay clear of the parallel solve (TSan). Both are
    // DOUBLE-BUFFERED: the tick fills the `_back` buffer, then swaps it to `_front`, so the public
    // accessors expose a stable snapshot of the completed tick until the next step().
    std::vector<ContactEvent> contact_events_front;
    std::vector<ContactEvent> contact_events_back;
    std::vector<SleepEvent> sleep_events_front;
    std::vector<SleepEvent> sleep_events_back;

    // The began/persisted/ended witness: one record per contact REGION (pair + child sub-pair
    // since M7.12; one region per pair for plain bodies), keyed by (broadphase pair key, packed
    // child indices) and kept ASCENDING lexicographically by that key (manifolds already arrive
    // canonically ordered — pairs ascending, child sub-pairs ascending within a pair — so `cur`
    // is built sorted). A single linear merge of this tick's `cur` against last tick's `prev`
    // classifies every region and emits in canonical order — no hash map, no per-tick sort. The
    // two vectors are swapped each tick, reusing their capacity. Owning our own record (rather
    // than reading the warm-start cache) keeps the event lifecycle independent of the solver's
    // cache and lets an Ended event carry the region's last point/normal, which the cache does
    // not store.
    struct ContactRecord {
        std::uint64_t key = 0;      // (a.index << 32) | b.index
        std::uint32_t children = 0; // (child_a << 16) | child_b — the region within the pair
        BodyId a;
        BodyId b;
        core::Vec3 point{0.0f, 0.0f, 0.0f};
        core::Vec3 normal{0.0f, 1.0f, 0.0f};
        float normal_impulse = 0.0f; // summed over the manifold; meaningful for `cur` only
        float tangent_impulse = 0.0f;
        bool suppressed = false; // has a dynamic member but every one is asleep — present, no event
    };

    std::vector<ContactRecord> contact_cur;
    std::vector<ContactRecord> contact_prev;

    // Per-step scratch: `asleep` copied at the top of step(), diffed at the tail to find the tick's
    // sleep/wake transitions. Dense-indexed and stable through a step (no create/destroy mid-step).
    std::vector<std::uint8_t> asleep_snapshot;

    [[nodiscard]] std::size_t count() const noexcept { return position.size(); }

    // The collision half of a tick, shared by step() and the public compute_contacts() (defined
    // below, after the struct). Split in two so step() can run the solver in between: build the
    // warm-started manifolds, solve (mutating their impulses), then commit the cache from the
    // SOLVED manifolds.
    void compute_pairs(std::vector<Pair>& out) const;
    // `dt` > 0 enables M7.10 speculative CCD contacts for opted-in bodies; dt == 0 (the default,
    // used by the compute_contacts() inspection seam) builds only exact overlaps, unchanged from
    // M7.3.
    void build_contacts(std::vector<Manifold>& out, float dt = 0.0f) const;
    // The compound arm of build_contacts (M7.12): one pair whose side(s) are compounds, expanded
    // into child-vs-child sub-pairs, emitting one manifold per touching child pair. Defined below
    // beside build_contacts.
    void build_compound_contacts(const Pair& pr,
                                 std::uint32_t da,
                                 std::uint32_t db,
                                 const CompoundShape* comp_a,
                                 const CompoundShape* comp_b,
                                 float dt,
                                 std::vector<Manifold>& out,
                                 std::uint32_t& warm) const;
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

    // Resolve a shape's hull reference to its store entry — nullptr for primitives and for an
    // unresolvable id (null/foreign/stale/unregistered). The generation must match the slot's
    // current one and the slot must be live (M8.5 — a freed-then-reused slot has a bumped
    // generation, so an id from before the free reads dead). This is the ONE place a HullId turns
    // into geometry; everything under the seam takes the pointer from here (ADR-0027).
    [[nodiscard]] const ConvexHull* hull_of(const ShapeDesc& s) const noexcept {
        if (s.type != ShapeType::ConvexHull) {
            return nullptr;
        }
        if (s.hull.index >= hulls.size() || hull_generation[s.hull.index] != s.hull.generation ||
            hull_live[s.hull.index] == 0) {
            return nullptr;
        }
        return &hulls[s.hull.index];
    }

    // Resolve a shape's compound reference — hull_of's twin for the compound store (ADR-0028).
    [[nodiscard]] const CompoundShape* compound_of(const ShapeDesc& s) const noexcept {
        if (s.type != ShapeType::Compound) {
            return nullptr;
        }
        if (s.compound.index >= compounds.size() ||
            compound_generation[s.compound.index] != s.compound.generation ||
            compound_live[s.compound.index] == 0) {
            return nullptr;
        }
        return &compounds[s.compound.index];
    }

    // The hull store as a span — the form the compound helpers take (compound.hpp stays free of
    // world internals; a compound's hull children resolve against exactly this store).
    [[nodiscard]] std::span<const ConvexHull> hull_span() const noexcept {
        return {hulls.data(), hulls.size()};
    }

    // Tight world AABB of a posed shape, store-aware: the shape-only compute_aabb (aabb.hpp)
    // cannot see the stores, so hull bounds come from posing the stored vertices and compound
    // bounds from the union of the posed children (the ONE broadphase bound a compound owns —
    // model A of ADR-0028 keeps proxy↔body strictly 1:1).
    [[nodiscard]] Aabb
    aabb_of(const ShapeDesc& s, core::Vec3 pos, const core::Quat& q) const noexcept {
        if (const CompoundShape* c = compound_of(s); c != nullptr) {
            return compound_world_aabb(*c, hull_span(), pos, q);
        }
        const ConvexHull* h = hull_of(s);
        return h != nullptr ? hull_world_aabb(*h, pos, q) : compute_aabb(s, pos, q);
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

void PhysicsWorld::Impl::build_contacts(std::vector<Manifold>& out, float dt) const {
    out.clear();

    // Broadphase first: the candidate pairs are canonical (a.index < b.index), deterministically
    // sorted, and already exclude both-static pairs. A fat-AABB overlap is only a *candidate* — the
    // narrowphase below confirms (or rejects) each with exact geometry.
    std::vector<Pair> pairs;
    compute_pairs(pairs);
    broadphase_pairs_last = static_cast<std::uint32_t>(pairs.size());

    std::uint32_t warm = 0;
    for (const Pair& pr : pairs) {
        const std::uint32_t da = dense_of(pr.a);
        const std::uint32_t db = dense_of(pr.b);
        if (da == core::kInvalidSlotIndex || db == core::kInvalidSlotIndex) {
            continue; // impossible for a pair compute_pairs just returned, but stay defensive
        }

        // A pair with a compound on either side expands into child-vs-child sub-pairs and may
        // emit SEVERAL manifolds — one per touching child pair (M7.12, ADR-0028). Split off so
        // the plain-pair path below stays byte-for-byte what it was through M7.11.
        const CompoundShape* comp_a = compound_of(shape[da]);
        const CompoundShape* comp_b = compound_of(shape[db]);
        if (comp_a != nullptr || comp_b != nullptr) {
            build_compound_contacts(pr, da, db, comp_a, comp_b, dt, out, warm);
            continue;
        }

        Manifold m;
        m.a = pr.a;
        m.b = pr.b;
        // collide_shapes canonicalizes by ShapeType internally and flips the normal to match, so
        // passing the bodies in slot order yields a normal oriented from a toward b (contact.hpp).
        // Hull ids resolve to store pointers HERE — below the seam, above the dispatch (ADR-0027).
        const ConvexHull* ha = hull_of(shape[da]);
        const ConvexHull* hb = hull_of(shape[db]);
        bool touching = collide_shapes(shape[da],
                                       position[da],
                                       orientation[da],
                                       shape[db],
                                       position[db],
                                       orientation[db],
                                       m,
                                       ha,
                                       hb) &&
                        m.count != 0;
        if (!touching) {
            // The exact shapes do not overlap. For a CCD pair (either body opted in) inside a real
            // step, ask whether a fast approach will close the gap THIS tick and, if so, emit a
            // speculative contact so the solver can arrest the body at the surface (M7.10). Passed
            // in slot order, so the speculative normal is a → b like the exact one. dt == 0 (the
            // inspection seam) never speculates, keeping compute_contacts at its M7.3 semantics.
            if (dt <= 0.0f || (ccd[da] == 0 && ccd[db] == 0) ||
                !collide_speculative(shape[da],
                                     position[da],
                                     orientation[da],
                                     linear_velocity[da],
                                     shape[db],
                                     position[db],
                                     orientation[db],
                                     linear_velocity[db],
                                     dt,
                                     m,
                                     ha,
                                     hb)) {
                continue; // a broadphase near-miss, or too far / not approaching to matter
            }
        }

        // Warm-start from last tick's cache: match points by feature id and carry their
        // accumulated impulses into the fresh manifold. The stored generations guard against a
        // recycled slot inheriting a dead pair's impulses. A plain pair is region (0, 0).
        const std::uint64_t pair_key =
            (static_cast<std::uint64_t>(pr.a.index) << 32) | static_cast<std::uint64_t>(pr.b.index);
        const auto it = contact_cache.find(ContactCacheKey{pair_key, 0});
        if (it != contact_cache.end() && it->second.gen_a == pr.a.generation &&
            it->second.gen_b == pr.b.generation) {
            warm += warm_start_from(it->second, m);
        }
        out.push_back(m);
    }
    warm_started_last = warm;
}

void PhysicsWorld::Impl::build_compound_contacts(const Pair& pr,
                                                 std::uint32_t da,
                                                 std::uint32_t db,
                                                 const CompoundShape* comp_a,
                                                 const CompoundShape* comp_b,
                                                 float dt,
                                                 std::vector<Manifold>& out,
                                                 std::uint32_t& warm) const {
    // Model A of ADR-0028, the narrowphase expansion: the broadphase reported ONE candidate pair
    // (compounds own a single union-bound proxy); here it fans out into child-vs-child sub-pairs.
    // A non-compound side counts as one child — its own shape at the body pose — so all four
    // combinations (compound-vs-primitive/hull/compound, either side) run through one loop. Every
    // stage is in FIXED order — a's child index ascending, then b's — so the emitted manifolds
    // extend the canonical pair order lexicographically by (child_a, child_b): determinism by
    // construction, no sorting needed (ADR-0026).

    // Pose each side's children once — O(children_a + children_b), not O(children_a·children_b) —
    // caching pose, resolved hull, and the tight world bound the cull below reads.
    struct PosedChild {
        const ShapeDesc* shape;
        const ConvexHull* hull;
        core::Vec3 pos;
        core::Quat orient;
        Aabb bounds;
    };

    // CCD (M7.10 × M7.12): a compound has no single support function (it is not convex), so
    // speculative contacts run PER CHILD — each child is convex and rides the body rigidly, so
    // its linear velocity is the body's (angular sweep ignored, exactly M7.10's documented
    // posture). For the cull to see a not-yet-touching-but-closing child pair at all, each side's
    // child bounds are swept by its body's velocity — the per-child mirror of the stage-1.5
    // proxy sweep.
    const bool ccd_pair = dt > 0.0f && (ccd[da] != 0 || ccd[db] != 0);
    const auto pose_side =
        [&](std::uint32_t dense, const CompoundShape* comp, std::vector<PosedChild>& list) {
            const core::Vec3 body_pos = position[dense];
            const core::Quat& body_q = orientation[dense];
            const std::size_t n = comp != nullptr ? comp->child_count() : 1;
            list.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                PosedChild c{};
                if (comp != nullptr) {
                    c.shape = &comp->child_shape[i];
                    c.pos = compound_child_world_pos(*comp, i, body_pos, body_q);
                    c.orient = compound_child_world_orient(*comp, i, body_q);
                } else {
                    c.shape = &shape[dense];
                    c.pos = body_pos;
                    c.orient = body_q;
                }
                c.hull = hull_of(*c.shape);
                c.bounds = c.hull != nullptr ? hull_world_aabb(*c.hull, c.pos, c.orient)
                                             : compute_aabb(*c.shape, c.pos, c.orient);
                if (ccd_pair) {
                    const core::Vec3 sweep = linear_velocity[dense] * dt;
                    Aabb swept = c.bounds;
                    swept.min += sweep;
                    swept.max += sweep;
                    c.bounds = merge(c.bounds, swept);
                }
                list.push_back(c);
            }
        };
    std::vector<PosedChild> side_a;
    std::vector<PosedChild> side_b;
    pose_side(da, comp_a, side_a);
    pose_side(db, comp_b, side_b);

    const std::uint64_t pair_key =
        (static_cast<std::uint64_t>(pr.a.index) << 32) | static_cast<std::uint64_t>(pr.b.index);

    for (std::size_t ia = 0; ia < side_a.size(); ++ia) {
        for (std::size_t ib = 0; ib < side_b.size(); ++ib) {
            const PosedChild& ca = side_a[ia];
            const PosedChild& cb = side_b[ib];
            // The v1 midphase: a brute-force child AABB cull, right at fracture-cell counts (a
            // child BVH is the measured-need follow-up, ADR-0028).
            if (!overlaps(expanded(ca.bounds, kChildCullMargin), cb.bounds)) {
                continue;
            }

            Manifold m;
            m.a = pr.a;
            m.b = pr.b;
            m.child_a = static_cast<std::uint16_t>(ia);
            m.child_b = static_cast<std::uint16_t>(ib);
            // The children are convex, so each sub-pair runs the EXISTING per-convex-shape
            // routines unchanged — passed in body slot order, so the normal is a → b exactly as
            // the plain path's (contact.hpp). The solver later applies the manifold to the PARENT
            // bodies: contact points are world-space and lever arms come from the parent COM,
            // which is precisely what "rigidly attached child" means.
            bool touching = collide_shapes(*ca.shape,
                                           ca.pos,
                                           ca.orient,
                                           *cb.shape,
                                           cb.pos,
                                           cb.orient,
                                           m,
                                           ca.hull,
                                           cb.hull) &&
                            m.count != 0;
            if (!touching) {
                if (!ccd_pair || !collide_speculative(*ca.shape,
                                                      ca.pos,
                                                      ca.orient,
                                                      linear_velocity[da],
                                                      *cb.shape,
                                                      cb.pos,
                                                      cb.orient,
                                                      linear_velocity[db],
                                                      dt,
                                                      m,
                                                      ca.hull,
                                                      cb.hull)) {
                    continue; // this child pair is separated and not imminently closing
                }
            }

            // Fold the child indices into every feature id (kFeatCompoundChild): two children
            // touching the same other shape through identical child-local features must still
            // carry DISTINCT ids. Applied uniformly to every pair that involves a compound —
            // and never to plain pairs, whose id spaces stay bit-identical to M7.11 (the
            // backward-compat contract).
            const std::uint32_t child_fold =
                feature_combine(feature_combine(kFeatCompoundChild, static_cast<std::uint32_t>(ia)),
                                static_cast<std::uint32_t>(ib));
            for (std::uint8_t k = 0; k < m.count; ++k) {
                m.points[k].feature_id = feature_combine(child_fold, m.points[k].feature_id);
            }

            // Warm-start from this REGION's own cache entry — (pair, child_a, child_b) — so one
            // foot of a standing compound can never inherit the other foot's impulses.
            const ContactCacheKey key{
                pair_key, (static_cast<std::uint32_t>(ia) << 16) | static_cast<std::uint32_t>(ib)};
            const auto it = contact_cache.find(key);
            if (it != contact_cache.end() && it->second.gen_a == pr.a.generation &&
                it->second.gen_b == pr.b.generation) {
                warm += warm_start_from(it->second, m);
            }
            out.push_back(m);
        }
    }
}

void PhysicsWorld::Impl::commit_contact_cache(const std::vector<Manifold>& manifolds) const {
    // Rebuild the persistent cache from this tick's manifolds — in step() they are the SOLVED
    // manifolds, so the impulses that carry to next tick are the converged ones (the whole point
    // of warm starting). Keyed per contact REGION: the broadphase pair key ((slot_a << 32) |
    // slot_b) plus the packed child sub-pair (0 for plain pairs — M7.12), which is exactly why a
    // pair carrying several compound manifolds commits several entries instead of silently
    // keeping only the first. A flat hash map is plenty here — lookups are per-region, nothing
    // ever iterates it, so its unordered-ness cannot leak into simulation order (the determinism
    // contract).
    std::unordered_map<ContactCacheKey, ManifoldCacheEntry, ContactCacheKeyHash> next;
    next.reserve(manifolds.size());
    for (const Manifold& m : manifolds) {
        const std::uint64_t pair_key =
            (static_cast<std::uint64_t>(m.a.index) << 32) | static_cast<std::uint64_t>(m.b.index);
        const ContactCacheKey key{pair_key,
                                  (static_cast<std::uint32_t>(m.child_a) << 16) |
                                      static_cast<std::uint32_t>(m.child_b)};
        next.emplace(key, make_cache_entry(pair_key, m.a.generation, m.b.generation, m));
    }
    contact_cache.swap(next);
}

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() = default;

BodyId PhysicsWorld::create_body(const BodyDesc& d) {
    Impl& p = *impl_;

    // A hull- or compound-shaped body must reference a store entry THIS world knows, or the body
    // would have no geometry to collide, bound, or weigh. Refusing with the null id (rather than
    // creating a ghost) keeps the failure at the call site that made it.
    if (d.shape.type == ShapeType::ConvexHull && p.hull_of(d.shape) == nullptr) {
        return BodyId{};
    }
    if (d.shape.type == ShapeType::Compound && p.compound_of(d.shape) == nullptr) {
        return BodyId{};
    }

    // The shape id is now known-valid (checked just above), and the rest of create_body cannot
    // fail — so take the store reference here (M8.5): this body now holds hull/compound i, and
    // unregister refuses to free geometry a live body still names. destroy_body drops it again.
    if (d.shape.type == ShapeType::ConvexHull) {
        ++p.hull_refs[d.shape.hull.index];
    } else if (d.shape.type == ShapeType::Compound) {
        ++p.compound_refs[d.shape.compound.index];
    }

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
    const float mass = d.mass > 0.0f ? d.mass : 1.0f;
    // Mass distribution: primitives have closed forms (shape.hpp); a hull's principal moments
    // were integrated at registration (per unit mass — inertia scales linearly with mass at
    // fixed geometry) and its principal-axis rotation rides along in the SoA column, identity
    // for every other shape. A compound's moments were COMPOSED at registration the same way
    // (parallel-axis over the children, ADR-0028) — the body just scales them by its mass, and
    // its `position` is the compound's COM because the store re-centred the child poses.
    MassProperties mp = compute_mass_properties(d.shape, mass);
    core::Quat principal = core::quat_identity();
    if (const ConvexHull* h = p.hull_of(d.shape); h != nullptr) {
        mp.inertia_diagonal = h->inertia_per_mass * mass;
        principal = h->principal;
    } else if (const CompoundShape* c = p.compound_of(d.shape); c != nullptr) {
        mp.inertia_diagonal = c->inertia_per_mass * mass;
        principal = c->principal;
    }
    const float im = dynamic ? inv_or_zero(mp.mass) : 0.0f;
    const core::Vec3 ii = dynamic ? core::Vec3{inv_or_zero(mp.inertia_diagonal.x),
                                               inv_or_zero(mp.inertia_diagonal.y),
                                               inv_or_zero(mp.inertia_diagonal.z)}
                                  : core::Vec3{0.0f, 0.0f, 0.0f};
    const core::Quat orient = core::normalize(d.orientation);

    // Insert the broadphase proxy tagged with the stable slot id, so a query returns slot ids
    // directly (mappable back to a BodyId without touching the dense arrays).
    const Aabb tight = p.aabb_of(d.shape, d.position, orient);
    const auto motion8 = static_cast<std::uint8_t>(d.motion);
    const std::int32_t proxy = p.tree_for(motion8).create_proxy(tight, slot);

    const auto dense = static_cast<std::uint32_t>(p.count());
    p.position.push_back(d.position);
    p.orientation.push_back(orient);
    p.linear_velocity.push_back(d.linear_velocity);
    p.angular_velocity.push_back(d.angular_velocity);
    p.inv_mass.push_back(im);
    p.inv_inertia.push_back(ii);
    p.inertia_principal.push_back(principal);
    p.motion.push_back(motion8);
    p.linear_damping.push_back(d.linear_damping);
    p.angular_damping.push_back(d.angular_damping);
    p.gravity_factor.push_back(d.gravity_factor);
    p.friction.push_back(d.friction);
    p.restitution.push_back(d.restitution);
    p.ccd.push_back(d.ccd ? std::uint8_t{1} : std::uint8_t{0});
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

    // Capture the shape reference BEFORE the swap-remove below overwrites p.shape[d]; the store
    // reference this body held is released at the end (M8.5), the mirror of create_body's
    // increment.
    const ShapeDesc released_shape = p.shape[d];

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
        p.inertia_principal[d] = p.inertia_principal[last];
        p.motion[d] = p.motion[last];
        p.linear_damping[d] = p.linear_damping[last];
        p.angular_damping[d] = p.angular_damping[last];
        p.gravity_factor[d] = p.gravity_factor[last];
        p.friction[d] = p.friction[last];
        p.restitution[d] = p.restitution[last];
        p.ccd[d] = p.ccd[last];
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
    p.inertia_principal.pop_back();
    p.motion.pop_back();
    p.linear_damping.pop_back();
    p.angular_damping.pop_back();
    p.gravity_factor.pop_back();
    p.friction.pop_back();
    p.restitution.pop_back();
    p.ccd.pop_back();
    p.sleep_timer.pop_back();
    p.asleep.pop_back();
    p.shape.pop_back();
    p.proxy.pop_back();
    p.dense_to_slot.pop_back();

    // Free the slot and bump its generation so old ids referencing it read as dead.
    p.slots[id.index].dense = core::kInvalidSlotIndex;
    ++p.slots[id.index].generation;
    p.free_slots.push_back(id.index);

    // Release the hull/compound reference this body held (M8.5). Guarded against underflow — a
    // primitive-shaped body holds none, and the count was incremented at create for exactly the
    // hull/compound shapes.
    if (released_shape.type == ShapeType::ConvexHull &&
        released_shape.hull.index < p.hull_refs.size() &&
        p.hull_refs[released_shape.hull.index] > 0) {
        --p.hull_refs[released_shape.hull.index];
    } else if (released_shape.type == ShapeType::Compound &&
               released_shape.compound.index < p.compound_refs.size() &&
               p.compound_refs[released_shape.compound.index] > 0) {
        --p.compound_refs[released_shape.compound.index];
    }
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

    // Snapshot the sleep state up front (M7.9): the tail diffs it against the post-step `asleep` to
    // emit sleep/wake events for exactly the transitions THIS step caused. Dense indices are stable
    // through a step (create/destroy happen only between steps), so snapshot[i] and asleep[i] name
    // the same body at both ends.
    p.asleep_snapshot.assign(p.asleep.begin(), p.asleep.end());

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

    // ---- 1.5 Sweep CCD proxies (M7.10). A body opted into continuous collision gets its
    // broadphase bound expanded to enclose where it WILL be this tick (position + post-gravity
    // velocity·dt), so the broadphase reports a thin obstacle in its path BEFORE the shapes overlap
    // — the precondition for stage 2's speculative test to see the pair at all. Done here, ahead of
    // the broadphase and every tick (not folded into the stage-8 refit), so a body that is fast
    // from the moment it spawns is covered on its very first step. Sequential, before the parallel
    // region; move_proxy adds the fat margin on top, and the stage-8 refit later restores the tight
    // box at the body's real resting position for external queries.
    if (dt > 0.0f) {
        for (std::size_t i = 0; i < n; ++i) {
            if (p.ccd[i] == 0 || !is_dynamic(i) || p.asleep[i] != 0) {
                continue; // CCD only helps a moving, awake, dynamic body (see docs/design)
            }
            const Aabb tight = p.aabb_of(p.shape[i], p.position[i], p.orientation[i]);
            const core::Vec3 predicted = p.position[i] + p.linear_velocity[i] * dt;
            const Aabb swept = merge(tight, p.aabb_of(p.shape[i], predicted, p.orientation[i]));
            p.dynamic_tree.move_proxy(p.proxy[i], swept);
        }
    }

    // ---- 2. Detect contacts: broadphase pairs → narrowphase manifolds, warm-started from last
    // tick's cache (committed post-solve at stage 8, so warm starts carry SOLVED impulses). Runs
    // for ALL bodies, asleep included: a new contact between a faller and a sleeping stack is
    // exactly how the stack learns to wake — stage 4 merges them into one island, stage 5
    // reactivates it. With CCD (M7.10), dt > 0 also lets an approaching fast pair get a speculative
    // contact from the swept bounds set just above.
    std::vector<Manifold> manifolds;
    p.build_contacts(manifolds, dt);

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
                        p.inertia_principal.data(),
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
            prepare_contact_constraint(bodies, m, da, db, static_cast<std::uint32_t>(mi), dt));
    }

    // ---- 4. Partition into ISLANDS (islands.hpp): connected components of the dynamic-body
    // contact graph. Every awake dynamic body lands in exactly one island (a contactless body is a
    // singleton); static/kinematic bodies are shared anchors, not island members.
    build_islands(n, p.motion, constraints, p.islands);
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

    // ---- 7.5 Emit contact & sleep events (M7.9, events.hpp). A pure read of the state the solve
    // just produced, in the sequential tail: the parallel_for above has joined, so the manifolds'
    // impulses are settled and reading them is race-free; nothing here writes body state, so the
    // world hash and cross-thread determinism are untouched. The event STREAM is itself bit-
    // identical for any worker count — it derives only from the canonical manifolds and the
    // (thread- count-independent) solved impulses and sleep decisions.

    // Build this tick's contact records from the SOLVED manifolds, in their canonical order —
    // one record per contact REGION (pair + child sub-pair; plain pairs are one region, M7.12).
    // Skip a pair with no dynamic member (an immovable pair exchanges no impulse — nothing to
    // report). A region that has a dynamic member but whose every dynamic member is now asleep is
    // recorded `suppressed`: kept present (so it is not falsely reported as Ended — an asleep pair
    // never separated) but emitting no event, which is what makes a settled pile silent.
    p.contact_cur.clear();
    for (const Manifold& m : manifolds) {
        const std::uint32_t da = p.dense_of(m.a);
        const std::uint32_t db = p.dense_of(m.b);
        if (da == core::kInvalidSlotIndex || db == core::kInvalidSlotIndex) {
            continue; // defensive: build_contacts only emits live pairs
        }
        if (p.inv_mass[da] + p.inv_mass[db] <= 0.0f) {
            continue; // no dynamic member — not an evented contact
        }
        // Representative point = the deepest (first max, strict >, so ties resolve
        // deterministically); impulses = the region's total exchanged momentum this tick.
        std::uint8_t best = 0;
        float normal_impulse = 0.0f;
        float tangent_impulse = 0.0f;
        for (std::uint8_t k = 0; k < m.count; ++k) {
            if (m.points[k].penetration > m.points[best].penetration) {
                best = k;
            }
            normal_impulse += m.points[k].normal_impulse;
            tangent_impulse += m.points[k].tangent_impulse;
        }
        const bool awake_dyn_a = p.inv_mass[da] > 0.0f && p.asleep[da] == 0;
        const bool awake_dyn_b = p.inv_mass[db] > 0.0f && p.asleep[db] == 0;
        p.contact_cur.push_back(Impl::ContactRecord{
            (static_cast<std::uint64_t>(m.a.index) << 32) | static_cast<std::uint64_t>(m.b.index),
            (static_cast<std::uint32_t>(m.child_a) << 16) | static_cast<std::uint32_t>(m.child_b),
            m.a,
            m.b,
            m.points[best].position,
            m.normal,
            normal_impulse,
            tangent_impulse,
            /*suppressed=*/!(awake_dyn_a || awake_dyn_b)});
    }

    // Classify by a linear merge of the two key-sorted lists (cur = this tick, prev = last tick):
    // a region only in cur is Began, only in prev is Ended, in both is Persisted. The merge key is
    // (pair key, child sub-pair) compared lexicographically — plain pairs all carry sub-pair 0,
    // so their merge is exactly the M7.9 pair merge. Suppressed cur records still consume their
    // merge position (so a matching prev record is not reported Ended) but emit nothing. The
    // result is naturally in canonical region order.
    p.contact_events_back.clear();
    const auto emit_contact = [&](const Impl::ContactRecord& r, ContactPhase phase) {
        ContactEvent e;
        e.a = r.a;
        e.b = r.b;
        e.point = r.point;
        e.normal = r.normal;
        // An Ended region exchanged nothing this tick; its record is last tick's, so ignore its
        // stored impulses and report zero.
        e.normal_impulse = phase == ContactPhase::Ended ? 0.0f : r.normal_impulse;
        e.tangent_impulse = phase == ContactPhase::Ended ? 0.0f : r.tangent_impulse;
        e.phase = phase;
        e.child_a = static_cast<std::uint16_t>(r.children >> 16);
        e.child_b = static_cast<std::uint16_t>(r.children & 0xFFFFu);
        p.contact_events_back.push_back(e);
    };
    const auto record_less = [](const Impl::ContactRecord& x, const Impl::ContactRecord& y) {
        return x.key < y.key || (x.key == y.key && x.children < y.children);
    };
    {
        const std::vector<Impl::ContactRecord>& cur = p.contact_cur;
        const std::vector<Impl::ContactRecord>& prev = p.contact_prev;
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < cur.size() && j < prev.size()) {
            if (record_less(cur[i], prev[j])) {
                if (!cur[i].suppressed) {
                    emit_contact(cur[i], ContactPhase::Began);
                }
                ++i;
            } else if (record_less(prev[j], cur[i])) {
                emit_contact(prev[j], ContactPhase::Ended);
                ++j;
            } else {
                if (!cur[i].suppressed) {
                    emit_contact(cur[i], ContactPhase::Persisted);
                }
                ++i;
                ++j;
            }
        }
        for (; i < cur.size(); ++i) {
            if (!cur[i].suppressed) {
                emit_contact(cur[i], ContactPhase::Began);
            }
        }
        for (; j < prev.size(); ++j) {
            emit_contact(prev[j], ContactPhase::Ended);
        }
    }
    p.contact_prev.swap(p.contact_cur); // this tick becomes next tick's baseline

    // Sleep/wake events: the bodies whose sleep state changed during this step, in dense order.
    p.sleep_events_back.clear();
    for (std::size_t i = 0; i < n; ++i) {
        if (p.asleep_snapshot[i] == p.asleep[i]) {
            continue;
        }
        SleepEvent e;
        e.body = BodyId{p.dense_to_slot[i], p.slots[p.dense_to_slot[i]].generation};
        e.phase = p.asleep[i] != 0 ? SleepPhase::Slept : SleepPhase::Woke;
        p.sleep_events_back.push_back(e);
    }

    // Publish: swap the filled back buffers to front, so the accessors return this tick's events,
    // stable until the next step() refills and swaps again (the double buffer).
    p.contact_events_front.swap(p.contact_events_back);
    p.sleep_events_front.swap(p.sleep_events_back);

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
            const Aabb tight = p.aabb_of(p.shape[i], p.position[i], p.orientation[i]);
            p.dynamic_tree.move_proxy(p.proxy[i], tight);
        }
    }

    // ---- 9. Collect the tick's instrument panel (M7.13, WorldStats). A pure read of the state the
    // pipeline just produced — body population, collision load, island structure — as deterministic
    // counts: it never touches body state, so the world hash and cross-thread determinism are
    // untouched, and the numbers themselves are thread-count-invariant (they derive only from the
    // canonical manifolds and the pure-function island partition). Cheap: one O(n) pass over the
    // bodies plus the O(manifolds)/O(islands) walks the tick already made.
    WorldStats& st = p.last_stats;
    st = WorldStats{};
    st.body_count = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
        switch (static_cast<MotionType>(p.motion[i])) {
            case MotionType::Dynamic:
                ++st.dynamic_bodies;
                if (p.asleep[i] != 0) {
                    ++st.sleeping_bodies;
                } else {
                    ++st.awake_bodies;
                }
                break;
            case MotionType::Static:
                ++st.static_bodies;
                break;
            case MotionType::Kinematic:
                ++st.kinematic_bodies;
                break;
        }
    }
    st.broadphase_pairs = p.broadphase_pairs_last;
    st.manifolds = static_cast<std::uint32_t>(manifolds.size());
    for (const Manifold& m : manifolds) {
        st.contact_points += m.count;
    }
    st.contacts_warm_started = p.warm_started_last;
    st.islands = static_cast<std::uint32_t>(isl.island_count);
    for (std::size_t k = 0; k < isl.island_count; ++k) {
        st.active_islands += (active[k] != 0) ? 1u : 0u;
        const std::uint32_t sz = isl.body_offsets[k + 1] - isl.body_offsets[k];
        st.largest_island = std::max(st.largest_island, sz);
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
    return impl_->last_stats.islands;
}

WorldStats PhysicsWorld::stats() const noexcept {
    return impl_->last_stats;
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

bool PhysicsWorld::raycast(const Ray& ray, RayHit& out, const QueryFilter& filter) const {
    const Impl& p = *impl_;
    const float dir_len = core::length(ray.direction);
    if (dir_len < 1e-8f) {
        return false; // a zero-length ray hits nothing
    }
    const core::Vec3 dir = ray.direction / dir_len; // unit ⇒ the reported t is a world distance
    const float tmax = ray.max_distance;

    float best_t = tmax;
    std::uint32_t best_slot = core::kInvalidSlotIndex;
    core::Vec3 best_n{0.0f, 0.0f, 0.0f};
    std::uint16_t best_child = 0;

    // A reported leaf's slot is live (it is in the tree), so slots[slot].dense is its current row.
    // Pass the running `best_t` as the exact test's bound so a farther candidate is rejected
    // cheaply; the nearest survivor across both trees wins.
    const auto test = [&](std::uint32_t slot) {
        const std::uint32_t d = p.slots[slot].dense;
        float t = 0.0f;
        core::Vec3 n{0.0f, 0.0f, 0.0f};
        std::uint16_t child = 0;
        if (ray_vs_shape(p.shape[d],
                         p.position[d],
                         p.orientation[d],
                         ray.origin,
                         dir,
                         best_t,
                         t,
                         n,
                         p.hull_of(p.shape[d]),
                         p.compound_of(p.shape[d]),
                         p.hull_span(),
                         &child) &&
            t < best_t) {
            best_t = t;
            best_slot = slot;
            best_n = n;
            best_child = child;
        }
    };

    // Each tree carries exactly one filter class — the dynamic tree is dynamic+kinematic, the
    // static tree is static — so a filter flag simply gates a whole tree; no per-leaf motion check
    // needed.
    if (filter.dynamics) {
        p.dynamic_tree.query_ray(ray.origin, dir, tmax, test);
    }
    if (filter.statics) {
        p.static_tree.query_ray(ray.origin, dir, tmax, test);
    }

    if (best_slot == core::kInvalidSlotIndex) {
        return false;
    }
    out.body = BodyId{best_slot, p.slots[best_slot].generation};
    out.point = ray.origin + dir * best_t;
    out.normal = best_n;
    out.distance = best_t;
    out.child = best_child; // which compound child was pierced (0 for a plain body) — M8.3
    return true;
}

void PhysicsWorld::overlap_sphere(core::Vec3 center,
                                  float radius,
                                  std::vector<BodyId>& out,
                                  const QueryFilter& filter) const {
    const Impl& p = *impl_;
    out.clear();
    const core::Vec3 r3{radius, radius, radius};
    const Aabb box{center - r3, center + r3};

    std::vector<std::uint32_t> hits;
    const auto collect = [&](std::uint32_t slot) {
        const std::uint32_t d = p.slots[slot].dense;
        if (sphere_vs_shape(center,
                            radius,
                            p.shape[d],
                            p.position[d],
                            p.orientation[d],
                            p.hull_of(p.shape[d]),
                            p.compound_of(p.shape[d]),
                            p.hull_span())) {
            hits.push_back(slot);
        }
    };
    if (filter.dynamics) {
        p.dynamic_tree.query(box, collect);
    }
    if (filter.statics) {
        p.static_tree.query(box, collect);
    }
    // Sort by stable slot id so the reported set is order-deterministic run to run (a hash map /
    // tree traversal order must never leak into a result the game or replication sees).
    std::sort(hits.begin(), hits.end());
    out.reserve(hits.size());
    for (const std::uint32_t slot : hits) {
        out.push_back(BodyId{slot, p.slots[slot].generation});
    }
}

void PhysicsWorld::apply_impulse(BodyId id, core::Vec3 impulse, core::Vec3 point) noexcept {
    Impl& p = *impl_;
    const std::uint32_t d = p.dense_of(id);
    if (d == core::kInvalidSlotIndex || p.inv_mass[d] == 0.0f) {
        return; // stale, or immovable (static/kinematic) — an impulse cannot move it
    }
    p.linear_velocity[d] += impulse * p.inv_mass[d];
    // Angular part: I⁻¹(r × J), with I⁻¹ the world-space inverse inertia — the exact same
    // composed apply_inv_inertia the solver uses (principal rotation included, M7.11), so an
    // external push and a contact impulse rotate a body by identical math.
    const core::Vec3 r = point - p.position[d];
    p.angular_velocity[d] += apply_inv_inertia(
        p.orientation[d], p.inertia_principal[d], p.inv_inertia[d], core::cross(r, impulse));
    // A sleeping body ignores velocity until something wakes it, so an impulse must wake it (its
    // island reactivates on the next step, exactly as wake_body documents).
    p.asleep[d] = 0;
    p.sleep_timer[d] = 0.0f;
}

void PhysicsWorld::apply_central_impulse(BodyId id, core::Vec3 impulse) noexcept {
    Impl& p = *impl_;
    const std::uint32_t d = p.dense_of(id);
    if (d == core::kInvalidSlotIndex || p.inv_mass[d] == 0.0f) {
        return;
    }
    p.linear_velocity[d] += impulse * p.inv_mass[d]; // applied through the COM ⇒ no torque
    p.asleep[d] = 0;
    p.sleep_timer[d] = 0.0f;
}

HullId PhysicsWorld::register_hull(const HullDesc& desc) {
    // Validate + derive (planes, mass properties, principal axes) in one cold pass — see
    // build_convex_hull (src/hull.hpp) for the rules and the math. Nothing is stored on failure,
    // so a rejected registration leaves the world bit-identical to before the call.
    ConvexHull hull;
    if (!build_convex_hull(desc.vertices, desc.face_counts, desc.face_indices, hull)) {
        return HullId{}; // null id — HullId::is_valid() == false
    }
    Impl& p = *impl_;
    // Reuse a freed slot if one is waiting (LIFO — a pure function of the register/unregister call
    // sequence, so ids stay deterministic), else grow the store. A reused slot keeps the generation
    // unregister_hull already bumped; a fresh slot starts at generation 0.
    std::uint32_t index;
    if (!p.hull_free_list.empty()) {
        index = p.hull_free_list.back();
        p.hull_free_list.pop_back();
        p.hulls[index] = std::move(hull);
        p.hull_live[index] = 1;
        p.hull_refs[index] = 0;
    } else {
        index = static_cast<std::uint32_t>(p.hulls.size());
        p.hulls.push_back(std::move(hull));
        p.hull_generation.push_back(0);
        p.hull_live.push_back(1);
        p.hull_refs.push_back(0);
    }
    return HullId{index, p.hull_generation[index]};
}

bool PhysicsWorld::unregister_hull(HullId id) {
    Impl& p = *impl_;
    // Unknown / stale / already-freed ids are a safe no-op (return false) — never touch another
    // slot's state. A live-but-referenced hull is refused too: some body or compound still names
    // it, and freeing it would dangle that reference (the reject-if-referenced contract, ADR-0027).
    if (id.index >= p.hulls.size() || p.hull_generation[id.index] != id.generation ||
        p.hull_live[id.index] == 0) {
        return false;
    }
    if (p.hull_refs[id.index] != 0) {
        return false;
    }
    p.hull_live[id.index] = 0;
    ++p.hull_generation[id.index]; // any id still holding the old generation now reads dead
    p.hull_free_list.push_back(id.index);
    return true;
}

bool PhysicsWorld::hull_info(HullId id, HullInfo& out) const {
    const Impl& p = *impl_;
    if (id.index >= p.hulls.size() || p.hull_generation[id.index] != id.generation ||
        p.hull_live[id.index] == 0) {
        return false;
    }
    const ConvexHull& h = p.hulls[id.index];
    out.volume = h.volume;
    out.centroid = h.centroid_authored;
    out.inertia_per_mass = h.inertia_per_mass;
    out.principal_rotation = h.principal;
    return true;
}

CompoundId PhysicsWorld::register_compound(const CompoundDesc& desc) {
    Impl& p = *impl_;
    // Validate each hull child against the LIVE store (generation + not-freed) BEFORE composing:
    // build_compound resolves children by index alone (compound_child_hull is a bounds check), so a
    // child naming a freed or stale hull slot must be caught here, where the slot metadata lives. A
    // primitive child carries no hull id and is skipped.
    for (const CompoundChildDesc& child : desc.children) {
        if (child.shape.type == ShapeType::ConvexHull) {
            const HullId h = child.shape.hull;
            if (h.index >= p.hulls.size() || p.hull_generation[h.index] != h.generation ||
                p.hull_live[h.index] == 0) {
                return CompoundId{}; // a child names a hull this world does not have live
            }
        }
    }

    // Compose (COM, parallel-axis inertia, principal axes, re-centred child poses) in one cold pass
    // — see build_compound (src/compound.hpp) for the rules and the math
    // (docs/math/compound-mass-properties.md). Nothing is stored on failure, so a rejected
    // registration leaves the world bit-identical to before the call — the register_hull posture.
    CompoundShape compound;
    if (!build_compound(desc.children, p.hull_span(), compound)) {
        return CompoundId{}; // null id — CompoundId::is_valid() == false
    }
    // Reuse a freed slot LIFO (deterministic ids) or grow the store, mirroring register_hull.
    std::uint32_t index;
    if (!p.compound_free_list.empty()) {
        index = p.compound_free_list.back();
        p.compound_free_list.pop_back();
        p.compounds[index] = std::move(compound);
        p.compound_live[index] = 1;
        p.compound_refs[index] = 0;
    } else {
        index = static_cast<std::uint32_t>(p.compounds.size());
        p.compounds.push_back(std::move(compound));
        p.compound_generation.push_back(0);
        p.compound_live.push_back(1);
        p.compound_refs.push_back(0);
    }
    // The compound now holds a reference on each hull child — so unregister_hull will refuse to
    // free a hull this compound still needs. unregister_compound releases these again.
    for (const CompoundChildDesc& child : desc.children) {
        if (child.shape.type == ShapeType::ConvexHull) {
            ++p.hull_refs[child.shape.hull.index];
        }
    }
    return CompoundId{index, p.compound_generation[index]};
}

bool PhysicsWorld::unregister_compound(CompoundId id) {
    Impl& p = *impl_;
    // Unknown / stale / already-freed → safe no-op. A compound still used by a live body is refused
    // (reject-if-referenced, ADR-0028).
    if (id.index >= p.compounds.size() || p.compound_generation[id.index] != id.generation ||
        p.compound_live[id.index] == 0) {
        return false;
    }
    if (p.compound_refs[id.index] != 0) {
        return false;
    }
    // Release the compound's hold on each of its child hulls (so they become unregisterable once no
    // other reference remains) — the mirror of the increments register_compound made.
    const CompoundShape& c = p.compounds[id.index];
    for (std::size_t i = 0; i < c.child_count(); ++i) {
        const ShapeDesc& cs = c.child_shape[i];
        if (cs.type == ShapeType::ConvexHull && cs.hull.index < p.hull_refs.size() &&
            p.hull_refs[cs.hull.index] > 0) {
            --p.hull_refs[cs.hull.index];
        }
    }
    p.compound_live[id.index] = 0;
    ++p.compound_generation[id.index];
    p.compound_free_list.push_back(id.index);
    return true;
}

bool PhysicsWorld::compound_info(CompoundId id, CompoundInfo& out) const {
    const Impl& p = *impl_;
    if (id.index >= p.compounds.size() || p.compound_generation[id.index] != id.generation ||
        p.compound_live[id.index] == 0) {
        return false;
    }
    const CompoundShape& c = p.compounds[id.index];
    out.volume = c.volume;
    out.centroid = c.centroid_authored;
    out.inertia_per_mass = c.inertia_per_mass;
    out.principal_rotation = c.principal;
    out.child_count = static_cast<std::uint32_t>(c.child_count());
    return true;
}

std::span<const ContactEvent> PhysicsWorld::contact_events() const noexcept {
    return {impl_->contact_events_front.data(), impl_->contact_events_front.size()};
}

std::span<const SleepEvent> PhysicsWorld::sleep_events() const noexcept {
    return {impl_->sleep_events_front.data(), impl_->sleep_events_front.size()};
}

} // namespace rime::physics
