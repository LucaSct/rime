// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rime::block_play {

// Presentation only: authoritative ammunition and fire acceptance belong to the game.
// All timings use elapsed wall time, including frames longer than a normal render frame.
class PlayWeaponMotion {
public:
    enum class Weapon : std::uint8_t { Scar, M4, Sniper };
    enum class Action : std::uint8_t { Idle, Reload, PendingBolt, Bolt };

    struct Pose {
        float lower = 0.0f;
        float tilt = 0.0f;
        float left_magazine = 0.0f;
        float magazine_pull = 0.0f;
        float right_bolt = 0.0f;
        float bolt_pull = 0.0f;
        // 0..1..0 weapon jolt as the fresh magazine seats; overshoots, so a spring-like bump.
        float seat = 0.0f;
        // Seconds since the spent magazine left the hand; negative while it is not falling.
        float old_mag_fall = -1.0f;
    };

    // Release instant of the spent magazine, as a fraction of the reload, and how long its
    // ballistic fall stays visible before it is culled as "out of sight below the frame".
    static constexpr float kMagReleaseFraction = 0.50f;
    static constexpr float kMagFallSeconds = 0.45f;

    static constexpr float kRifleReloadSeconds = 2.2f;
    static constexpr float kSniperReloadSeconds = 2.8f;
    static constexpr float kBoltSeconds = 1.1f;
    static constexpr float kExtractionSeconds = 0.3f;

    [[nodiscard]] bool begin_reload(Weapon weapon) noexcept {
        if (busy())
            return false;
        weapon_ = weapon;
        action_ = Action::Reload;
        elapsed_ = 0.0f;
        return true;
    }

    // fired is an accepted shot. A bolt cycle never creates a shot of its own.
    void advance(float dt, Weapon weapon, bool fired, bool aiming = false) noexcept {
        if (weapon != weapon_)
            cancel(weapon);
        // The shot was already accepted by the authoritative weapon, so record it even on a
        // frame whose clock is unusable. Checking dt first silently dropped its bolt and case.
        if (fired && weapon == Weapon::Sniper && !busy()) {
            // A scoped shot keeps its sight picture until the aim level is released. The
            // accepted event is stored even when this frame has an unusable clock.
            action_ = aiming ? Action::PendingBolt : Action::Bolt;
            elapsed_ = 0.0f;
        }
        if (action_ == Action::PendingBolt && !aiming) {
            action_ = Action::Bolt;
            elapsed_ = 0.0f;
        }
        if (!std::isfinite(dt) || dt < 0.0f)
            return;
        if (!busy() || action_ == Action::PendingBolt)
            return;
        const float prior = elapsed_;
        const float duration = action_ == Action::Bolt ? kBoltSeconds : reload_seconds(weapon_);
        elapsed_ = std::min(duration, elapsed_ + dt);
        if (action_ == Action::Bolt && prior < kExtractionSeconds && elapsed_ >= kExtractionSeconds)
            ++pending_cases_;
        if (elapsed_ >= duration) {
            action_ = Action::Idle;
            elapsed_ = 0.0f;
        }
    }

    void cancel(Weapon weapon) noexcept {
        weapon_ = weapon;
        action_ = Action::Idle;
        elapsed_ = 0.0f;
        pending_cases_ = 0;
    }

    [[nodiscard]] bool reloading() const noexcept { return action_ == Action::Reload; }

    [[nodiscard]] bool cycling() const noexcept { return action_ == Action::Bolt; }

    [[nodiscard]] bool pending_bolt() const noexcept { return action_ == Action::PendingBolt; }

    [[nodiscard]] bool busy() const noexcept { return action_ != Action::Idle; }

    [[nodiscard]] Action action() const noexcept { return action_; }

    [[nodiscard]] float elapsed() const noexcept { return elapsed_; }

    [[nodiscard]] bool take_case_ejection() noexcept {
        if (pending_cases_ == 0)
            return false;
        --pending_cases_;
        return true;
    }

    [[nodiscard]] Pose pose() const noexcept {
        if (!busy() || action_ == Action::PendingBolt)
            return {};
        const float duration = action_ == Action::Bolt ? kBoltSeconds : reload_seconds(weapon_);
        const float t = elapsed_ / duration;
        if (action_ == Action::Reload) {
            // Approach, grip, extract, pause, reseat, then return. The support wrist and
            // magazine share magazine_pull while gripped; idle and completed reloads are zero.
            // Rifles finish with a short bolt-catch slap: the right hand reaches up and
            // forward, taps, and returns while the weapon is already settling.
            const bool rifle = weapon_ != Weapon::Sniper;
            Pose p{window(t, 0.0f, 0.18f, 0.75f, 1.0f),
                   window(t, 0.04f, 0.24f, 0.72f, 0.97f),
                   window(t, 0.10f, 0.30f, 0.80f, 0.94f),
                   // Pull-out eases out (a quick yank that decelerates against the stop); the
                   // fresh magazine eases IN to the well, accelerating until the seat stops it.
                   pull_window(t),
                   rifle ? 0.5f * window(t, 0.80f, 0.86f, 0.90f, 0.95f) : 0.0f,
                   rifle ? window(t, 0.86f, 0.89f, 0.89f, 0.93f) : 0.0f,
                   seat_pulse(t),
                   -1.0f};
            const float released = (t - kMagReleaseFraction) * duration;
            if (released >= 0.0f && released < kMagFallSeconds)
                p.old_mag_fall = released;
            return p;
        }
        // The sniper's right hand reaches the bolt, pulls, and returns to its grip.
        return {0.28f * window(t, 0.03f, 0.16f, 0.70f, 0.95f),
                0.12f * window(t, 0.08f, 0.20f, 0.70f, 0.92f),
                0.0f,
                0.0f,
                window(t, 0.04f, 0.20f, 0.68f, 0.91f),
                window(t, 0.21f, 0.42f, 0.56f, 0.76f)};
    }

private:
    static float reload_seconds(Weapon weapon) noexcept {
        return weapon == Weapon::Sniper ? kSniperReloadSeconds : kRifleReloadSeconds;
    }

    static float smooth(float t) noexcept {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Rise eases out, fall eases in (cubic): the opposite curves of smooth(), so the hand
    // snaps away from the well and then accelerates into it, like a real insertion.
    static float pull_window(float t) noexcept {
        const float up = std::clamp((t - 0.34f) / 0.14f, 0.0f, 1.0f);
        const float down = std::clamp((t - 0.61f) / 0.16f, 0.0f, 1.0f);
        const float rise = 1.0f - (1.0f - up) * (1.0f - up) * (1.0f - up);
        return rise * (1.0f - down * down * down);
    }

    // The seat: a fast ease-out rise on contact (t=0.77) and a longer settle, peaking around 0.80
    // and undershooting slightly so the weapon rebounds instead of stopping dead.
    static float seat_pulse(float t) noexcept {
        const float u = (t - 0.77f) / 0.17f;
        if (u <= 0.0f || u >= 1.0f)
            return 0.0f;
        return std::sin(3.14159265f * std::sqrt(u)) * std::exp(-2.5f * u) * 1.6f;
    }

    static float
    window(float t, float rise_start, float rise_end, float fall_start, float fall_end) noexcept {
        return smooth((t - rise_start) / (rise_end - rise_start)) *
               (1.0f - smooth((t - fall_start) / (fall_end - fall_start)));
    }

    Weapon weapon_ = Weapon::Scar;
    Action action_ = Action::Idle;
    float elapsed_ = 0.0f;
    std::uint32_t pending_cases_ = 0;
};

} // namespace rime::block_play
