// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>

#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/first_person.hpp"
#include "rime/gameplay/weapon.hpp" // kActionFire — the bit the fire binding sets
#include "rime/platform/event.hpp"
#include "rime/platform/input.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"

// The device → intent map (m13.3c): one frame's keyboard and mouse, folded into the engine's
// `CharacterInput` and a `FirstPersonView`.
//
// WHY THIS FILE EXISTS AT ALL. Both halves of it were already built and neither was connected to
// anything. `platform::Input` (M2.3) does polled state with per-frame edges — and until this brick
// it was referenced by its own test and by nothing else in the engine, the samples, or the tools.
// `gameplay::FirstPersonView` (m13.3a) turns pointer motion into an eye transform, proved across 37
// angles — and it was referenced by its own header, its own .cpp and its own test. The window
// opened in m13.3a with no way to move in it because THE JOIN was missing, not the parts. That is
// this repo's recurring failure shape (docs: assert the handoff, not the value), so the join is a
// named, tested seam rather than forty lines inlined into whichever sample needed it first.
//
// LAYERING. `gameplay` may depend on `platform`: platform sits two layers below it in the cake
// (docs/ARCHITECTURE.md §2) and the edge points downward. This is not the same kind of edge as the
// ones gameplay/CMakeLists.txt deliberately refuses (`destruction`, `net`) — those are sideways, to
// peer feature modules, and taking them would make a single-player build drag in the whole
// networking stack. Depending on the OS input abstraction costs a build nothing and keeps every
// proof here GPU-free.
namespace rime::gameplay {

// Which physical control means which intent. A struct rather than constants so a game rebinds by
// assignment, and so a test can drive an unusual layout without the engine knowing about it.
struct InputBindings {
    platform::Key forward = platform::Key::W;
    platform::Key back = platform::Key::S;
    platform::Key left = platform::Key::A;
    platform::Key right = platform::Key::D;

    // Jump for a character; "rise" for a free camera. The same physical key serves both because the
    // two are consumed by different code — `step_character` reads the action bit, `fly_step` reads
    // the vertical axis — so there is no conflict to resolve.
    platform::Key up = platform::Key::Space;
    platform::Key down = platform::Key::LeftCtrl;
    platform::Key boost = platform::Key::LeftShift; // hold to move faster (free camera only)
    platform::Key quit = platform::Key::Escape;

    platform::MouseButton fire = platform::MouseButton::Left;

    // ── Look, and why it still DEFAULTS to a drag ────────────────────────────────────────────
    // Pointer capture is real as of m15.5 — `platform::Window::set_cursor_mode` is implemented by
    // every backend — but it is not something a control scheme may ASSUME. A locked cursor is a
    // request the platform can refuse: a Wayland compositor need not advertise pointer constraints,
    // another X11 client can hold the pointer, and the headless backend has no pointer at all. Free
    // look on a cursor that is not actually locked walks it off the window and the camera stops
    // steering mid-turn — the failure reads as a broken camera rather than a missing capability.
    //
    // So the default is hold-to-look, the DCC/editor convention, which needs nothing from the
    // backends and cannot break that way. A game flips it from what the window ACTUALLY gave it:
    //
    //     bindings.look_requires_drag = look_requires_drag_for(win->set_cursor_mode(Locked));
    //
    // — never from what it asked for.
    platform::MouseButton look_drag = platform::MouseButton::Right;
    bool look_requires_drag = true;
};

// Hold-to-look is required exactly when the cursor is not locked (m15.5).
//
// One line, and it is a named function rather than an inline comparison for a reason: it is the
// single place the engine decides that a REQUEST for capture is not evidence of capture. Take the
// argument from `set_cursor_mode`'s return value or from `cursor_mode()`; passing the mode you
// wanted turns the check into a tautology, which is the bug this exists to make hard to write.
[[nodiscard]] constexpr bool look_requires_drag_for(platform::CursorMode achieved) noexcept {
    return achieved != platform::CursorMode::Locked;
}

// What one frame of device input asked for.
//
// The split is deliberate: `character` is the SIMULATION intent — the exact struct `step_character`
// consumes and an input tape replicates — while the fields beside it are things a simulated
// character has no concept of. A free camera flies; a capsule does not. Keeping `vertical` out of
// `CharacterInput` matters because that struct goes on the wire and is replayed by m12.4's
// reconciliation: growing it for a debug camera would cost every packet, forever.
struct FrameIntent {
    CharacterInput character{}; // ground-plane move, absolute yaw/pitch, action bits
    float vertical = 0.0f;      // +1 rise / −1 sink — a FREE-CAMERA axis, never a character's
    bool boost = false;         // the speed modifier is held (free camera only)
    bool quit = false;          // the quit key went down THIS frame (an edge, not a level)
};

// Fold one frame's events into `in`, then into `view` and a returned intent.
//
// ONE CALL, not three, because the three-call version has a silent failure mode: forgetting
// `Input::new_frame()` leaves every `*_pressed()` edge stuck true forever, so ESC "works" on the
// first frame and then quits the app on every frame after it. Taking the raw event span and owning
// the new_frame/process loop makes that unrepresentable.
//
// `view` is IN/OUT: pointer motion ACCUMULATES into it, which is what makes yaw absolute (and what
// `CharacterInput::yaw` requires — the mover reads an angle, not a delta). The returned intent's
// yaw/pitch are copied from the view after the update, so a caller cannot hand the mover a stale
// angle by reading them in the wrong order.
[[nodiscard]] FrameIntent map_frame_input(platform::Input& in,
                                          std::span<const platform::Event> events,
                                          FirstPersonView& view,
                                          const InputBindings& bindings = {}) noexcept;

// Advance a free-flying eye by one frame and return its new position.
//
// The move is along the view basis INCLUDING PITCH — look up, push forward, you climb — which is
// what separates a fly camera from a walker. `step_character` deliberately flattens the same intent
// onto the ground plane; both are correct for their own job, and the difference is the whole
// distinction between the two functions.
//
// The composed direction is normalised when it exceeds unit length, so holding forward+right+rise
// is not √3 times faster than forward alone. (The unit-DISC clamp `step_character` applies to
// move_x/move_y is the two-dimensional case of the same rule.)
[[nodiscard]] core::Vec3 fly_step(const FirstPersonView& view,
                                  const FrameIntent& intent,
                                  core::Vec3 position,
                                  float speed,
                                  float dt) noexcept;

// A free camera: the view, the position, and the input state, driven by one call per frame.
//
// This is what a renderer sample or an editor viewport flies with. It is NOT a player — no gravity,
// no collision, nothing the server arbitrates — and it is deliberately advanced by FRAME time
// rather than by the fixed tick, because a camera is presentation. Tying it to the tick would make
// it stutter on every frame the accumulator happened to run zero ticks.
class FlyCamera {
public:
    // `position` IS the eye, so eye_height is zero here — the 0.6 m offset FirstPersonView defaults
    // to is a CHARACTER's eye above its capsule centre, and applying it to a free camera would put
    // the viewpoint half a metre above the coordinate the caller thinks it set.
    FirstPersonView view{.yaw = 0.0f, .pitch = 0.0f, .eye_height = 0.0f};
    core::Vec3 position{};
    InputBindings bindings{};
    float speed = 6.0f;       // metres per second
    float boost_scale = 4.0f; // multiplier while `bindings.boost` is held

    // Fold one frame's events in and integrate. Pass `Application::frame_input()` and the frame's
    // dt; headless, the same span is whatever a test posted, which is what makes the whole path
    // provable without a window.
    void update(std::span<const platform::Event> events, float dt) noexcept;

    // Where the eye is and which way it faces — hand this straight to a camera entity's transform.
    [[nodiscard]] core::Transform transform() const noexcept {
        return eye_transform(view, position);
    }

    // LATCHED, not per-frame: once the quit key has been seen this stays true. A per-frame flag is
    // missable by any caller whose loop skips a check, and "the app ignored my ESC" is precisely
    // the bug this brick exists to fix.
    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    // The last frame's intent — exposed so a caller can drive something else (a HUD readout, a
    // weapon) from the same fold rather than re-deriving it.
    [[nodiscard]] const FrameIntent& intent() const noexcept { return intent_; }

private:
    platform::Input input_{};
    FrameIntent intent_{};
    bool quit_ = false;
};

} // namespace rime::gameplay
