// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/world.hpp"

// The character controller (m12.2, ADR-0035 §3) — how a player-shaped capsule moves through a
// simulated world without being simulated by it.
//
// WHY IT IS NOT A RIGID BODY. A dynamic rigid body with a capsule shape is a terrible player: it
// tips over, it bounces off stairs, it slides down every ramp, and its response to a wall is
// whatever the solver's friction and restitution happen to produce that tick. What players expect
// instead is precise, authored motion that STOPS at walls, SLIDES along them, CLIMBS steps, and
// STICKS to slopes. So a controller is a kinematic body whose pose the game computes with SHAPE
// CASTS — "if I move this capsule this way, what do I hit first?" — and the technique for turning
// a blocked move into a sensible one is COLLIDE-AND-SLIDE: advance to the first contact, project
// the leftover motion onto the surface, repeat. It is the same idea Quake shipped in 1996, and it
// is still the right one, because it is the direct answer to "the player asked to go there and
// something is in the way".
//
// WHY IT IS PURE. `step_character` is a function from (state, input, config, observed world) to a
// new state. That is not stylistic: m12.4 wants client-side prediction, which means replaying the
// last N ticks of input from a corrected state and getting BIT-IDENTICAL results. Anything the
// controller remembers between ticks that is not in `CharacterState` — a cached ground normal, a
// coyote-time counter, an accumulator — silently breaks that replay, and breaks it in the form of
// a rare desync rather than a failing test. So the rule is stated and testable: if step_character
// reads it, it is in CharacterState.
//
// DETERMINISM SCOPE: SAME BINARY. Two runs of the same executable produce identical bits.
// Cross-platform bit-identity is a recorded NON-GOAL (ADR-0033, docs/ROADMAP.md) — m12.1 measured
// x86-64 and arm64 rounding a shape cast's advance differently — and nothing here upgrades that
// claim. `std::sqrt` is correctly rounded by IEEE-754; `std::sin`/`std::cos` (the yaw basis) are
// not specified to be, but are stable within one binary, which is the whole promise.
namespace rime::gameplay {

// ── The tape entry ────────────────────────────────────────────────────────────────────────────

// One tick of player intent, and the REPLAY TAPE entry. It mirrors replication::InputCommand by
// VALUE SHAPE — the same fields, the same meanings — minus `sequence`, which is the network's
// bookkeeping and not the mover's. Deliberately mirrored rather than reused: linking gameplay to
// replication would mean a single-player build cannot delete the networking stack, which
// guardrail 2 promises it can.
//
// Recorded and replayed as raw float BITS. A tape is exact by construction, not by a quantization
// policy that has to be got right; whatever the wire does to these numbers happens before they
// arrive here.
struct CharacterInput {
    float move_x = 0.0f; // intent on the ground plane, nominally the unit disc: +x is right …
    float move_y = 0.0f; // … +y is forward. NOT a velocity — how fast it moves you is config's.
    float yaw = 0.0f;    // ABSOLUTE radians about world +Y. The mover uses only this.
    // Carried for the tape and for the weapon that lands in m12.3; the mover ignores it entirely.
    // It is here so that a tape recorded today replays a full intent later, rather than needing a
    // second parallel stream bolted on when aiming arrives.
    float pitch = 0.0f;
    std::uint32_t held = 0;    // actions held (a level — self-healing across a dropped packet)
    std::uint32_t pressed = 0; // actions that went down this tick (an edge — exists in one tick)
};

// Action bits for `held` / `pressed`. The engine transports every bit and assigns meaning to
// exactly TWO of them — this one and `kActionFire` (weapon.hpp, bit 1). Everything from bit 2 up is
// the game's, and both engine bits are configurable at their consumer (WeaponConfig::fire_bit), so
// a game that wants bit 1 for something else is not fighting the engine for it.
inline constexpr std::uint32_t kActionJump = 1u << 0;

// ── The state ─────────────────────────────────────────────────────────────────────────────────

// The COMPLETE tick-to-tick memory of a character. See the purity note at the top of this file:
// m12.4's rewind-and-replay restores exactly this struct and nothing else, so a future feature
// that needs memory (crouch, coyote time, a jump buffer) must EXTEND THIS STRUCT rather than
// stash state beside it. There are deliberately no timers and no accumulators here yet.
struct CharacterState {
    core::Vec3 position{0.0f, 0.0f, 0.0f}; // the capsule's origin — its centre, as a body's is
    core::Vec3 velocity{0.0f, 0.0f, 0.0f};
    bool grounded = false;
    // The supporting surface's normal. Meaningful only while `grounded`; it is the basis the
    // ground-plane projection and the slide use, which is why it is state rather than a value
    // recomputed from a fresh cast each tick (that cast would disagree the moment the capsule is
    // between two surfaces).
    core::Vec3 ground_normal{0.0f, 1.0f, 0.0f};
};

// ── The configuration ─────────────────────────────────────────────────────────────────────────

// Constant per character, and NOT state: both sides of a networked session agree on it out of
// band (m12.3 ships it as authored data), so it is never replayed and never replicated per tick.
struct CharacterConfig {
    float radius = 0.4f;      // capsule radius
    float half_height = 0.5f; // CYLINDER half-height; total height is 2*(half_height + radius)

    // m/s on flat ground. On a slope the same budget buys max_speed * cos(slope) ALONG the
    // surface — climbing costs speed, by design and with no separate rule (character.cpp).
    float max_speed = 6.0f;
    float accel = 50.0f;    // m/s² toward the wish velocity, on the ground
    float air_accel = 8.0f; // …and in the air, where a player has much less authority
    float gravity = 9.81f;
    float jump_speed = 4.5f; // the vertical speed a jump sets outright (not an impulse)

    // Walkable iff dot(ground_normal, up) >= this. A DIMENSIONLESS comparison — a cosine of two
    // unit vectors — so unlike every length below it is legitimately an absolute constant, and
    // scaling it with anything would be meaningless. Default: 45°.
    float max_slope_cos = 0.70710678f;

    float step_height = 0.3f;   // the tallest riser the step-up ladder will climb
    float snap_distance = 0.3f; // how far down the ground snap reaches; step_height is the sane
                                // default (you can step down what you could step up)
    float skin = 0.02f;         // the CONTACT OFFSET — see the numerics note in character.cpp
    int max_slide_iterations = 4;
    // Bounds the recovery teleport rate, in metres per tick. Depenetration is a position write
    // with no velocity behind it, so an unbounded one is a teleport; capping it turns "shoved 2 m
    // into a wall" into a visible slide out over ten ticks instead of a jump cut.
    float max_depenetration_per_tick = 0.2f;
};

// Clamp a config into the contract its fields advertise and report whether anything had to move
// (the replication::sanitize pattern: a value that needed correcting is worth a return value
// rather than a silent fix). Non-finite fields are REPLACED, not clamped — a NaN compares false
// against every bound, which is how "we validate our inputs" quietly becomes untrue.
//
// The one relationship worth naming: `skin` is held in [1e-3, 0.25 * radius]. Too small and it is
// inside the query error floor, so a cast can report a stop the capsule is already past; too
// large and the capsule floats visibly off every surface.
bool validate(CharacterConfig& config) noexcept;

// ── The per-tick instrument panel ─────────────────────────────────────────────────────────────

// Deterministic counts of what one tick actually did. Every skip, give-up, and fallback path in
// the controller has a number here — guardrail 5's discipline applied outside replication, for
// the same reason: a proof that cannot see what it skipped reads exactly like a passing one.
//
// NOT part of CharacterState: never replayed, never replicated, and changing it can never change
// the trajectory. `stuck` and `slide_exhausted` are the two to watch — they are the failure modes
// that would otherwise present as "the player froze" with nothing to point at.
struct StepStats {
    std::uint32_t casts = 0;            // world queries issued (a vacuity witness for the tests)
    std::uint32_t slide_iterations = 0; // iterations of the collide-and-slide loop
    std::uint32_t slide_exhausted = 0;  // hit max_slide_iterations with motion left over
    std::uint32_t steps_climbed = 0;    // step-up accepted
    std::uint32_t step_rejected = 0;    // step-up tried and refused (too tall, or no floor above)
    std::uint32_t snaps = 0;            // ground snap placed the capsule on a surface
    std::uint32_t depenetrations = 0;   // recovery pushes performed
    std::uint32_t stuck = 0;            // STILL overlapping after recovery — the freeze, counted
    std::uint32_t corner_locked = 0;    // a third incompatible plane zeroed the velocity
    // A contact the shape cast reported that the depenetration query could not confirm — see
    // "PHANTOM CONTACTS" in character.cpp. A steady trickle is the known shape-cast weakness on
    // large rotated faces; a flood means something underneath has changed for the worse.
    std::uint32_t phantom_contacts = 0;
};

// ── The step ──────────────────────────────────────────────────────────────────────────────────

// Advance one character by one FIXED tick and return its new state.
//
// PURE: the same (state, input, config, world-as-observed, self, dt) yields the same result, bit
// for bit, on the same binary. It reads the world only through const queries (shape_cast,
// penetration — both documented safe between steps and never during one), never moves a body, and
// keeps no memory outside `state`. `self` is the character's own kinematic body, excluded from
// every query: without that exclusion each cast hits the capsule's own body at distance 0.
//
// `stats` is optional and write-only; passing null costs nothing.
//
// This does NOT touch the physics world. The caller writes the returned position into the
// character entity's WorldTransform; PhysicsSync::push_in then drives the kinematic body there
// with the velocity the move implies, which is what makes the player PUSH a crate rather than
// teleport through it (sync.hpp). Canonical tick order: docs/design/simulation-tick.md.
[[nodiscard]] CharacterState step_character(const CharacterState& state,
                                            const CharacterInput& input,
                                            const CharacterConfig& config,
                                            const physics::PhysicsWorld& world,
                                            physics::BodyId self,
                                            float dt,
                                            StepStats* stats = nullptr) noexcept;

// The capsule a character of this config occupies — the shape every query above poses, and the
// one a caller should give the character's own body so the two agree. Exposed because "what shape
// am I" is a question the ECS wrapper, the tests, and the spawn code all ask.
[[nodiscard]] physics::ShapeDesc character_shape(const CharacterConfig& config) noexcept;

} // namespace rime::gameplay
