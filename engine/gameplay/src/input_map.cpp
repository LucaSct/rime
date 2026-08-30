// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/gameplay/input_map.hpp"

#include <cmath>

namespace rime::gameplay {
namespace {

// A two-key axis. Holding BOTH gives zero rather than favouring one: the alternative (last key
// wins, or positive wins) makes a player who rolls their fingers across A and D drift in a
// direction they did not ask for, and it is the classic first bug in a hand-rolled input map.
[[nodiscard]] float axis(const platform::Input& in, platform::Key pos, platform::Key neg) noexcept {
    return (in.key_down(pos) ? 1.0f : 0.0f) - (in.key_down(neg) ? 1.0f : 0.0f);
}

} // namespace

FrameIntent map_frame_input(platform::Input& in,
                            std::span<const platform::Event> events,
                            FirstPersonView& view,
                            const InputBindings& bindings) noexcept {
    in.new_frame();
    for (const platform::Event& e : events) {
        in.process(e);
    }

    // Look FIRST, so the yaw handed to the mover below is the angle the player is looking along
    // this frame rather than last frame's. One frame of lag here is not cosmetic: the mover turns
    // the yaw into a movement basis, so a stale angle makes fast turns curve the walk.
    if (!bindings.look_requires_drag || in.mouse_down(bindings.look_drag)) {
        apply_look(view, in.mouse_dx(), in.mouse_dy());
    }

    FrameIntent intent{};

    float mx = axis(in, bindings.right, bindings.left);
    float my = axis(in, bindings.forward, bindings.back);
    // The unit DISC, not the unit square — the same rule `step_character` applies to a tape it did
    // not author (character.cpp: "clamping per axis would leave a diagonal at length √2"). Doing it
    // HERE as well is not redundant: `fly_step` consumes this intent without going near the mover,
    // and a keyboard is the one device that can hand you a perfect (1, 1).
    const float len2 = mx * mx + my * my;
    if (len2 > 1.0f) {
        const float inv = 1.0f / std::sqrt(len2);
        mx *= inv;
        my *= inv;
    }
    intent.character.move_x = mx;
    intent.character.move_y = my;

    // Copied from the view AFTER apply_look, so these are absolute angles and current. This
    // assignment is the entire handoff m13.3a left unmade: the view knows where you are looking and
    // the mover needs that angle, and nothing carried it across.
    intent.character.yaw = view.yaw;
    intent.character.pitch = view.pitch;

    // `held` is a LEVEL and `pressed` is an EDGE — the distinction CharacterInput's comment draws,
    // and it exists because a level self-heals across a dropped packet while an edge does not.
    if (in.key_down(bindings.up)) {
        intent.character.held |= kActionJump;
    }
    if (in.key_pressed(bindings.up)) {
        intent.character.pressed |= kActionJump;
    }
    if (in.mouse_down(bindings.fire)) {
        intent.character.held |= kActionFire;
    }
    if (in.mouse_pressed(bindings.fire)) {
        intent.character.pressed |= kActionFire;
    }

    intent.vertical = axis(in, bindings.up, bindings.down);
    intent.boost = in.key_down(bindings.boost);
    // An EDGE, deliberately. On a level, holding ESC for two frames would quit twice — harmless
    // here, but the same mistake on a "drop weapon" binding drops the whole inventory.
    intent.quit = in.key_pressed(bindings.quit);
    return intent;
}

core::Vec3 fly_step(const FirstPersonView& view,
                    const FrameIntent& intent,
                    core::Vec3 position,
                    float speed,
                    float dt) noexcept {
    const core::Vec3 forward = look_direction(view);

    // Right is computed from YAW ALONE, and that is not a shortcut. normalize(forward × +Y) reduces
    // algebraically to exactly this for every pitch — the cos(pitch) factor cancels — so the closed
    // form is the same vector with no cross product to go degenerate as pitch approaches ±90°. It
    // is also, character.cpp:537 verbatim, the mover's own right basis; the two agreeing by
    // construction is what keeps strafing consistent between the free camera and a walking player.
    const core::Vec3 right{std::cos(view.yaw), 0.0f, -std::sin(view.yaw)};

    core::Vec3 dir = right * intent.character.move_x + forward * intent.character.move_y;
    dir.y += intent.vertical; // world up, not view up: rise means rise, whatever you are looking at

    const float len2 = core::length_squared(dir);
    if (len2 > 1.0f) {
        dir = dir * (1.0f / std::sqrt(len2));
    }
    return position + dir * (speed * dt);
}

void FlyCamera::update(std::span<const platform::Event> events, float dt) noexcept {
    intent_ = map_frame_input(input_, events, view, bindings);
    quit_ = quit_ || intent_.quit; // latched — see the accessor's comment
    position = fly_step(view, intent_, position, intent_.boost ? speed * boost_scale : speed, dt);
}

} // namespace rime::gameplay
