// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <cstddef>
#include <cstdint>
#include <limits>

#include "rime/physics/world.hpp"
#include "world_impl.hpp"

// The debris lifetime & budget half of M8.5 (ADR-0029 §8). M8.3 left the physics stores
// APPEND-ONLY: every fracture registers fresh compounds and spawns fresh bodies, and nothing is
// ever freed (the recorded ADR-0027/0028 deferral — "the old compound id is deliberately leaked").
// This file closes that gap so a wall under CONTINUOUS refracture keeps the hull/compound/body
// stores BOUNDED instead of growing without limit: settled debris are reclaimed after a linger, and
// the live debris-body population is held under a cap. It leans on the M8.5 physics half
// (unregister_hull/ unregister_compound, reject-if-referenced) that PR #60 added expressly for
// this.
//
// Determinism (the M11 replay contract) is preserved by the same discipline as the rest of the
// module: this pass runs only in update()'s SEQUENTIAL TAIL (never touching the parallel step), it
// walks the roster in ascending index order, and the cap's victim is chosen by a strict total order
// (score, with the ascending scan as the tiebreak). Nothing here reads the camera, the wall clock,
// or a thread id — so the same inputs AND the same LifecycleConfig reclaim the same bodies on every
// run and for any physics worker count. Enabling the lifecycle DOES change the physics id sequence
// (frees happen), but that sequence is still a pure function of the calls — "same config" equality,
// not "same as lifecycle-off".
namespace rime::destruction {

std::size_t DestructionWorld::Impl::visual_debris_count() const noexcept {
    std::size_t n = 0;
    for (const Debris& d : debris) {
        n += d.phase != DebrisPhase::Retired ? 1 : 0;
    }
    return n;
}

void DestructionWorld::Impl::retire_debris(std::size_t debris_index) {
    Debris& rec = debris[debris_index];
    if (rec.phase == DebrisPhase::Retired) {
        return; // idempotent, like freeze
    }
    rec.phase = DebrisPhase::Retired;

    // The announcement, on the channel that already exists. Same payload shape as PartDied and
    // IslandDetached, so a lighting cache subscribes to it with the code it already has — which is
    // exactly what ADR-0032 C6 promised ("no new plumbing").
    DestructionEvent ev;
    ev.kind = DestructionEventKind::DebrisEvicted;
    ev.instance = InstanceId{rec.instance, 0};
    ev.body = physics::BodyId{}; // already destroyed at freeze time
    ev.world_bounds = rec.last_bounds;
    ev.magnitude = 0.0f;
    events.push(ev);
}

std::size_t DestructionWorld::Impl::live_debris_count() const noexcept {
    std::size_t n = 0;
    for (const Debris& d : debris) {
        if (d.phase != DebrisPhase::Frozen) {
            ++n;
        }
    }
    return n;
}

float DestructionWorld::Impl::debris_size(const Debris& d) const noexcept {
    // Σ the member parts' cooked volumes — the debris's size in the same units the cap scores by
    // (mass is this × the uniform density, so volume is the density-free size proxy).
    const Pattern& pat = patterns[instances[d.instance].pattern.index];
    float v = 0.0f;
    for (std::uint32_t i = 0; i < d.part_count; ++i) {
        v += pat.part_volume[debris_part_pool[d.first_part + i]];
    }
    return v;
}

void DestructionWorld::Impl::freeze_debris(std::size_t debris_index, physics::PhysicsWorld& world) {
    Debris& rec = debris[debris_index];
    if (rec.phase == DebrisPhase::Frozen) {
        return; // already reclaimed — idempotent (the linger sweep and the cap can both pick it)
    }
    // Destroy the body FIRST: that releases its reference to the shape, so the compound is then
    // unreferenced and unregister_compound (reject-if-referenced) accepts it. A k>1 island owns a
    // runtime compound registered just for it; a k=1 debris body instead carries a SHARED pattern
    // hull, so its `compound` is null and there is nothing to free — unregistering that shared hull
    // would dangle every sibling and every future spawn that uses it (the pattern is immortal).
    // Capture the extent BEFORE the body goes. This is the last moment anyone can ask the physics
    // world where this debris is, and the visual-retirement stage (m13.2b) needs it possibly
    // hundreds of ticks later to tell a lighting cache which region to invalidate.
    physics::BodyState st{};
    if (world.get_body_state(rec.body, st)) {
        rec.last_bounds = physics::Aabb{st.position, st.position};
    }
    world.destroy_body(rec.body);
    if (rec.compound.is_valid()) {
        (void)world.unregister_compound(rec.compound);
        rec.compound = physics::CompoundId{};
    }
    // Keep the roster RECORD (append-only ⇒ debris ids never shift ⇒ state_hash stays
    // reproducible); a frozen debris simply reads a null body from here on. A per-part render leaf,
    // when M8.6 adds one, can outlive the physics body at its last pose — the point of "freeze" vs
    // "despawn".
    rec.body = physics::BodyId{};
    rec.phase = DebrisPhase::Frozen;
}

void DestructionWorld::Impl::enforce_debris_budget(physics::PhysicsWorld& world) {
    const std::uint64_t now = tick;

    // 1) Linger → freeze. Any SETTLED debris that has rested at least freeze_delay_ticks is
    //    reclaimed. Ascending roster order keeps it deterministic; a still-Falling (or already
    //    Frozen) debris is skipped — freezing a moving body would pop it out of the air.
    for (std::size_t d = 0; d < debris.size(); ++d) {
        const Debris& rec = debris[d];
        if (rec.phase != DebrisPhase::Settled) {
            continue;
        }
        if (now - rec.settled_tick >= lifecycle.freeze_delay_ticks) {
            freeze_debris(d, world);
        }
    }

    // 2) Live-body cap. While more than max_live_debris bodies are still live, reclaim the "least
    //    interesting" SETTLED one — highest cooked base × size × age (age = ticks since it
    //    settled), so big, long-settled rubble sheds before small or fresh debris. Only settled
    //    debris are eligible; a Falling piece is mid-motion. The victim is a strict > on the score
    //    with the ascending scan as the tiebreak — a total order, so no float tie and no iteration
    //    order can leak in. Camera distance is deliberately NOT a factor: it is per-view and would
    //    break determinism (ADR-0029 §8). If nothing is settled, the cap is momentarily soft (we
    //    never freeze a moving body); it is re-checked every tick and catches up as debris come to
    //    rest.
    constexpr float kBaseScore = 1.0f; // a per-pattern authored priority is a later refinement
    while (live_debris_count() > lifecycle.max_live_debris) {
        std::size_t victim = std::numeric_limits<std::size_t>::max();
        float best = -1.0f;
        for (std::size_t d = 0; d < debris.size(); ++d) {
            const Debris& rec = debris[d];
            if (rec.phase != DebrisPhase::Settled) {
                continue;
            }
            const float age = static_cast<float>(now - rec.settled_tick);
            const float score = kBaseScore * debris_size(rec) * age;
            if (score > best) {
                best = score;
                victim = d;
            }
        }
        if (victim == std::numeric_limits<std::size_t>::max()) {
            break; // no settled debris to shed right now — soft cap under an active burst
        }
        freeze_debris(victim, world);
    }

    // 3) VISUAL budget (m13.2b — ADR-0032 C6). Past it, retire the OLDEST frozen debris until the
    //    visible population is back under the cap.
    //
    //    Oldest-first by roster index, not by the size×age score the live cap uses, and the
    //    difference is deliberate. The live cap is shedding SIMULATION cost, so it wants to drop
    //    whatever is least interesting to keep simulating. This is shedding VISUAL cost from a set
    //    of things that are all equally static — so the only fair rule is the one a player would
    //    predict: the wreckage that has been lying there longest goes first. Roster index IS
    //    creation order (the roster is append-only), so it needs no extra state and stays
    //    deterministic for free.
    //
    //    Only FROZEN debris are eligible. A Falling or Settled piece is still simulating and still
    //    plainly visible; retiring it would delete rubble out from under a player watching it come
    //    to rest. Under a burst the visual cap is therefore soft in exactly the way the live cap
    //    is, and catches up as pieces freeze.
    if (lifecycle.max_visual_debris > 0) {
        while (visual_debris_count() > lifecycle.max_visual_debris) {
            std::size_t victim = std::numeric_limits<std::size_t>::max();
            for (std::size_t d = 0; d < debris.size(); ++d) {
                if (debris[d].phase == DebrisPhase::Frozen) {
                    victim = d;
                    break; // ascending scan ⇒ the first Frozen row is the oldest
                }
            }
            if (victim == std::numeric_limits<std::size_t>::max()) {
                break; // nothing frozen yet — soft, and re-checked next tick
            }
            retire_debris(victim);
        }
    }
}

} // namespace rime::destruction
