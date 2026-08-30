// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"

// Simulation events (M7.9): step()'s per-tick report of what happened, as data the game reads
// AFTER the step rather than callbacks fired during it. Buffered and deterministic on purpose —
// the destruction system (M8) turns contact impulses into damage, so the events must arrive in a
// reproducible order with reproducible payloads (same-binary determinism, ADR-0026): a networked
// or replayed destruction sequence must be a pure function of the physics, event stream included.
//
// Three families ship here — contact, sleep, and (m15.6) TRIGGER events. The third was deferred at
// M7.9 for a good reason and stayed deferred for a bad one: `Collider::sensor` was added anyway,
// reflected, authored and shown in the inspector, and read by nothing — so a designer could tick
// "sensor" on a volume and get a solid wall with no warning. This is that field becoming real.
namespace rime::physics {

// What happened to a contact between a body pair across one tick. Modelled on the standard
// enter/stay/exit lifecycle (Jolt's added/persisted/removed, PhysX/Unity's enter/stay/exit) so the
// familiar damage-on-impact / sustained-crush / released patterns map straight onto it.
enum class ContactPhase : std::uint8_t {
    Began = 0,     // touching this tick, was NOT last tick — the impact (damage usually peaks here)
    Persisted = 1, // touching this tick AND last tick — a sustained/resting contact
    Ended = 2, // touched last tick, separated this tick — impulses are 0 (nothing was exchanged)
};

// One contact REGION between two bodies this tick. For plain (non-compound) bodies a pair is one
// region and this is exactly "one entry per body pair", as it has been since M7.9. A compound body
// (M7.12, ADR-0028) can touch the same other body through several of its children at once — a
// dumbbell standing on two feet — and each touching child pair is its own region with its own
// began/persisted/ended lifecycle. Per-region reporting is the deliberate choice for M8 damage: a
// hit on a destructible must name WHICH part took the impulse (that is the cell fracture
// detaches); a consumer wanting per-pair totals can sum the regions, while the reverse — naming
// the part from an aggregate — is impossible. Emitted only for a pair with a dynamic participant
// (an all-immovable pair exchanges no impulse); a region whose every dynamic member is asleep goes
// silent (a settled pile costs nothing) without being reported as Ended — it never separated.
//
// Conventions mirror the manifold (contact.hpp) exactly, so a consumer never has to re-derive a
// sign: `a`/`b` are in canonical broadphase order (a.index < b.index) and `normal` is unit and
// points FROM a TOWARD b. `child_a`/`child_b` are the compound child indices of the region (0 for
// a non-compound body). An Ended event's bodies may already be dead (destroyed since last tick) —
// check PhysicsWorld::is_alive if it matters.
struct ContactEvent {
    BodyId a;
    BodyId b;
    core::Vec3 point{0.0f, 0.0f, 0.0f};  // representative world point: the deepest contact point
    core::Vec3 normal{0.0f, 1.0f, 0.0f}; // unit, from a toward b
    float normal_impulse = 0.0f;  // TOTAL normal impulse the solver exchanged over this region
                                  // this tick (kg·m/s, >= 0) — the M8 damage signal. 0 for Ended.
    float tangent_impulse = 0.0f; // total friction impulse over this region this tick. 0 for Ended.
    ContactPhase phase = ContactPhase::Began;
    std::uint16_t child_a = 0; // compound child index on each side (M7.12) — the part that was
    std::uint16_t child_b = 0; // hit, the M8 damage-to-part mapping; 0 for a plain body
};

// A body deactivated or reactivated *as a result of a step()*. `Slept` is the basis for M8's
// `DebrisSettled` — a piece has come fully to rest. `Woke` reports a SIMULATION-driven wake (a
// faller landing on and rousing a sleeping stack); an explicit wake_body()/apply_impulse() wake is
// the caller's own action and is intentionally NOT reported as an event (the caller already knows).
enum class SleepPhase : std::uint8_t {
    Slept = 0, // awake -> asleep: velocities zeroed, from next tick skipped by the step
    Woke = 1,  // asleep -> awake: rejoined the simulation
};

struct SleepEvent {
    BodyId body;
    SleepPhase phase = SleepPhase::Slept;
};

// An overlap with a SENSOR body across one tick (m15.6). A sensor takes part in the broadphase and
// the exact narrowphase like any other body, so this is real geometry rather than an AABB guess —
// but it is skipped by the solver, so it exchanges no impulse, joins no island, and cannot push or
// wake anything.
//
// It carries WHO and WHICH PHASE and nothing else, deliberately: there is no contact point, no
// normal and no impulse, because nothing was solved. A ContactEvent with those fields zeroed would
// read like a contact that happened to be gentle.
//
// `a`/`b` are in the same canonical order as ContactEvent (a.index < b.index), which means EITHER
// may be the sensor — a consumer that wants "what entered my trigger" checks both ends. Phases are
// the same enter/stay/exit lifecycle, computed by the same merge, so `Began`/`Ended` are the pair
// that matters and `Persisted` is every tick in between.
//
// Unlike contacts, a trigger is NOT suppressed when the bodies inside it are asleep: an overlap
// does not stop existing because nothing is moving, and a kinematic character — the archetypal
// thing a trigger watches for — is never an "awake dynamic body" at all.
struct TriggerEvent {
    BodyId a;
    BodyId b;
    ContactPhase phase = ContactPhase::Began;
    std::uint16_t child_a = 0; // the touching child within a compound (0 for a plain body)
    std::uint16_t child_b = 0;
};

} // namespace rime::physics
