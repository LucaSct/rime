// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay/weapon.hpp"

#include <algorithm>
#include <cmath>

#include "rime/physics/query.hpp"

// Weapon v1 (m12.3) — the implementation. The header carries the design; this file carries the
// three numerical decisions the design leaves open, each argued where it is made:
//
//   1. what "the trigger answers" means when the intent is an edge, a level, or both at once;
//   2. what a degenerate aim does (it refuses, and says so);
//   3. why the cooldown ages BEFORE the trigger is read rather than after.
namespace rime::gameplay {
namespace {

constexpr core::Vec3 kUp{0.0f, 1.0f, 0.0f};

[[nodiscard]] bool finite(float x) noexcept {
    return std::isfinite(x);
}

[[nodiscard]] bool finite(core::Vec3 v) noexcept {
    return finite(v.x) && finite(v.y) && finite(v.z);
}

} // namespace

core::Vec3 aim_direction(float yaw, float pitch) noexcept {
    // Non-finite angles collapse to the neutral pose rather than propagating a NaN into a raycast.
    // The cast would answer "no hit" for a NaN direction on most paths and "something absurd" on
    // the rest, and neither is a thing a caller can act on.
    const float y = finite(yaw) ? yaw : 0.0f;
    // Pitch is clamped to just inside the poles. Exactly ±π/2 is a legitimate direction (straight
    // up, straight down) but it is also the one place where the horizontal component vanishes, and
    // a camera built on this basis later needs a non-degenerate horizontal to derive `right` from.
    // Clamping here, once, is cheaper than every consumer discovering the pole separately.
    constexpr float kMaxPitch = 1.5697963f; // π/2 - 1e-3
    const float p = std::clamp(finite(pitch) ? pitch : 0.0f, -kMaxPitch, kMaxPitch);

    const float sy = std::sin(y);
    const float cy = std::cos(y);
    const float sp = std::sin(p);
    const float cp = std::cos(p);

    // forward = (-sin y, 0, -cos y), exactly as character.cpp derives it; pitch tilts it toward up.
    // Unit by construction (forward and up are orthonormal), so no normalize is needed and none is
    // done — a normalize here would only add a rounding step the caller cannot see.
    return core::Vec3{-sy * cp, sp, -cy * cp};
}

bool validate(WeaponConfig& config) noexcept {
    const WeaponConfig defaults{};
    bool changed = false;

    // Non-finite by REPLACEMENT before any clamping — the CharacterConfig rule, for the same
    // reason: a NaN compares false against every bound, so a clamp alone passes it through.
    const auto fix = [&](float& field, float fallback) {
        if (!finite(field)) {
            field = fallback;
            changed = true;
        }
    };
    fix(config.range, defaults.range);
    fix(config.damage, defaults.damage);
    fix(config.damage_radius, defaults.damage_radius);
    fix(config.impulse, defaults.impulse);
    fix(config.eye_height, defaults.eye_height);

    const auto clamp_to = [&](float& field, float lo, float hi) {
        const float v = std::clamp(field, lo, hi);
        if (v != field) {
            field = v;
            changed = true;
        }
    };
    // A shot with no range at all is a trigger that can never hit anything, which is a
    // configuration bug that would otherwise present as "the gun is broken". One millimetre is the
    // floor; 10 km is well past any level and short of the float range where a ray's parametric
    // arithmetic starts losing digits.
    clamp_to(config.range, 1e-3f, 1.0e4f);
    // The ceiling is deliberately far above the 1.0-per-part scale the engine cooks: "how much
    // health is a lot" is the game's call, and a validator that enforced a design budget would be
    // the engine legislating one. What it refuses is only the absurd.
    clamp_to(config.damage, 0.0f, 1.0e6f);
    // The radius floor matters: apply_damage's falloff is linear to zero at `radius`, so a radius
    // of 0 deposits exactly nothing however large `damage` is.
    clamp_to(config.damage_radius, 1e-3f, 100.0f);
    clamp_to(config.impulse, 0.0f, 1.0e7f);
    // The eye may sit below the origin (a prone or crouched mount) but not outside a plausible
    // human-scale capsule in either direction.
    clamp_to(config.eye_height, -4.0f, 4.0f);

    if (config.fire_bit == 0u) {
        // A zero bit mask is a trigger no input can ever pull. Silently accepting it would make a
        // mis-authored weapon indistinguishable from a working one that nobody happened to fire.
        config.fire_bit = defaults.fire_bit;
        changed = true;
    }
    // Ticks are integral, so there is nothing to clamp downward; the cap keeps a mis-authored
    // cooldown from parking the weapon for the rest of the match.
    if (config.cooldown_ticks > 3600u) {
        config.cooldown_ticks = 3600u;
        changed = true;
    }
    return changed;
}

Aim character_aim(const CharacterState& state,
                  const CharacterInput& input,
                  const WeaponConfig& config) noexcept {
    WeaponConfig cfg = config;
    (void)validate(cfg); // clamped locally; the caller's copy is theirs (the step_character rule)

    Aim aim;
    // The eye rides the capsule ORIGIN, which is its centre, so the offset is up by eye_height.
    // Using the state's position rather than a body pose is deliberate: the shot must be resolved
    // against the pose the mover just certified this tick, not against whatever the physics body
    // happens to hold before push_in drives it there.
    aim.origin = state.position + kUp * cfg.eye_height;
    if (!finite(aim.origin)) {
        aim.origin = core::Vec3{};
    }
    aim.direction = aim_direction(input.yaw, input.pitch);
    return aim;
}

WeaponState step_weapon(const WeaponState& state,
                        const CharacterInput& input,
                        const WeaponConfig& config,
                        const Aim& aim,
                        const physics::PhysicsWorld& world,
                        physics::BodyId self,
                        ShotResult* out,
                        FireStats* stats) noexcept {
    ShotResult discard_result;
    FireStats discard_stats;
    ShotResult& result = out != nullptr ? *out : discard_result;
    FireStats& st = stats != nullptr ? *stats : discard_stats;
    result = ShotResult{};

    WeaponConfig cfg = config;
    (void)validate(cfg);

    WeaponState s = state;

    // ── (a) The cooldown ages FIRST ───────────────────────────────────────────────────────────
    //
    // Order matters, and the wrong order is invisible in a test that fires once. Ageing before the
    // trigger is read makes the contract exactly what the field name says:
    //
    //     cooldown_ticks = N  ⇒  at most one shot every max(N, 1) ticks.
    //
    // (N = 0 and N = 1 both mean "every tick": a shot sets the counter to N, and the very next
    // tick's ageing has already returned it to 0.) Ageing AFTER the trigger would spend one extra
    // tick on every shot, so the same N would buy a period of N+1 — the same authored number
    // meaning a different rate of fire because of where one line sits, which is the kind of thing
    // a designer discovers with a stopwatch instead of by reading.
    if (s.cooldown > 0u) {
        --s.cooldown;
    }

    // ── (b) Does the trigger answer? ──────────────────────────────────────────────────────────
    //
    // Semi-automatic reads the EDGE, automatic reads the LEVEL. An automatic weapon deliberately
    // also accepts the edge: `pressed` and `held` are both set on the tick a key goes down
    // (input.hpp's sampler guarantees it), so this is belt-and-braces against a game that builds
    // commands by hand and sets only one of the two.
    const bool edge = (input.pressed & cfg.fire_bit) != 0u;
    const bool level = (input.held & cfg.fire_bit) != 0u;
    const bool intent = cfg.automatic ? (level || edge) : edge;
    if (!intent) {
        return s;
    }
    ++st.intents;

    if (s.cooldown > 0u) {
        // Counted rather than silent. A weapon that refuses every shot because a config left the
        // cooldown at some absurd value looks exactly like one nobody fired, and this is the number
        // that tells the two apart.
        ++st.refused_cooldown;
        return s;
    }

    const float dir_len2 = core::length_squared(aim.direction);
    if (!finite(aim.origin) || !finite(dir_len2) || dir_len2 <= 0.0f) {
        // The barrel points nowhere. Refusing costs the shooter a shot; firing along a normalized
        // NaN would put a garbage ray through the broadphase, and the answer that came back would
        // be acted on as if it meant something.
        ++st.refused_aim;
        return s;
    }
    const core::Vec3 direction = aim.direction * (1.0f / std::sqrt(dir_len2));

    // ── (c) The shot ──────────────────────────────────────────────────────────────────────────
    s.cooldown = cfg.cooldown_ticks;
    ++st.shots;
    result.fired = true;
    result.shot.origin = aim.origin;
    result.shot.direction = direction;
    result.shot.range = cfg.range;

    physics::Ray ray;
    ray.origin = aim.origin;
    ray.direction = direction;
    ray.max_distance = cfg.range;

    physics::QueryFilter filter;
    filter.exclude = self; // or every shot hits the shooter's own capsule at distance ~0

    physics::RayHit hit;
    if (!world.raycast(ray, hit, filter)) {
        ++st.misses;
        return s; // a miss still fired: `result.fired` stays true, `did_hit` stays false
    }

    ++st.hits;
    result.did_hit = true;
    result.hit.body = hit.body;
    result.hit.point = hit.point;
    result.hit.normal = hit.normal;
    result.hit.distance = hit.distance;
    result.hit.child = hit.child;
    return s;
}

} // namespace rime::gameplay
