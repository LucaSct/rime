// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 00-hello-window — the M2.5 proof for the platform layer: open a native window, run a real frame
// loop, react to keyboard/mouse through the polled Input layer (M2.3), show a live FPS readout in
// the title bar via FrameTimer (M2.4), and close cleanly. There is no rendering yet (that is M3);
// this exercises the whole window/input/timing seam end to end on Windows, Linux (Wayland or X11),
// and macOS — the same source, no OS #ifdefs.
//
// Run it:   build/dev/bin/hello_window            (opens a real window)
//           build/dev/bin/hello_window --headless (null backend; for CI / display-less machines)

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "rime/platform/platform.hpp"

int main(int argc, char** argv) {
    using namespace rime::platform;

    // --headless forces the null backend so this still runs where there is no window server.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--headless") {
            set_headless(true);
        }
    }

    if (!init()) {
        std::fprintf(stderr, "platform::init() failed\n");
        return 1;
    }

    WindowDesc desc{};
    desc.title = "Rime — hello window";
    desc.width = 1280;
    desc.height = 720;

    auto window = create_window(desc);
    if (!window) {
        std::fprintf(
            stderr, "create_window() returned null — no native window backend for this session?\n");
        shutdown();
        return 1;
    }
    window->show();

    const Extent2D fb = window->framebuffer_size();
    std::printf("hello-window: %ux%u px (content scale %.2f). Press ESC or close the window.\n",
                fb.width,
                fb.height,
                window->content_scale());

    Input input;
    FrameTimer timer;
    double last_title_update = 0.0;

    // The frame loop: tick the clock, start a new input frame, pump the OS, fold this frame's
    // events into Input (printing the window-level ones), then query polled state. pump_events()
    // returns false once a close/quit has been requested; should_close() is the per-window
    // equivalent.
    while (pump_events() && !window->should_close()) {
        timer.tick();
        input.new_frame();

        Event e{};
        while (poll_event(e)) {
            input.process(e);
            switch (e.type) {
                case EventType::WindowResize:
                    std::printf("resize: %ux%u px\n", e.resize.size.width, e.resize.size.height);
                    break;
                case EventType::WindowFocus:
                    std::printf("focus: %s\n", e.focus.focused ? "gained" : "lost");
                    break;
                default:
                    break;
            }
        }

        // Polled input (M2.3): edge-triggered queries plus the text typed this frame.
        if (input.key_pressed(Key::Escape)) {
            window->request_close();
        }
        if (input.mouse_pressed(MouseButton::Left)) {
            std::printf("left click at %.0f, %.0f\n", input.mouse_x(), input.mouse_y());
        }
        if (!input.text().empty()) {
            std::printf("text: %s\n", std::string(input.text()).c_str());
        }

        // Live FPS in the title bar (M2.4 FrameTimer), refreshed ~4x/second so it stays readable.
        if (timer.elapsed_seconds() - last_title_update > 0.25) {
            last_title_update = timer.elapsed_seconds();
            char title[96];
            std::snprintf(
                title, sizeof(title), "Rime — hello window — %.0f FPS", timer.smoothed_fps());
            window->set_title(title);
        }

        // No renderer/vsync yet, so cap the loop (~125 Hz) instead of spinning a core flat. Once M3
        // drives presentation the swapchain paces the frame and this sleep goes away.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    shutdown();
    std::printf("hello-window: closed cleanly.\n");
    return 0;
}
