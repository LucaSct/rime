// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "keymap_linux.hpp"
#include "linux_backend.hpp"
#include "platform_backend.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// Wayland window backend.
//
// Wayland is a protocol, not a library of window calls: we connect to the compositor, bind the
// globals we need from the registry (wl_compositor for surfaces, xdg_wm_base for the desktop
// window role, wl_seat for input), then drive everything through asynchronous listener callbacks.
// Window geometry and lifetime come through xdg-shell (xdg_surface/xdg_toplevel); keyboard handling
// goes through xkbcommon (Wayland hands us a keymap over a file descriptor). The xdg-shell glue is
// generated from its protocol XML by wayland-scanner at build time (see CMakeLists).
//
// Keys carry the physical evdev code (shared key_from_evdev); text and modifier state come from
// xkbcommon. Unlike X11/Win32/macOS, a Wayland surface only becomes visible once a buffer is
// attached — which is the renderer's job — so under M2 (no renderer yet) the window is created and
// fully event-wired but not yet mapped on screen; the runnable M2.5 proof on Linux therefore uses
// X11, and pixels arrive here with the M3 Vulkan swapchain (VK_KHR_wayland_surface).
namespace rime::platform {
namespace {

wl_display* g_display = nullptr;
wl_registry* g_registry = nullptr;
wl_compositor* g_compositor = nullptr;
xdg_wm_base* g_wm_base = nullptr;
wl_seat* g_seat = nullptr;
wl_keyboard* g_keyboard = nullptr;
wl_pointer* g_pointer = nullptr;

xkb_context* g_xkb_ctx = nullptr;
xkb_keymap* g_xkb_keymap = nullptr;
xkb_state* g_xkb_state = nullptr;

class WaylandWindow;

// Pointer/keyboard input events name a wl_surface; this maps it back to the owning window so events
// can be tagged and routed. Focus is tracked per device (set on enter, cleared on leave).
std::unordered_map<wl_surface*, WaylandWindow*>& g_windows() {
    static std::unordered_map<wl_surface*, WaylandWindow*> windows;
    return windows;
}

WaylandWindow* g_keyboard_focus = nullptr;
WaylandWindow* g_pointer_focus = nullptr;
double g_pointer_x = 0.0;
double g_pointer_y = 0.0;
bool g_pointer_have_last = false;

WaylandWindow* window_for_surface(wl_surface* surface) {
    const auto it = g_windows().find(surface);
    return it != g_windows().end() ? it->second : nullptr;
}

// Effective modifier state, queried from the live xkb state.
KeyMods current_mods() {
    KeyMods m = KeyMods::None;
    if (g_xkb_state == nullptr) {
        return m;
    }
    if (xkb_state_mod_name_is_active(g_xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) >
        0) {
        m |= KeyMods::Shift;
    }
    if (xkb_state_mod_name_is_active(g_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) >
        0) {
        m |= KeyMods::Ctrl;
    }
    if (xkb_state_mod_name_is_active(g_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= KeyMods::Alt;
    }
    if (xkb_state_mod_name_is_active(g_xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) >
        0) {
        m |= KeyMods::Super;
    }
    return m;
}

class WaylandWindow final : public Window {
public:
    explicit WaylandWindow(const WindowDesc& desc); // defined below the listeners it wires up

    ~WaylandWindow() override {
        g_windows().erase(surface_);
        if (this == g_keyboard_focus) {
            g_keyboard_focus = nullptr;
        }
        if (this == g_pointer_focus) {
            g_pointer_focus = nullptr;
        }
        if (toplevel_ != nullptr) {
            xdg_toplevel_destroy(toplevel_);
        }
        if (xdg_surface_ != nullptr) {
            xdg_surface_destroy(xdg_surface_);
        }
        if (surface_ != nullptr) {
            wl_surface_destroy(surface_);
        }
        wl_display_flush(g_display);
    }

    void set_title(std::string_view title) override {
        xdg_toplevel_set_title(toplevel_, std::string(title).c_str());
    }

    void set_size(Extent2D size) override {
        // Under Wayland the client cannot force its own size (the compositor negotiates it); we
        // record the request and re-commit. A real resize follows once there is a buffer to resize.
        width_ = size.width;
        height_ = size.height;
        wl_surface_commit(surface_);
    }

    [[nodiscard]] Extent2D framebuffer_size() const override { return Extent2D{width_, height_}; }

    [[nodiscard]] Extent2D logical_size() const override { return Extent2D{width_, height_}; }

    [[nodiscard]] float content_scale() const override { return 1.0f; }

    [[nodiscard]] bool should_close() const override { return should_close_; }

    void request_close() override { notify_close(); }

    void show() override {
        // No buffer yet (that is the renderer's job in M3), so committing here does not map pixels;
        // it simply finishes the surface setup. The compositor maps the window on the first buffer.
        wl_surface_commit(surface_);
        wl_display_flush(g_display);
    }

    [[nodiscard]] NativeWindow native_handle() const override {
        // M3's Vulkan backend builds a surface from wl_display* + wl_surface*
        // (VK_KHR_wayland_surface).
        return NativeWindow{WindowSystem::Wayland, g_display, surface_};
    }

    [[nodiscard]] WindowId id() const override { return id_; }

    void notify_close() {
        if (should_close_) {
            return;
        }
        should_close_ = true;
        detail::request_quit();
        Event e{};
        e.type = EventType::WindowClose;
        e.window = id_;
        post_event(e);
    }

    void notify_resize(std::uint32_t w, std::uint32_t h) {
        if (w == width_ && h == height_) {
            return;
        }
        width_ = w;
        height_ = h;
        Event e{};
        e.type = EventType::WindowResize;
        e.window = id_;
        e.resize.size = Extent2D{width_, height_};
        post_event(e);
    }

    [[nodiscard]] wl_surface* surface() const { return surface_; }

private:
    WindowId id_;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool should_close_ = false;
};

// ── Input listener callbacks ─────────────────────────────────────────────────────
// Each is a plain C callback matching the protocol's listener signature; the structs that collect
// them are defined just below (designated initializers fill the leading members and leave the
// optional trailing ones null — which is also why -Wmissing-field-initializers stays quiet).

void kb_keymap(void*, wl_keyboard*, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ::close(fd);
        return;
    }
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        return;
    }
    if (g_xkb_state != nullptr) {
        xkb_state_unref(g_xkb_state);
        g_xkb_state = nullptr;
    }
    if (g_xkb_keymap != nullptr) {
        xkb_keymap_unref(g_xkb_keymap);
    }
    g_xkb_keymap = xkb_keymap_new_from_string(g_xkb_ctx,
                                              static_cast<const char*>(mapped),
                                              XKB_KEYMAP_FORMAT_TEXT_V1,
                                              XKB_KEYMAP_COMPILE_NO_FLAGS);
    ::munmap(mapped, size);
    g_xkb_state = (g_xkb_keymap != nullptr) ? xkb_state_new(g_xkb_keymap) : nullptr;
}

void kb_enter(void*, wl_keyboard*, std::uint32_t, wl_surface* surface, wl_array*) {
    g_keyboard_focus = window_for_surface(surface);
}

void kb_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*) {
    g_keyboard_focus = nullptr;
}

void kb_key(void*,
            wl_keyboard*,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t key,
            std::uint32_t state) {
    if (g_keyboard_focus == nullptr) {
        return;
    }
    const bool down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    Event e{};
    e.type = down ? EventType::KeyDown : EventType::KeyUp;
    e.window = g_keyboard_focus->id();
    e.key.key = detail::key_from_evdev(key); // Wayland delivers the raw evdev code
    e.key.mods = current_mods();
    e.key.repeat = false;
    post_event(e);

    if (down && g_xkb_state != nullptr) {
        char buf[8] = {};
        // xkb maps keycodes as evdev + 8; get_utf8 applies layout + modifiers to produce text.
        const int n = xkb_state_key_get_utf8(g_xkb_state, key + 8, buf, sizeof(buf));
        if (n > 0 && static_cast<unsigned char>(buf[0]) >= 0x20 &&
            static_cast<unsigned char>(buf[0]) != 0x7F) {
            Event te{};
            te.type = EventType::TextInput;
            te.window = g_keyboard_focus->id();
            std::memcpy(te.text.utf8, buf, sizeof(te.text.utf8) - 1);
            post_event(te);
        }
    }
}

void kb_modifiers(void*,
                  wl_keyboard*,
                  std::uint32_t,
                  std::uint32_t depressed,
                  std::uint32_t latched,
                  std::uint32_t locked,
                  std::uint32_t group) {
    if (g_xkb_state != nullptr) {
        xkb_state_update_mask(g_xkb_state, depressed, latched, locked, 0, 0, group);
    }
}

const wl_keyboard_listener g_keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
};

void ptr_enter(void*,
               wl_pointer*,
               std::uint32_t,
               wl_surface* surface,
               wl_fixed_t sx,
               wl_fixed_t sy) {
    g_pointer_focus = window_for_surface(surface);
    g_pointer_x = wl_fixed_to_double(sx);
    g_pointer_y = wl_fixed_to_double(sy);
    g_pointer_have_last = false; // first motion after entering reports zero delta
}

void ptr_leave(void*, wl_pointer*, std::uint32_t, wl_surface*) {
    g_pointer_focus = nullptr;
}

void ptr_motion(void*, wl_pointer*, std::uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    if (g_pointer_focus == nullptr) {
        return;
    }
    const double x = wl_fixed_to_double(sx);
    const double y = wl_fixed_to_double(sy);
    Event e{};
    e.type = EventType::MouseMove;
    e.window = g_pointer_focus->id();
    e.mouse_move.x = static_cast<float>(x);
    e.mouse_move.y = static_cast<float>(y);
    e.mouse_move.dx = g_pointer_have_last ? static_cast<float>(x - g_pointer_x) : 0.0f;
    e.mouse_move.dy = g_pointer_have_last ? static_cast<float>(y - g_pointer_y) : 0.0f;
    g_pointer_x = x;
    g_pointer_y = y;
    g_pointer_have_last = true;
    post_event(e);
}

void ptr_button(void*,
                wl_pointer*,
                std::uint32_t,
                std::uint32_t,
                std::uint32_t button,
                std::uint32_t state) {
    if (g_pointer_focus == nullptr) {
        return;
    }
    MouseButton b = MouseButton::Left;
    switch (button) {
        case BTN_LEFT:
            b = MouseButton::Left;
            break;
        case BTN_RIGHT:
            b = MouseButton::Right;
            break;
        case BTN_MIDDLE:
            b = MouseButton::Middle;
            break;
        case BTN_SIDE:
            b = MouseButton::X1;
            break;
        case BTN_EXTRA:
            b = MouseButton::X2;
            break;
        default:
            return;
    }
    Event e{};
    e.type = EventType::MouseButton;
    e.window = g_pointer_focus->id();
    e.button.button = b;
    e.button.mods = current_mods();
    e.button.down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    post_event(e);
}

void ptr_axis(void*, wl_pointer*, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
    if (g_pointer_focus == nullptr) {
        return;
    }
    // Wayland axis values are in surface units (~10 per wheel notch) and positive-down; scale to
    // rough "lines" and flip vertical so positive dy means scrolling up, matching the other OSes.
    const float v = static_cast<float>(wl_fixed_to_double(value) / 10.0);
    Event e{};
    e.type = EventType::MouseWheel;
    e.window = g_pointer_focus->id();
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        e.wheel.dy = -v;
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        e.wheel.dx = v;
    }
    post_event(e);
}

const wl_pointer_listener g_pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
};

void seat_capabilities(void*, wl_seat* seat, std::uint32_t caps) {
    const bool has_keyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_keyboard && g_keyboard == nullptr) {
        g_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_keyboard, &g_keyboard_listener, nullptr);
    } else if (!has_keyboard && g_keyboard != nullptr) {
        wl_keyboard_destroy(g_keyboard);
        g_keyboard = nullptr;
    }
    const bool has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && g_pointer == nullptr) {
        g_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer, &g_pointer_listener, nullptr);
    } else if (!has_pointer && g_pointer != nullptr) {
        wl_pointer_destroy(g_pointer);
        g_pointer = nullptr;
    }
}

void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener g_seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// ── Surface / shell listener callbacks ───────────────────────────────────────────

void wm_base_ping(void*, xdg_wm_base* base, std::uint32_t serial) {
    xdg_wm_base_pong(base, serial); // answer the compositor's liveness check
}

const xdg_wm_base_listener g_wm_base_listener = {
    .ping = wm_base_ping,
};

void surface_configure(void*, xdg_surface* surface, std::uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
}

const xdg_surface_listener g_xdg_surface_listener = {
    .configure = surface_configure,
};

void toplevel_configure(void* data,
                        xdg_toplevel*,
                        std::int32_t width,
                        std::int32_t height,
                        wl_array*) {
    if (width > 0 && height > 0) {
        static_cast<WaylandWindow*>(data)->notify_resize(static_cast<std::uint32_t>(width),
                                                         static_cast<std::uint32_t>(height));
    }
}

void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<WaylandWindow*>(data)->notify_close();
}

const xdg_toplevel_listener g_toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

void registry_global(void*,
                     wl_registry* registry,
                     std::uint32_t name,
                     const char* interface,
                     std::uint32_t version) {
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        g_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g_wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(g_wm_base, &g_wm_base_listener, nullptr);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        g_seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(g_seat, &g_seat_listener, nullptr);
    }
}

void registry_global_remove(void*, wl_registry*, std::uint32_t) {}

const wl_registry_listener g_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Out-of-line so it can reference the listener structs defined above (the listeners in turn need
// the WaylandWindow type, so the constructor cannot be defined inside the class).
WaylandWindow::WaylandWindow(const WindowDesc& desc)
    : id_(detail::allocate_window_id()), width_(desc.width), height_(desc.height) {
    surface_ = wl_compositor_create_surface(g_compositor);
    g_windows().emplace(surface_, this);

    xdg_surface_ = xdg_wm_base_get_xdg_surface(g_wm_base, surface_);
    xdg_surface_add_listener(xdg_surface_, &g_xdg_surface_listener, this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(toplevel_, &g_toplevel_listener, this);

    const std::string title(desc.title);
    xdg_toplevel_set_title(toplevel_, title.c_str());
    xdg_toplevel_set_app_id(toplevel_, "rime");
    if (!desc.resizable) {
        xdg_toplevel_set_min_size(
            toplevel_, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_));
        xdg_toplevel_set_max_size(
            toplevel_, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_));
    }
    wl_surface_commit(surface_);
    wl_display_roundtrip(g_display); // settle the initial configure handshake
}

bool wayland_init() {
    g_display = wl_display_connect(nullptr);
    if (g_display == nullptr) {
        return false; // not a Wayland session -> the dispatcher falls back to X11
    }
    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &g_registry_listener, nullptr);
    wl_display_roundtrip(g_display); // first pass: bind globals advertised by the registry
    wl_display_roundtrip(g_display); // second pass: process seat capabilities from the bind

    if (g_compositor == nullptr || g_wm_base == nullptr) {
        return false; // missing essentials; treat as unavailable
    }
    return true;
}

void wayland_shutdown() {
    if (g_xkb_state != nullptr) {
        xkb_state_unref(g_xkb_state);
        g_xkb_state = nullptr;
    }
    if (g_xkb_keymap != nullptr) {
        xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = nullptr;
    }
    if (g_xkb_ctx != nullptr) {
        xkb_context_unref(g_xkb_ctx);
        g_xkb_ctx = nullptr;
    }
    if (g_keyboard != nullptr) {
        wl_keyboard_destroy(g_keyboard);
        g_keyboard = nullptr;
    }
    if (g_pointer != nullptr) {
        wl_pointer_destroy(g_pointer);
        g_pointer = nullptr;
    }
    if (g_seat != nullptr) {
        wl_seat_destroy(g_seat);
        g_seat = nullptr;
    }
    if (g_wm_base != nullptr) {
        xdg_wm_base_destroy(g_wm_base);
        g_wm_base = nullptr;
    }
    if (g_compositor != nullptr) {
        wl_compositor_destroy(g_compositor);
        g_compositor = nullptr;
    }
    if (g_registry != nullptr) {
        wl_registry_destroy(g_registry);
        g_registry = nullptr;
    }
    if (g_display != nullptr) {
        wl_display_disconnect(g_display);
        g_display = nullptr;
    }
}

// Non-blocking pump (the canonical prepare_read / poll / read_events dance): dispatch what is
// already queued, then read from the fd only if it has data, so the frame loop never stalls.
void wayland_pump() {
    if (g_display == nullptr) {
        return;
    }
    while (wl_display_prepare_read(g_display) != 0) {
        wl_display_dispatch_pending(g_display);
    }
    wl_display_flush(g_display);

    pollfd pfd{};
    pfd.fd = wl_display_get_fd(g_display);
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
        wl_display_read_events(g_display);
    } else {
        wl_display_cancel_read(g_display);
    }
    wl_display_dispatch_pending(g_display);
}

std::unique_ptr<Window> wayland_create_window(const WindowDesc& desc) {
    if (g_compositor == nullptr || g_wm_base == nullptr) {
        return nullptr;
    }
    return std::make_unique<WaylandWindow>(desc);
}

} // namespace

namespace detail {

const LinuxBackend& wayland_backend() {
    static const LinuxBackend backend{
        wayland_init, wayland_shutdown, wayland_pump, wayland_create_window};
    return backend;
}

} // namespace detail
} // namespace rime::platform
