// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>

#include "play_weapon_motion.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/weapon.hpp"

namespace rime::block_play {

// Magazines for the --play weapons: one count per gun, pure logic with no GPU and no clock.
//
// WHO OWNS THE COUNT. The engine's authoritative gameplay::WeaponConfig has no ammunition (see
// play_weapon_policy.hpp: its cooldown and trigger rule are all it knows), and that module is not
// this sample's to change. So the sample keeps the ledger beside the authority it already has:
// shots are CONSUMED in the per-tick loop over the server's accepted shots (main.cpp step_sim) and
// an empty trigger is REFUSED in the per-tick input filter (step_play), so neither depends on the
// render frame rate. The refill is the one presentation-timed event: it fires from the reload
// animation's "magazine seated" instant (PlayWeaponMotion::take_mag_seated).
//
// Reserve ammunition is unlimited on purpose: no pickups exist yet, so a finite reserve could only
// ever run out and strand the player. A reload therefore always refills to capacity.
class PlayAmmo {
public:
    using Weapon = PlayWeaponMotion::Weapon;

    // The ONE per-weapon table: rifle-class guns (SCAR-H, M4A1) hold 30, the MSR sniper holds 5.
    static constexpr std::array<std::uint32_t, 3> kCapacity{30, 30, 5};

    [[nodiscard]] static constexpr std::uint32_t capacity(Weapon weapon) noexcept {
        return kCapacity[static_cast<unsigned>(weapon)];
    }

    [[nodiscard]] std::uint32_t rounds(Weapon weapon) const noexcept {
        return rounds_[static_cast<unsigned>(weapon)];
    }

    [[nodiscard]] bool empty(Weapon weapon) const noexcept { return rounds(weapon) == 0; }

    [[nodiscard]] bool full(Weapon weapon) const noexcept {
        return rounds(weapon) >= capacity(weapon);
    }

    // R only starts a reload when there is something to put in.
    [[nodiscard]] bool wants_reload(Weapon weapon) const noexcept { return !full(weapon); }

    // One accepted shot. Returns false (and changes nothing) on an empty magazine.
    bool consume(Weapon weapon) noexcept {
        auto& r = rounds_[static_cast<unsigned>(weapon)];
        if (r == 0)
            return false;
        --r;
        return true;
    }

    // The magazine is seated: unlimited reserve, so this is always a full refill.
    void refill(Weapon weapon) noexcept {
        rounds_[static_cast<unsigned>(weapon)] = capacity(weapon);
    }

    struct TriggerResult {
        gameplay::CharacterInput input;
        // The player pulled the trigger on an empty gun while able to act: click, then reload.
        bool dry_fire = false;
    };

    // Removes the fire bit (level AND edge, like filter_busy_fire) when the magazine is empty.
    // `busy` guards the dry-fire report: during a reload or bolt cycle the busy filter already
    // owns the trigger and a click would be noise. Nothing is queued: a refused pull is gone.
    [[nodiscard]] TriggerResult
    filter_trigger(gameplay::CharacterInput input,
                   Weapon weapon,
                   bool busy,
                   std::uint32_t fire_bit = gameplay::kActionFire) const noexcept {
        TriggerResult out{input, false};
        if (!empty(weapon))
            return out;
        out.dry_fire = !busy && ((input.held | input.pressed) & fire_bit) != 0;
        out.input.held &= ~fire_bit;
        out.input.pressed &= ~fire_bit;
        return out;
    }

private:
    std::array<std::uint32_t, 3> rounds_{kCapacity[0], kCapacity[1], kCapacity[2]};
};

} // namespace rime::block_play
