// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"
#include "rime/platform/window.hpp"

// The event model: a *queue* the app drains once per frame, not a set of callbacks.
//
// Each OS delivers input through a callback its pump invokes (Win32 WNDPROC, Cocoa NSEvent, X11
// XEvent, Wayland listeners). Those callbacks do one job — translate the native event into a POD
// `Event` and enqueue it — and the app then drains the queue with poll_event() at a time of its
// choosing. This decouples OS callback timing from frame logic (good for a job-system world where
// the frame is data-parallel), keeps events trivially copyable and cache-friendly, and — crucially
// — makes the whole layer testable: the null backend (and tests) synthesize input with
// post_event().
namespace rime::platform {

enum class EventType : std::uint16_t {
    None = 0,
    WindowClose,  // the user/app asked to close the window
    WindowResize, // framebuffer size changed -> payload `resize`
    WindowFocus,  // gained/lost keyboard focus -> payload `focus`
    WindowMove,   // window moved -> payload `move`
    KeyDown,      // physical key pressed (or auto-repeat) -> payload `key`
    KeyUp,        // physical key released -> payload `key`
    TextInput,    // a typed character (layout/IME applied) -> payload `text`
    MouseMove,    // pointer moved -> payload `mouse_move`
    MouseButton,  // mouse button up/down -> payload `button`
    MouseWheel,   // scroll -> payload `wheel`
};

// A single input/window event. POD and trivially copyable; the active payload is selected by
// `type` (read only the matching union member). Construct as `Event e{};` then fill the fields.
// The payloads are *named* nested types (not anonymous) so the anonymous union stays within
// standard C++ — anonymous types inside an anonymous union are a non-portable extension.
struct Event {
    struct Resize {
        Extent2D size;
    };

    struct Move {
        std::int32_t x, y;
    };

    struct Focus {
        bool focused;
    };

    struct KeyData {
        Key key;
        KeyMods mods;
        bool repeat; // true if this KeyDown is an auto-repeat
    };

    struct Text {
        char utf8[8]; // one codepoint as null-terminated UTF-8
    };

    struct MouseMove {
        float x, y;   // absolute position in framebuffer pixels
        float dx, dy; // relative motion since the last event (raw, for camera control)
    };

    struct MouseButtonData {
        MouseButton button;
        KeyMods mods;
        bool down;
    };

    struct Wheel {
        float dx, dy; // horizontal / vertical scroll, in lines (or pixels for precise devices)
    };

    EventType type = EventType::None;
    WindowId window{}; // which window produced it (invalid handle if not window-specific)

    union {
        Resize resize;
        Move move;
        Focus focus;
        KeyData key;
        Text text;
        MouseMove mouse_move;
        MouseButtonData button;
        Wheel wheel;
    };
};

// Pump the OS event queue: run the active backend's native pump, translating every pending native
// event onto our queue. Call once per frame, on the main thread. Returns false once a close/quit
// has been requested (a convenience for `while (pump_events()) { ... }`); the per-window
// should_close() is the other way to end the loop.
bool pump_events();

// Remove the oldest queued event into `out`; returns false (and leaves `out` untouched) when the
// queue is empty. Drain in a loop after pump_events().
bool poll_event(Event& out);

// Push a synthetic event onto the queue. Backends use this to deliver translated native events;
// tests and app code can use it to inject input. Safe to call only from the main thread.
void post_event(const Event& e);

} // namespace rime::platform
