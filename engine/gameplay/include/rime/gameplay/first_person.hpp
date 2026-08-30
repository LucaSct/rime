// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"

// The first-person view (m13.3a): pointer motion in, an eye transform out.
//
// PURE, like `step_character` next door and for the same reason. No world, no entities, no camera
// component — it takes two floats and a position and returns a `core::Transform`. That is what lets
// it be tested analytically (a look tape replayed twice is bit-identical) and what keeps `gameplay`
// from depending on `render`: this module computes WHERE the eye is and WHICH WAY it faces, and
// whoever is drawing attaches a `render::Camera` to an entity carrying that transform.
//
// WHY THE VIEW IS NOT SIMULATION STATE. Looking around changes nothing the server arbitrates: the
// mover consumes only `CharacterInput::yaw` (an absolute angle it turns into a movement basis), and
// `pitch` rides the tape for the weapon without the mover ever reading it. So yaw/pitch are
// PRESENTATION accumulated from device deltas on the client, sampled into the input tape each tick.
// Keeping them out of `CharacterState` is deliberate — that struct's doc comment says it is the
// complete tick-to-tick memory the m12.4 replay restores, and a view angle that got rewound and
// replayed would snap the camera on every server correction.
namespace rime::gameplay {

// How far you may look up or down. A full ±90° makes the forward vector collinear with world +Y,
// at which point the view basis has no well-defined roll and the image rolls as you cross it. Every
// engine clamps just short; 89° is the usual number and leaves the singularity comfortably outside.
inline constexpr float kMaxPitch = 1.55334f; // 89° in radians

struct FirstPersonView {
    // ABSOLUTE angles, matching `CharacterInput::yaw`'s own contract — not deltas. Yaw is about
    // world +Y, right-handed, so at yaw = 0 forward is −Z: the same basis `step_character` builds
    // (`forward = (−sin yaw, 0, −cos yaw)`), which is what makes "walk forward" walk where you are
    // looking. Feeding the mover a different yaw convention is a bug that reads as the character
    // strafing when it should advance.
    float yaw = 0.0f;
    float pitch = 0.0f; // + is up, clamped to ±kMaxPitch

    // The eye sits this far above the character's ORIGIN, which for a capsule is its CENTRE and not
    // its feet (see `CharacterState::position`). For the default 0.4 m radius / 0.5 m half-height
    // capsule the top of the head is +0.9 m, so 0.6 m puts the eye plausibly inside it. Getting the
    // reference point wrong is the classic first-person bug: the camera looks correct standing
    // still and sinks into the floor the moment the capsule crouches or the config changes.
    float eye_height = 0.6f;

    // Radians per unit of pointer motion. Pointer deltas arrive in pixels, so this is the whole
    // mouse-sensitivity knob; the default sweeps ~180° across a 1200-pixel drag.
    float sensitivity = 0.0026f;
};

// Fold one frame's pointer motion into the view. `dx` is rightward pointer motion and `dy` is
// downward, which is the direction both X11 and Win32 report.
//
// Right turns RIGHT (yaw decreases) and pushing the mouse forward looks UP (pitch increases) — the
// non-inverted defaults. Pitch clamps; yaw WRAPS into (−π, π] rather than growing without bound,
// because an angle that accumulates for an hour loses float precision exactly where the player is
// aiming, and because the wrapped value is what goes on the input tape.
void apply_look(FirstPersonView& view, float dx, float dy) noexcept;

// Where the eye is and which way it faces: the character's position raised by `eye_height`, rotated
// yaw-then-pitch. The rotation maps −Z to the look direction, the convention `render::Camera`,
// `DirectionalLight` and `SpotLight` all share ("aiming a light is rotating its entity").
[[nodiscard]] core::Transform eye_transform(const FirstPersonView& view,
                                            core::Vec3 character_position) noexcept;

// The unit direction the view is looking along — `eye_transform`'s rotation applied to −Z, computed
// directly. Provided because a weapon ray and a camera must not disagree by a rounding path: both
// call this.
[[nodiscard]] core::Vec3 look_direction(const FirstPersonView& view) noexcept;

} // namespace rime::gameplay
