// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

#include "rime/core/hash.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/events.hpp"
#include "rime/physics/world.hpp"
#include "world_impl.hpp"

// Damage → connectivity → fracture (M8.3, ADR-0029 §2–§4): the half of DestructionWorld where the
// wall actually breaks. Everything in this file runs in the SEQUENTIAL TAIL of the simulation tick
// (after PhysicsWorld::step returns), and everything is written to be a pure function of its
// inputs: op lists are canonically sorted, every table is walked in ascending index order, debris
// bodies are created in a fixed order, and no unordered container exists to leak an iteration
// order. That discipline is not style — it is the M11 replay contract (ADR-0026/0029): the same
// damage inputs must produce bit-identical part state, island compositions, and physics ids on
// every run and for any physics worker count. state_hash() + world_hash() are the witnesses.
namespace rime::destruction {

namespace {

// Debris mass under the v1 uniform-density model: concrete-ish, applied to the cooked part
// volumes. One constant for every pattern until the damage material grows a density field (the
// cook's damage_threshold/damage_scale are per-pattern already; density joins them when a real
// material system lands — v1 keeps the honest single number).
constexpr float kDebrisDensity = 1000.0f; // kg/m³

[[nodiscard]] std::uint32_t float_bits(float f) noexcept {
    return std::bit_cast<std::uint32_t>(f);
}

// Distance from a world point to a part's world-space bounds: the cooked (destructible-frame)
// part AABB is carried through the instance placement corner by corner and re-boxed — a
// conservative axis-aligned bound of the rotated box, which is exactly the fidelity radius damage
// wants (it is a gameplay falloff shape, not a collision query; the hull-exact test is named
// polish, not built). Returns 0 for a point inside the bounds.
[[nodiscard]] float distance_to_part_bounds(const core::Transform& placement,
                                            core::Vec3 aabb_min,
                                            core::Vec3 aabb_max,
                                            core::Vec3 point) noexcept {
    core::Vec3 lo{0.0f, 0.0f, 0.0f};
    core::Vec3 hi{0.0f, 0.0f, 0.0f};
    for (int corner = 0; corner < 8; ++corner) {
        const core::Vec3 local{(corner & 1) != 0 ? aabb_max.x : aabb_min.x,
                               (corner & 2) != 0 ? aabb_max.y : aabb_min.y,
                               (corner & 4) != 0 ? aabb_max.z : aabb_min.z};
        const core::Vec3 w = core::transform_point(placement, local);
        if (corner == 0) {
            lo = w;
            hi = w;
        } else {
            lo = core::Vec3{std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = core::Vec3{std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
    }
    const core::Vec3 clamped{std::clamp(point.x, lo.x, hi.x),
                             std::clamp(point.y, lo.y, hi.y),
                             std::clamp(point.z, lo.z, hi.z)};
    return core::length(point - clamped);
}

} // namespace

// Every payload field participates in the key, so the order is a total order on the op's VALUE —
// two ops compare equal only when they are byte-identical, i.e. interchangeable — and therefore
// the arrival order of same-tick apply_damage calls cannot influence the applied sequence. Floats
// are compared as raw bit patterns: not numerically meaningful, but that is the point — stable,
// platform-independent, and total (no NaN traps).
std::array<std::uint32_t, 9> DestructionWorld::Impl::op_key(const DamageOp& o) noexcept {
    return {o.instance,
            o.part,
            float_bits(o.amount),
            float_bits(o.impulse.x),
            float_bits(o.impulse.y),
            float_bits(o.impulse.z),
            float_bits(o.point.x),
            float_bits(o.point.y),
            float_bits(o.point.z)};
}

void DestructionWorld::Impl::fracture_instance(std::uint32_t instance_index,
                                               std::span<const DamageOp> ops,
                                               physics::PhysicsWorld& world) {
    Instance& inst = instances[instance_index];
    const Pattern& pat = patterns[inst.pattern.index];
    const std::uint32_t n = pat.part_count;

    // ---- The support solve: union-find over the LIVE bond graph. A bond holds only while both
    // endpoints still stand; components are seeded from the still-standing anchors. Union-find is
    // the classic disjoint-set structure — near-O(1) amortized unions/finds — and it is made
    // deterministic here by one rule: a component's representative is its SMALLEST part id (unite
    // attaches the larger root under the smaller). With that, the partition and everything derived
    // from it is a pure function of the alive bits and the cooked bond list — no traversal order,
    // no hashing, nothing run-dependent.
    std::vector<std::uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&parent](std::uint32_t x) noexcept {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]]; // path halving: point at the grandparent as we walk
            x = parent[x];
        }
        return x;
    };
    for (const assets::DestructibleBond& b : pat.bonds) {
        if (inst.alive[b.a] == 0 || inst.alive[b.b] == 0) {
            continue; // a bond with a dead/detached endpoint is broken glue
        }
        const std::uint32_t ra = find(b.a);
        const std::uint32_t rb = find(b.b);
        if (ra != rb) {
            if (ra < rb) {
                parent[rb] = ra;
            } else {
                parent[ra] = rb;
            }
        }
    }
    std::vector<std::uint8_t> root_supported(n, 0);
    for (const std::uint32_t a : pat.anchors) {
        if (inst.alive[a] != 0) {
            root_supported[find(a)] = 1;
        }
    }

    // ---- Partition the standing parts: supported ⇒ the anchored REMAINDER (stays a wall);
    // unsupported ⇒ ISLANDS, grouped by union-find root. One ascending scan does both, so the
    // remainder is the standing ids in ascending order (exactly the derivable child→part table of
    // ADR-0029 §4) and islands are ordered by their smallest member — the canonical debris
    // creation order.
    std::vector<std::uint32_t> remainder;
    std::vector<std::vector<std::uint32_t>> islands;
    std::vector<std::uint32_t> island_of_root(n, kInvalidPartIndex);
    for (std::uint32_t p = 0; p < n; ++p) {
        if (inst.alive[p] == 0) {
            continue;
        }
        const std::uint32_t r = find(p);
        if (root_supported[r] != 0) {
            remainder.push_back(p);
            continue;
        }
        if (island_of_root[r] == kInvalidPartIndex) {
            island_of_root[r] = static_cast<std::uint32_t>(islands.size());
            islands.emplace_back();
        }
        islands[island_of_root[r]].push_back(p);
    }

    // ---- The body swap. A registered compound is immutable (ADR-0028), so the wall cannot lose
    // a child in place — the standing body is REPLACED: destroy it, re-register the remainder as
    // a fresh compound, stand a fresh static body. Read the parent's motion first: debris inherit
    // it (v = v_com + ω × r at their new COM). For today's static walls that is all zeros, but the
    // recipe is written generally so a moving destructible (a ship's hull, v2) inherits correctly.
    physics::BodyState parent_state{};
    (void)world.get_body_state(inst.body, parent_state);

    map_erase(inst.body);
    world.destroy_body(inst.body);
    inst.body = physics::BodyId{};

    if (!remainder.empty()) {
        // Children at their cooked COMs, identity-rotated — the register_pattern recipe on the
        // surviving subset. register_compound re-centres the stored poses on the subset's combined
        // COM, so placing the body at `placement ∘ centroid` lands every surviving part EXACTLY
        // where it stood before the swap (the no-pop invariant; spawn() documents the same
        // recipe). The old compound id is deliberately leaked — the stores are append-only until
        // m8.5's unregister (ADR-0027/0028's recorded deferral).
        std::vector<physics::CompoundChildDesc> children;
        children.reserve(remainder.size());
        for (const std::uint32_t p : remainder) {
            physics::ShapeDesc shape;
            shape.type = physics::ShapeType::ConvexHull;
            shape.hull = pat.hulls[p];
            children.push_back(
                physics::CompoundChildDesc{shape, pat.part_com[p], core::quat_identity()});
        }
        const physics::CompoundId comp = world.register_compound(physics::CompoundDesc{children});
        // A subset of a pattern that registered once cannot fail validation (same hulls, 1..256
        // children); the guard keeps a malformed store from cascading rather than expecting it.
        if (comp.is_valid()) {
            physics::CompoundInfo info;
            (void)world.compound_info(comp, info);
            physics::BodyDesc desc;
            desc.motion = physics::MotionType::Static;
            desc.shape.type = physics::ShapeType::Compound;
            desc.shape.compound = comp;
            desc.position = core::transform_point(inst.placement, info.centroid);
            desc.orientation = inst.placement.rotation;
            inst.body = world.create_body(desc);
            map_insert(inst.body, instance_index);
        }
    }
    inst.child_to_part = remainder; // rows = the standing part ids, ascending (ADR-0029 §4)

    // ---- The detached groups: the unsupported ISLANDS (computed above from the standing parts),
    // plus every part KILLED this tick — ADR-0029 §2's "the part that just died detaches as a hull
    // body carrying the killing impulse". A killed part is already alive == 0 here (stage 2 set
    // it), and it died THIS tick (not in a prior fracture) iff an APPLIED op of this update names
    // it: an op only applies to a part that still stood when it ran, so a now-dead target must have
    // fallen in this very update. Each killed part is its own single-part group. Islands and killed
    // parts are disjoint sets of parts (killed ⇒ alive 0 and excluded from the union-find; islands
    // are alive), so ordering the merged list by smallest member is a strict total order — the
    // canonical debris creation order that makes the roster reproducible.
    std::vector<std::vector<std::uint32_t>> groups = std::move(islands);
    {
        std::vector<std::uint8_t> killed_this_tick(n, 0);
        for (const DamageOp& op : ops) {
            if (op.applied && op.instance == instance_index && inst.alive[op.part] == 0) {
                killed_this_tick[op.part] = 1;
            }
        }
        for (std::uint32_t p = 0; p < n; ++p) {
            if (killed_this_tick[p] != 0) {
                groups.push_back({p});
            }
        }
    }
    std::sort(groups.begin(),
              groups.end(),
              [](const std::vector<std::uint32_t>& x,
                 const std::vector<std::uint32_t>& y) noexcept { return x.front() < y.front(); });

    // ---- Each group becomes a dynamic debris body, in that canonical order. One part → a hull
    // body (a killed chunk, or a lone orphaned part); several → a runtime dynamic compound
    // (decision ratified: a multi-part island keeps its shape — the Frostbite look — rather than
    // shattering into loose parts).
    for (const std::vector<std::uint32_t>& group : groups) {
        float volume = 0.0f;
        for (const std::uint32_t p : group) {
            inst.alive[p] = 0; // the part leaves the wall (a killed part already is; harmless)
            volume += pat.part_volume[p];
        }

        physics::BodyDesc desc;
        desc.motion = physics::MotionType::Dynamic;
        desc.mass = kDebrisDensity * volume;
        core::Vec3 com_local{0.0f, 0.0f, 0.0f}; // group COM in the destructible frame
        if (group.size() == 1) {
            desc.shape.type = physics::ShapeType::ConvexHull;
            desc.shape.hull = pat.hulls[group[0]];
            com_local = pat.part_com[group[0]];
        } else {
            std::vector<physics::CompoundChildDesc> children;
            children.reserve(group.size());
            for (const std::uint32_t p : group) {
                physics::ShapeDesc shape;
                shape.type = physics::ShapeType::ConvexHull;
                shape.hull = pat.hulls[p];
                children.push_back(
                    physics::CompoundChildDesc{shape, pat.part_com[p], core::quat_identity()});
            }
            const physics::CompoundId comp =
                world.register_compound(physics::CompoundDesc{children});
            if (!comp.is_valid()) {
                continue; // unreachable for a registered pattern's subset; never half-spawn
            }
            physics::CompoundInfo info;
            (void)world.compound_info(comp, info);
            desc.shape.type = physics::ShapeType::Compound;
            desc.shape.compound = comp;
            com_local = info.centroid;
        }
        // Same placement recipe as the remainder: position IS the group's COM, carried through
        // the instance placement, so on its birth tick the debris sits exactly where its parts
        // stood — no detach-tick pop, no first-tick interpenetration.
        desc.position = core::transform_point(inst.placement, com_local);
        desc.orientation = inst.placement.rotation;
        const core::Vec3 r = desc.position - parent_state.position;
        desc.linear_velocity =
            parent_state.linear_velocity + core::cross(parent_state.angular_velocity, r);
        desc.angular_velocity = parent_state.angular_velocity;
        const physics::BodyId body = world.create_body(desc);

        // The damage that freed this group kicks it off: every APPLIED op whose target part is a
        // member, applied in the canonical op order. A killed part carries the very impulse that
        // destroyed it (ADR-0029 §2 — the struck chunk flies off), and an orphaned island carries
        // whatever ops struck its members; either way the push is the accumulated applied impulse,
        // so momentum is conserved and never doubled (an op landing on already-dead rubble has
        // applied == false and is skipped).
        for (const DamageOp& op : ops) {
            if (!op.applied || op.instance != instance_index) {
                continue;
            }
            if (!std::binary_search(group.begin(), group.end(), op.part)) {
                continue;
            }
            if (op.central) {
                world.apply_central_impulse(body, op.impulse);
            } else {
                world.apply_impulse(body, op.impulse, op.point);
            }
        }

        Debris rec;
        rec.body = body;
        rec.instance = instance_index;
        rec.first_part = static_cast<std::uint32_t>(debris_part_pool.size());
        rec.part_count = static_cast<std::uint32_t>(group.size());
        debris_part_pool.insert(debris_part_pool.end(), group.begin(), group.end());
        debris.push_back(rec);
    }
}

void DestructionWorld::apply_damage(InstanceId instance,
                                    core::Vec3 point,
                                    float radius,
                                    float amount,
                                    core::Vec3 impulse) {
    // Unknown/stale ids are safe no-ops (instances are append-only in v1, so generation is always
    // 0 — a nonzero generation is from a different era and cannot match).
    if (instance.index >= impl_->instances.size() || instance.generation != 0) {
        return;
    }
    impl_->pending_damage.push_back(
        Impl::DamageCall{instance.index, point, std::max(radius, 0.0f), amount, impulse});
}

void DestructionWorld::update(physics::PhysicsWorld& world) {
    Impl& impl = *impl_;

    // ================================================================================== stage 1
    // Gather this tick's damage ops in CANONICAL order (ADR-0029 §3): first the explicit ops,
    // sorted by (instance, part, op bytes); then the contact ops in the event stream's order,
    // which PhysicsWorld already guarantees canonical (ascending a, b, child_a, child_b) for any
    // worker count. The one fixed sequence is what makes the float accumulation below — and hence
    // every health value, every death, every island — bit-reproducible no matter how the inputs
    // arrived.
    std::vector<Impl::DamageOp> ops;

    for (const Impl::DamageCall& call : impl.pending_damage) {
        const Impl::Instance& inst = impl.instances[call.instance];
        const Impl::Pattern& pat = impl.patterns[inst.pattern.index];
        for (std::uint32_t p = 0; p < pat.part_count; ++p) {
            if (inst.alive[p] == 0) {
                continue; // eroded or already detached — debris damage is deferred (m8.5)
            }
            const float dist = distance_to_part_bounds(
                inst.placement, pat.part_aabb_min[p], pat.part_aabb_max[p], call.point);
            if (dist > call.radius) {
                continue;
            }
            // Linear falloff (the ratified v1 shape): full amount at the centre, zero at the rim.
            // A zero radius is a point op — full strength on any part whose bounds contain it.
            const float falloff = call.radius > 0.0f ? 1.0f - dist / call.radius : 1.0f;
            const float amount = call.amount * falloff;
            if (!(amount > 0.0f)) {
                continue; // rim-exact or non-positive: no health change, no push to carry
            }
            Impl::DamageOp op;
            op.instance = call.instance;
            op.part = p;
            op.amount = amount;
            op.impulse = call.impulse * falloff;
            op.central = true;
            ops.push_back(op);
        }
    }
    std::sort(
        ops.begin(), ops.end(), [](const Impl::DamageOp& a, const Impl::DamageOp& b) noexcept {
            return Impl::op_key(a) < Impl::op_key(b);
        });

    // Contact-derived ops. Each event region names the struck child on both sides; a side that
    // resolves to a live destructible instance takes damage on child→part (the ADR-0029 §4 remap —
    // compound child index == remap row, and on an intact instance that is the identity, because
    // register_pattern pushed one compound child per part in cook order and register_compound
    // preserves child order — the invariant the whole damage-to-part bridge stands on). The cooked
    // threshold fences the resting case: a standing wall's own support contacts exchange m·g·dt
    // every tick (Persisted events) and must never erode it. The push a side carries is the
    // solver's normal impulse along ITS direction of shove — the normal points a → b, so a was
    // pushed along −normal and b along +normal.
    const auto drain_side = [&impl, &ops](const physics::ContactEvent& e, bool side_a) {
        const physics::BodyId body = side_a ? e.a : e.b;
        const std::uint32_t instance_index = impl.instance_of(body);
        if (instance_index == 0xFFFFFFFFu) {
            return; // not a destructible's standing body (ground, marble, debris, stale id)
        }
        const Impl::Instance& inst = impl.instances[instance_index];
        const Impl::Pattern& pat = impl.patterns[inst.pattern.index];
        const std::uint16_t child = side_a ? e.child_a : e.child_b;
        if (child >= inst.child_to_part.size()) {
            return; // a child the current compound does not have — cannot happen for events of
                    // the body we mapped, but never index on faith
        }
        const std::uint32_t part = inst.child_to_part[child];
        const float damage = (e.normal_impulse - pat.damage_threshold) * pat.damage_scale;
        if (!(damage > 0.0f)) {
            return; // below the material threshold: absorbed (the resting m·g·dt case)
        }
        Impl::DamageOp op;
        op.instance = instance_index;
        op.part = part;
        op.amount = damage;
        op.impulse = e.normal * (side_a ? -e.normal_impulse : e.normal_impulse);
        op.point = e.point;
        op.central = false;
        ops.push_back(op);
    };
    for (const physics::ContactEvent& e : world.contact_events()) {
        if (!(e.normal_impulse > 0.0f)) {
            continue; // Ended regions exchanged nothing (and may name already-dead bodies)
        }
        drain_side(e, true);
        drain_side(e, false);
    }

    // ================================================================================== stage 2
    // Apply the sequence. Health erodes op by op; a part reaching zero DIES (erodes) and dirties
    // its instance. Ops that land after their part is gone are absorbed — overkill does not carry
    // over, and their impulse is spent (op.applied stays false).
    std::vector<std::uint32_t> dirty;
    for (Impl::DamageOp& op : ops) {
        Impl::Instance& inst = impl.instances[op.instance];
        if (inst.alive[op.part] == 0) {
            continue;
        }
        inst.health[op.part] -= op.amount;
        op.applied = true;
        if (inst.health[op.part] <= 0.0f) {
            inst.health[op.part] = 0.0f;
            inst.alive[op.part] = 0;
            dirty.push_back(op.instance);
        }
    }

    // ================================================================================ stage 3+4
    // Per instance whose membership changed (ascending — the canonical instance order): the
    // support solve and the body swap. Only a DEATH can change connectivity (bonds break when an
    // endpoint dies), so "some part died" is exactly "the compound must be rebuilt"; an update
    // that eroded health without a death — or a tick of mere resting contacts — swaps nothing.
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    for (const std::uint32_t instance_index : dirty) {
        impl.fracture_instance(instance_index, ops, world);
    }

    impl.pending_damage.clear();
}

std::uint64_t DestructionWorld::state_hash() const noexcept {
    // FNV-1a over every field that defines the destruction state, folded field by field (never
    // over whole structs — padding bytes would poison the hash) in the one canonical order:
    // instances ascending, then debris in creation order. Pairs with PhysicsWorld::world_hash()
    // as the M11 witness: destruction equality + motion-state equality = the replayed tick really
    // was the same tick.
    const Impl& impl = *impl_;
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    const auto fold_u32 = [&h](std::uint32_t v) {
        const std::array<std::uint32_t, 1> one = {v};
        h = core::fnv1a_64(std::as_bytes(std::span<const std::uint32_t>{one}), h);
    };
    fold_u32(static_cast<std::uint32_t>(impl.instances.size()));
    for (const Impl::Instance& inst : impl.instances) {
        fold_u32(inst.body.index);
        fold_u32(inst.body.generation);
        h = core::fnv1a_64(
            std::as_bytes(std::span<const std::uint8_t>{inst.alive.data(), inst.alive.size()}), h);
        for (const float f : inst.health) {
            fold_u32(float_bits(f));
        }
    }
    fold_u32(static_cast<std::uint32_t>(impl.debris.size()));
    for (const Impl::Debris& d : impl.debris) {
        fold_u32(d.body.index);
        fold_u32(d.body.generation);
        fold_u32(d.instance);
        fold_u32(d.part_count);
        h = core::fnv1a_64(std::as_bytes(std::span<const std::uint32_t>{
                               impl.debris_part_pool.data() + d.first_part, d.part_count}),
                           h);
    }
    return h;
}

} // namespace rime::destruction
