// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/vec.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/world.hpp"

// Weapon v1 (m12.3, ADR-0035 §3) — a deterministic hitscan shot, resolved by a single raycast.
//
// WHY HITSCAN AND NOT A PROJECTILE. A projectile is an entity: it needs a body, a lifetime, a
// replication story of its own, and a rule for what happens when it and its target disagree about
// where they are. A hitscan shot is a QUERY — one ray, answered inside the tick that fired it, with
// no state left behind. That is the whole reason it is the right v1: it makes "the shot feels
// connected" (ADR-0035 §1) a property of the same tick that consumed the input, rather than of a
// second object that has to be networked before anyone can tell whether the first one works.
// Projectiles are a later brick, and they inherit this file's aim convention rather than replacing
// it.
//
// WHY IT IS PURE, exactly like step_character. `step_weapon` is a function from (state, input,
// config, observed world) to a new state plus a description of what left the barrel. m12.4 replays
// the last N ticks of input after a correction, and a weapon that remembered anything outside
// `WeaponState` would fire a different number of shots on the replay than it did the first time —
// a desync that presents as "sometimes a shot vanishes" rather than as a failing test. So the same
// rule holds here: if step_weapon reads it, it is in WeaponState.
//
// WHAT IS DELIBERATELY NOT HERE, because leaving it out is a decision rather than an oversight:
//
//   * NO LAG COMPENSATION. The shot is resolved against the world as it stands on the tick the
//     server consumed the command, not against the world as the shooter saw it RTT/2 ago. So a
//     player shooting a moving target must lead it by their own latency. ADR-0035 §4 rules this
//     out of M12 on the grounds that the block's targets are buildings, and buildings do not dodge;
//     the machinery it needs (a clock offset, a rewound history of every hitbox) is named there as
//     future work rather than quietly missing.
//   * NO SPREAD, NO RECOIL, NO AMMO. Each is a game rule wearing an engine feature's clothes, and
//     each would want a random source — which, on a predicted path, has to be a seeded stream both
//     peers agree on. That is a real design and it does not belong in the brick that first proves
//     a shot crosses the wire at all.
//   * NO DAMAGE APPLICATION. This module never links `rime::destruction`; see the fan-out note in
//     CMakeLists.txt. A shot reports WHAT it hit and the consumer decides what that means — which
//     is what lets the same weapon shoot a destructible wall in one game and a health bar in
//     another. `gameplay_net` (m12.3) carries the glue for the first case.
namespace rime::gameplay {

// The action bit that pulls the trigger, and the second (and last) bit the ENGINE assigns meaning
// to — see the registry note on kActionJump in character.hpp. Everything above bit 1 is the game's.
// It is a WeaponConfig field rather than a hard-coded constant so a game that wants three weapons
// on three bits is not fighting the engine for bit 1.
inline constexpr std::uint32_t kActionFire = 1u << 1;

// ── The aim ───────────────────────────────────────────────────────────────────────────────────

// Where a shot starts and which way it points. `direction` need not be unit length — every consumer
// normalizes — but it must be non-degenerate, and step_weapon refuses (and counts) one that is not.
struct Aim {
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
};

// The view direction for a (yaw, pitch) pair, in ONE place so the mover, the weapon, and m12.8's
// camera cannot drift apart.
//
// The convention is the engine's (render/components.hpp), continued from character.cpp's yaw
// basis: forward at yaw = 0 is -Z, yaw turns about world +Y right-handed, and PITCH IS POSITIVE
// LOOKING UP — a rotation about the character's own right axis. Writing it out:
//
//     right   = ( cos y, 0, -sin y)
//     forward = (-sin y, 0, -cos y)          the mover's ground-plane forward
//     aim     = forward * cos p + up * sin p
//
// Note that the mover uses `yaw` and ignores `pitch` entirely (character.hpp); this function is
// where the ignored half finally means something. That is why `pitch` has been on the wire and on
// the replay tape since m11.6c — so a tape recorded before weapons existed still replays a
// complete intent now that they do.
[[nodiscard]] core::Vec3 aim_direction(float yaw, float pitch) noexcept;

// ── The configuration ─────────────────────────────────────────────────────────────────────────

// Constant per weapon, agreed out of band by both peers exactly as CharacterConfig is: never
// replayed, never replicated per tick.
struct WeaponConfig {
    float range = 120.0f; // metres; the ray's max_distance, and the miss boundary

    // Health removed at the hit point, and the radius the consumer's damage call should use.
    //
    // THE SCALE IS NORMALIZED, and the default is chosen against it rather than guessed. A cooked
    // destructible part stands at 1.0 health (destruction/world.hpp: "every part alive, full (1.0)
    // health"), so 0.5 means two clean hits take a part out of a wall and a glancing one merely
    // scars it. Authoring this in "hit points" — 45, say — is not wrong so much as it is a claim
    // about a health scale this engine does not have, and it shows up as a rifle that levels a
    // building with one round. (It did: the first draft of this header said 45, and the m12.3
    // end-to-end proof deleted all sixteen parts of its test wall on the opening shot.)
    //
    // The radius is SMALL BY DESIGN and it is not a spread model. `destruction::apply_damage` is a
    // radius call with a linear falloff (world.hpp), so a radius of exactly 0 would deposit damage
    // nowhere at all; a few centimetres makes the shot bite the part it hit and essentially nothing
    // else. An explosion is the same call with a large radius, which is why there is no second API.
    float damage = 0.5f;
    float damage_radius = 0.06f;

    // World-space impulse magnitude carried into whatever the shot dislodges, applied ALONG THE
    // SHOT, not along the surface normal: a bullet pushes the way it was already travelling, and
    // pushing along the normal makes every hit on a sloped wall shove debris uphill.
    //
    // Scaled against the same fixtures: the destruction suite tips a wall's upper slab off its
    // stump with an impulse of 30 kg·m/s, so 25 on a single dislodged part is a solid shove rather
    // than a launch.
    float impulse = 25.0f;

    // How high above the capsule's ORIGIN (its centre — character.hpp) the ray starts. Shooting
    // from the centre is a real bug and a subtle one: a wall the player can plainly see over is
    // chest-high to a ray fired from the navel, so half the shots at a low cover edge stop dead in
    // the cover. Defaults to the top of the cylinder, i.e. eye level for the default capsule.
    float eye_height = 0.5f;

    std::uint32_t fire_bit = kActionFire;

    // Ticks between shots: `cooldown_ticks = N` fires at most once every max(N, 1) ticks, so 6 at
    // 60 Hz is ten shots a second. 0 and 1 both mean "as fast as the tick rate" — the counter a
    // shot sets is already back at zero by the time the next tick reads it (weapon.cpp derives it).
    //
    // In TICKS rather than seconds, and the reason is m12.4: a float cooldown is a value that must
    // survive being restored from a server correction and then re-advanced, and every restore is a
    // quantization of a number that was exact when it left. An integer tick count is exactly
    // representable, replays bit-for-bit, and reads the same on both peers.
    std::uint32_t cooldown_ticks = 6;

    // Semi-automatic (the default) fires on the `pressed` EDGE: one trigger pull, one shot,
    // whatever the tick rate. Automatic fires while `held` is set, gated only by the cooldown.
    //
    // The distinction is exactly the one input.hpp's held/pressed split was built for, and it is
    // the reason the redundancy window exists: a lost `pressed` edge is a shot that never happened
    // and no later packet repairs it, whereas a lost `held` level is restated by the next sample.
    bool automatic = false;
};

// Clamp a config into the contract its fields advertise, reporting whether anything had to move —
// the replication::sanitize / gameplay::validate pattern. Non-finite fields are REPLACED rather
// than clamped, because a NaN compares false against every bound and would otherwise sail through
// a validator that looks like it works.
bool validate(WeaponConfig& config) noexcept;

// ── The state ─────────────────────────────────────────────────────────────────────────────────

// The COMPLETE tick-to-tick memory of a weapon — the WeaponState half of the purity rule above.
// m12.4's rewind restores exactly this struct, so anything a future feature needs to remember
// (ammo, a burst counter, a reload timer) extends it rather than sitting beside it.
struct WeaponState {
    std::uint32_t cooldown = 0; // ticks remaining before the trigger will answer again
};

// ── What the tick produced ────────────────────────────────────────────────────────────────────

// A shot left the barrel. Emitted whether or not it hit anything, because a miss still has a
// muzzle flash, a tracer and a report — the m12.6 FX families read this, and a `WeaponFired` that
// only existed on hits would make missing silent.
struct WeaponFired {
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    core::Vec3 direction{0.0f, 0.0f, -1.0f}; // unit
    float range = 0.0f;                      // how far the ray was allowed to travel
};

// What a shot struck. `child` is the compound child index the ray pierced, which on an INTACT
// destructible is the part index (query.hpp's RayHit::child, ADR-0029's remap table): that
// equivalence is the whole reason hitscan can name a part without this module knowing what a part
// is. A consumer translates body → its own instance and fires the damage call.
struct HitResult {
    physics::BodyId body{};
    core::Vec3 point{0.0f, 0.0f, 0.0f};
    core::Vec3 normal{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
    std::uint16_t child = 0;
};

// One tick's output. `fired` gates `shot`; `hit` is meaningful only when `did_hit` is set, and the
// two are separate booleans rather than one tri-state because "fired and missed" is a real,
// common, and visually distinct outcome from "did not fire".
struct ShotResult {
    bool fired = false;
    bool did_hit = false;
    WeaponFired shot{};
    HitResult hit{};
};

// Deterministic counts of what the trigger actually did — the StepStats discipline (guardrail 5)
// applied to the weapon, for the same reason: a proof that cannot see the shots that were refused
// reads exactly like one where every shot landed.
//
// NOT part of WeaponState: never replayed, never replicated, and changing it can never change what
// the weapon does.
struct FireStats {
    std::uint32_t intents = 0;          // ticks the fire intent was present at all
    std::uint32_t shots = 0;            // shots that actually left the barrel
    std::uint32_t refused_cooldown = 0; // intent present, weapon not ready yet
    std::uint32_t refused_aim = 0; // a degenerate or non-finite aim — the barrel points nowhere
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
};

// ── The step ──────────────────────────────────────────────────────────────────────────────────

// The aim a character with this config is holding this tick. The character↔weapon bridge, kept a
// separate function so a turret, a vehicle mount or a test can call step_weapon with an aim that
// came from somewhere else entirely.
[[nodiscard]] Aim character_aim(const CharacterState& state,
                                const CharacterInput& input,
                                const WeaponConfig& config) noexcept;

// Advance one weapon by one FIXED tick: age the cooldown, decide whether the trigger answers, and
// if it does, resolve the shot with one raycast.
//
// PURE, on the same terms as step_character: the same (state, input, config, aim, world-as-
// observed, self) yields the same result bit for bit on the same binary. It reads the world only
// through `raycast`, never moves a body, and keeps no memory outside `state`.
//
// `self` is the shooter's own body, excluded from the cast. Without it every shot hits the
// shooter's own capsule at distance ~0 — the exact failure QueryFilter::exclude was added for in
// m12.2, arriving here for the second time.
//
// `out` and `stats` are optional and write-only; passing null for either costs nothing.
[[nodiscard]] WeaponState step_weapon(const WeaponState& state,
                                      const CharacterInput& input,
                                      const WeaponConfig& config,
                                      const Aim& aim,
                                      const physics::PhysicsWorld& world,
                                      physics::BodyId self,
                                      ShotResult* out = nullptr,
                                      FireStats* stats = nullptr) noexcept;

} // namespace rime::gameplay
