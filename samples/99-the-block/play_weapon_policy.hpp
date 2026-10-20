// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "play_weapon_motion.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/weapon.hpp"

namespace rime::block_play {

// Shared capsule-relative eye policy. Rendering and authoritative firing configure their
// existing eye-height fields from this function so changing capsule height cannot separate them.
[[nodiscard]] inline float play_eye_height(const gameplay::CharacterConfig& character) noexcept {
    return character.half_height * .8f;
}

// The play sample's three guns, expressed ONLY through the engine's existing trigger contract.
//
// WHY THIS IS A CONFIGURATION AND NOT AN INVENTORY. The authoritative weapon is one
// gameplay::WeaponConfig on the server, and step_weapon already knows the two things that make a
// rifle feel different from a bolt-action: a cooldown in ticks and the held-vs-pressed trigger
// rule. Selecting a gun therefore rewrites those two fields and nothing else. There is no
// replicated "which gun" state, no ammunition and no magazine: the sample has infinite ammo, and
// reload/bolt are presentation timelines (play_weapon_motion.hpp) whose only gameplay effect is
// the busy filter below. A multiplayer game would need per-player weapon state in WeaponState;
// this is deliberately not that design.
//
// WHY THE RATES ARE TICKS. WeaponConfig::cooldown_ticks = N fires at most once every N ticks, so
// at the sample's 60 Hz simulation the authored rates are exact integers:
//   SCAR-H  8 ticks -> 450 rounds/min (a heavy 7.62 rifle, deliberately slower than the M4)
//   M4A1    7 ticks -> ~514 rounds/min
//   MSR    72 ticks ->  50 rounds/min, semi-automatic: one trigger PRESS per shot, and the bolt
//                     presentation (1.1 s) fits inside the 1.2 s cooldown.
inline constexpr std::uint32_t kPlaySimHz = 60;
inline constexpr std::uint32_t kScarCooldownTicks = 8;
inline constexpr std::uint32_t kM4CooldownTicks = 7;
inline constexpr std::uint32_t kSniperCooldownTicks = 72;

[[nodiscard]] constexpr float rounds_per_minute(std::uint32_t cooldown_ticks,
                                                std::uint32_t sim_hz = kPlaySimHz) noexcept {
    // validate() treats 0 and 1 alike ("every tick"), so the period is max(N, 1) ticks.
    return 60.0f * static_cast<float>(sim_hz) /
           static_cast<float>(cooldown_ticks > 1u ? cooldown_ticks : 1u);
}

// The bolt's presentation must finish before the server would accept the next press, or a fast
// second click would fire while the hand is still on the bolt.
static_assert(PlayWeaponMotion::kBoltSeconds * kPlaySimHz < kSniperCooldownTicks);

// Rewrites only the trigger rule and cadence of an existing config: range, damage, impulse, eye
// height and fire_bit are the game's, and a selection must not reset them.
inline void configure_selected_weapon(gameplay::WeaponConfig& config,
                                      PlayWeaponMotion::Weapon weapon) noexcept {
    using Weapon = PlayWeaponMotion::Weapon;
    config.automatic = weapon != Weapon::Sniper;
    config.cooldown_ticks = weapon == Weapon::Sniper ? kSniperCooldownTicks
                            : weapon == Weapon::Scar ? kScarCooldownTicks
                                                     : kM4CooldownTicks;
}

// While a reload or bolt cycle is on screen the trigger is dead. BOTH the held level and the
// pressed edge are removed: clearing only `held` would let an automatic rifle still fire on the
// edge (step_weapon accepts either), and clearing only `pressed` would let it fire on the level.
//
// No shot is QUEUED. A press swallowed here is gone; when the action ends, a semi-automatic gun
// needs a fresh press, and an automatic one fires only because the trigger is still held -- a
// level the player is restating every tick, not a remembered request. Everything else in the
// command (movement, look, jump and any game-defined bit) passes through untouched, so a player
// can reload while running and jumping.
[[nodiscard]] inline gameplay::CharacterInput
filter_busy_fire(gameplay::CharacterInput input,
                 bool busy,
                 std::uint32_t fire_bit = gameplay::kActionFire) noexcept {
    if (busy) {
        input.held &= ~fire_bit;
        input.pressed &= ~fire_bit;
    }
    return input;
}

} // namespace rime::block_play
