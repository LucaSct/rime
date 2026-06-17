// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 00-hello-window — a minimal driver for the M2 platform layer: open a native window, pump the OS
// event queue, react to a few input events, and close cleanly. There is no rendering yet (that is
// M3); this exists so the window/input seam can be seen and felt on a real machine. The full M2.5
// proof (live FPS in the title, CI on all three OSes) lands once the frame timer (M2.4) is in.
//
// Run it:   build/dev/bin/hello_window           (opens a real window)
//           build/dev/bin/hello_window --headless (null backend; for CI / display-less machines)

#include <chrono>
#include <cstdio>
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
        // The native backend for this OS is not implemented yet (Win32/Linux are stubs until
        // M2.2b-d); on macOS this should not happen.
        std::fprintf(stderr, "create_window() returned null — no native window backend yet?\n");
        shutdown();
        return 1;
    }
    window->show();

    const Extent2D fb = window->framebuffer_size();
    std::printf("hello-window: %ux%u px (content scale %.2f). Press ESC or close the window.\n",
                fb.width,
                fb.height,
                window->content_scale());

    // The game loop: pump the OS each frame, drain our event queue, react. pump_events() returns
    // false once a close/quit is requested; should_close() is the per-window equivalent.
    while (pump_events() && !window->should_close()) {
        Event e{};
        while (poll_event(e)) {
            switch (e.type) {
                case EventType::KeyDown:
                    if (e.key.key == Key::Escape) {
                        window->request_close();
                    }
                    break;
                case EventType::TextInput:
                    std::printf("text: %s\n", e.text.utf8);
                    break;
                case EventType::MouseButton:
                    if (e.button.down) {
                        std::printf("mouse button %d down\n", static_cast<int>(e.button.button));
                    }
                    break;
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
        // No renderer yet, so cap the loop so it does not spin a core flat (~125 Hz).
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    shutdown();
    std::printf("hello-window: closed cleanly.\n");
    return 0;
}
