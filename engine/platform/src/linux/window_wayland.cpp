// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <linux/input-event-codes.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <poll.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "keymap_linux.hpp"
#include "linux_backend.hpp"
#include "platform_backend.hpp"
#include "rime/core/diagnostics/log.hpp"
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
// attached — a rule with a sharp consequence: a window with no renderer is not merely blank, it is
// never MAPPED. The compositor does not lay it out, does not list it as a client, and — because an
// unmapped surface is never re-laid-out — sends exactly one xdg_toplevel.configure and then nothing
// ever again. Measured on Hyprland 0.56.1: `hyprctl clients` reported zero windows for a running
// 00-hello-window, which prints one 784x1029 configure and then sits invisible forever.
//
// So this backend attaches a PLACEHOLDER wl_shm buffer for windows that no renderer claims. That
// keeps the platform layer's promise — "open a window, get input, get resizes, close cleanly" —
// true on Wayland without a GPU, which is what the M2.5 proof actually asserts. The claim is
// deferred by one pump rather than made in the constructor, and the test is `native_handle()`: the
// only reason to ask for the raw wl_surface is to build a rendering surface on it, and the RHI does
// so during setup, before the first frame. A window whose handle was never taken has no renderer,
// so we paint it ourselves; a window whose handle was taken pays nothing, because the placeholder
// is never allocated at all. That inference is documented on `Window::native_handle()` itself,
// since it gives a plain getter a side effect that a caller could not otherwise guess.
//
// Every buffer touch is gated on an acked xdg_surface.configure — see on_surface_configured() for
// the two protocol races that gate closes, both of which were traced live on this compositor.
//
// Pixels for real content still arrive with the M3 Vulkan swapchain (VK_KHR_wayland_surface); the
// placeholder is only ever the fallback for a rendererless window.
namespace rime::platform {
namespace {

wl_display* g_display = nullptr;
wl_registry* g_registry = nullptr;
wl_compositor* g_compositor = nullptr;
xdg_wm_base* g_wm_base = nullptr;
wl_shm* g_shm = nullptr; // optional: only the rendererless placeholder below needs it
wl_seat* g_seat = nullptr;
wl_keyboard* g_keyboard = nullptr;
wl_pointer* g_pointer = nullptr;

// POINTER CAPTURE (m15.5), and the pair is not optional. `pointer_constraints` pins the cursor;
// `relative_pointer` is the only way to learn how far it TRIED to move while pinned — a lock
// without it freezes the pointer and reports nothing, which is a worse camera than no lock at all.
// Both stay null when the compositor does not advertise them, and that is a supported outcome, not
// an error: set_cursor_mode then reports the weaker mode it actually achieved.
zwp_pointer_constraints_v1* g_pointer_constraints = nullptr;
zwp_relative_pointer_manager_v1* g_relative_pointer_manager = nullptr;
zwp_relative_pointer_v1* g_relative_pointer = nullptr;
// The user's cursor theme, and the surface we draw it on. Loaded lazily on the first hide, because
// a window that never hides its cursor should not pay for a theme it will not use — and because the
// load needs wl_shm, which arrives from the registry after the display is up.
wl_cursor_theme* g_cursor_theme = nullptr;
wl_surface* g_cursor_surface = nullptr;
bool g_cursor_theme_tried = false;
// The serial of the last pointer `enter`. wl_pointer::set_cursor — the ONLY way to hide a cursor on
// Wayland, by giving it a null surface — must quote a serial from a pointer event, so the value has
// to be kept from when it arrived.
std::uint32_t g_pointer_enter_serial = 0;

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

// One anonymous, memory-backed file to share pixels with the compositor. wl_shm works by handing
// the compositor a file descriptor it maps read-only, so the buffer must live in a real file —
// memfd_create gives us one with no filesystem name to clean up or collide on.
int make_shm_fd(std::size_t bytes) {
    const int fd = ::memfd_create("rime-placeholder", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Load the default cursor theme once, and answer whether we can draw a pointer at all. This is the
// gate on hiding: a client that hides the cursor and cannot put it back has taken the pointer away
// from the user for the life of the process, so the honest thing is to refuse to hide rather than
// to hide optimistically.
bool ensure_cursor_theme() {
    if (g_cursor_theme_tried) {
        return g_cursor_theme != nullptr && g_cursor_surface != nullptr;
    }
    g_cursor_theme_tried = true;
    if (g_shm == nullptr || g_compositor == nullptr) {
        return false;
    }
    g_cursor_theme =
        wl_cursor_theme_load(nullptr, 24, g_shm); // nullptr = the user's configured one
    if (g_cursor_theme == nullptr) {
        return false;
    }
    g_cursor_surface = wl_compositor_create_surface(g_compositor);
    return g_cursor_surface != nullptr;
}

// Put the ordinary arrow back. "left_ptr" is the X11-era name every theme still ships; "default" is
// the newer cursor-spec name and not universally present, so both are tried.
void restore_default_cursor() {
    if (!ensure_cursor_theme() || g_pointer == nullptr) {
        return;
    }
    wl_cursor* cursor = wl_cursor_theme_get_cursor(g_cursor_theme, "left_ptr");
    if (cursor == nullptr) {
        cursor = wl_cursor_theme_get_cursor(g_cursor_theme, "default");
    }
    if (cursor == nullptr || cursor->image_count == 0) {
        return;
    }
    wl_cursor_image* image = cursor->images[0]; // frame 0: this backend does not animate cursors
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr) {
        return;
    }
    wl_surface_attach(g_cursor_surface, buffer, 0, 0);
    wl_surface_damage(g_cursor_surface,
                      0,
                      0,
                      static_cast<std::int32_t>(image->width),
                      static_cast<std::int32_t>(image->height));
    wl_surface_commit(g_cursor_surface);
    wl_pointer_set_cursor(g_pointer,
                          g_pointer_enter_serial,
                          g_cursor_surface,
                          static_cast<std::int32_t>(image->hotspot_x),
                          static_cast<std::int32_t>(image->hotspot_y));
}

// Defined below, beside the listener it installs.
void ensure_relative_pointer();

class WaylandWindow final : public Window {
public:
    explicit WaylandWindow(const WindowDesc& desc); // defined below the listeners it wires up

    // ── Pointer capture (m15.5) ──────────────────────────────────────────────────────────────
    // Wayland does not let a client move or grab the pointer; it lets it ASK the compositor to
    // constrain one, and the compositor may simply not offer the protocol. So the honest answer to
    // "did I get Locked?" is computed, never assumed — `active_` is set from what actually
    // succeeded, and that is what `cursor_mode()` returns.
    CursorMode set_cursor_mode(CursorMode mode) override {
        desired_ = mode;
        reapply_cursor_mode();
        return active_;
    }

    [[nodiscard]] CursorMode cursor_mode() const override { return active_; }

    [[nodiscard]] bool is_locked() const noexcept { return active_ == CursorMode::Locked; }

    // Establish (or stand down) whatever the compositor will actually give us. Called on request
    // and again on pointer `enter`, because hiding a cursor needs a serial from a pointer event —
    // a mode set before the pointer ever entered can only take effect once it does.
    void reapply_cursor_mode() {
        const bool have_pointer = g_pointer != nullptr && g_pointer_focus == this;
        CursorMode got = CursorMode::Normal;
        if (have_pointer) {
            // A null cursor surface IS the hide request in this protocol.
            if (desired_ != CursorMode::Normal && ensure_cursor_theme()) {
                wl_pointer_set_cursor(g_pointer, g_pointer_enter_serial, nullptr, 0, 0);
                got = CursorMode::Hidden;
            } else if (desired_ == CursorMode::Normal && active_ != CursorMode::Normal) {
                // Nothing restores the default cursor for us. Leaving it hidden after a mode change
                // is the bug where the pointer never comes back and the user force-quits.
                restore_default_cursor();
            }
        }
        // Belt and braces on registry ordering. `seat_capabilities` normally runs in the second
        // init roundtrip, after every global has bound — but the seat listener is added from within
        // the FIRST roundtrip's dispatch, so a compositor is free to deliver capabilities before
        // the relative-pointer manager has been bound. That would leave a pointer with no relative
        // source and locking permanently unavailable, for a reason nothing would report.
        ensure_relative_pointer();
        // WHY IT DID NOT LOCK, said once. There are three distinct reasons and they need three
        // different responses from whoever is reading: a compositor without the protocol will never
        // lock (ship the drag scheme), a pointer that has not entered the surface yet will lock the
        // moment it does (wait), and neither is distinguishable from the other — or from a bug in
        // this file — by watching the return value alone. Once, because it is re-evaluated on every
        // pointer enter and a per-frame line would bury the log.
        if (desired_ == CursorMode::Locked && !lock_diagnosed_) {
            if (g_pointer_constraints == nullptr || g_relative_pointer_manager == nullptr) {
                lock_diagnosed_ = true;
                RIME_WARN("wayland: this compositor advertises no pointer constraints — the cursor "
                          "cannot be locked, so free look is unavailable");
            } else if (!have_pointer) {
                lock_diagnosed_ = true;
                RIME_INFO("wayland: pointer lock is pending — a surface can only lock a pointer "
                          "that is over it, so move the mouse into the window");
            }
        }
        const bool want_lock = desired_ == CursorMode::Locked && have_pointer;
        if (!want_lock && locked_ != nullptr) {
            zwp_locked_pointer_v1_destroy(locked_);
            locked_ = nullptr;
        }
        if (want_lock && locked_ == nullptr && g_pointer_constraints != nullptr &&
            g_relative_pointer != nullptr) {
            locked_ = zwp_pointer_constraints_v1_lock_pointer(
                g_pointer_constraints,
                surface_,
                g_pointer,
                nullptr,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        }
        if (locked_ != nullptr) {
            got = CursorMode::Locked;
        }
        if (got == CursorMode::Locked && active_ != CursorMode::Locked) {
            // Say it once when it happens. Acquiring the lock is otherwise completely invisible —
            // no event, no error, and the only observable is a camera that starts behaving — so
            // without this line "did the constraint attach?" and "is the rest of the input path
            // broken?" look identical from a log.
            RIME_INFO("wayland: pointer locked — free look is live");
        }
        active_ = got;
    }

    ~WaylandWindow() override {
        // Drop the constraint first: a locked pointer whose surface is gone is the compositor's
        // problem to clean up, and not every compositor does it gracefully.
        if (locked_ != nullptr && g_display != nullptr) {
            zwp_locked_pointer_v1_destroy(locked_);
        }
        locked_ = nullptr;
        g_windows().erase(surface_);
        if (this == g_keyboard_focus) {
            g_keyboard_focus = nullptr;
        }
        if (this == g_pointer_focus) {
            g_pointer_focus = nullptr;
        }
        // If platform::shutdown() already disconnected the display, wl_display_disconnect freed
        // every proxy (toplevel/xdg_surface/surface) with it — destroying them again is a
        // use-after-free and wl_display_flush(nullptr) a null-deref. Guard the Wayland teardown on
        // a live display (the same display-gated teardown-order footgun the X11 backend has; the
        // local focus/registry bookkeeping above is process-local and always safe).
        if (g_display != nullptr) {
            release_placeholder(); // buffer + pool are proxies too: same live-display rule
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
        } else if (pool_data_ != nullptr) {
            // Display already gone: the proxies died with it, but the mapping is ours to return.
            ::munmap(pool_data_, pool_bytes_);
            pool_data_ = nullptr;
        }
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
        // Committing here finishes the surface setup; it cannot map pixels, because mapping needs a
        // buffer and no renderer has had the chance to attach one yet. The window becomes visible
        // on the first buffer — either the renderer's, or the placeholder settle_placeholder()
        // attaches on the first pump if no renderer ever claims this surface.
        wl_surface_commit(surface_);
        wl_display_flush(g_display);
    }

    [[nodiscard]] NativeWindow native_handle() const override {
        // M3's Vulkan backend builds a surface from wl_display* + wl_surface*
        // (VK_KHR_wayland_surface). Taking the handle is also the signal that a renderer owns this
        // surface's pixels from here on, so the placeholder below stands down permanently — see the
        // file header on why this, and not a WindowDesc flag, is the honest test. The store is
        // atomic because this method is `const` and documented stable, so nothing stops a render
        // thread from calling it while the main thread pumps and reads the flag.
        renderer_attached_.store(true, std::memory_order_relaxed);
        return NativeWindow{WindowSystem::Wayland, g_display, surface_};
    }

    // Called once per pump. By the time the first pump runs, any renderer has already taken the
    // native handle during setup, so this is the earliest moment at which "nobody is going to draw
    // this window" is a decidable fact rather than a guess. Note this only records the INTENT to
    // paint: the paint itself must wait for a configure to have been acked (see maybe_paint).
    void settle_placeholder() {
        if (renderer_attached_.load(std::memory_order_relaxed) || placeholder_settled_) {
            return;
        }
        placeholder_settled_ = true;
        maybe_paint_placeholder();
    }

    // The ack half of the xdg-shell configure handshake, and the only place a placeholder paint may
    // originate. xdg-shell is strict about the order here: a client must ack a configure before the
    // commit that answers it, and attaching ANY buffer before the first xdg_surface.configure is a
    // protocol error (`xdg_surface.unconfigured_buffer`), which is fatal — every later dispatch
    // fails and the app soft-hangs with a dead connection.
    //
    // Both halves of that were live traced as real races, not theory. The initial configure arrived
    // ~460us AFTER the constructor's wl_display_roundtrip returned, so painting from the pump was
    // relying on init being slow enough for the bytes to land first — luck that a Release build or
    // a compositor that defers configures to its own layout clock (Mutter, KWin) removes. And
    // painting straight from xdg_toplevel.configure committed the new size BEFORE this ack went
    // out, which compositors enforcing a state mandate (weston, on a maximized window) treat as
    // fatal.
    //
    // So: xdg_toplevel.configure only records a pending size, and everything that touches a buffer
    // happens here, after the ack.
    void on_surface_configured() {
        configured_ = true;
        // Adopting a new size already repaints, so painting again here would allocate and fill a
        // second identical buffer for every configure — measured on the wire as two attach/commit
        // cycles per resize. The else covers the first configure of a window whose size did not
        // change, which is what maps it.
        if (pending_size_) {
            apply_pending_size();
        } else {
            maybe_paint_placeholder();
        }
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

    // xdg_toplevel.configure: the ADVISORY half of the handshake. It records the size and nothing
    // else — no buffer may be touched until the xdg_surface.configure that closes this batch has
    // been acked. If that ack already happened for an earlier batch, the size is applied when the
    // next one arrives, which is the protocol's own pacing rather than ours.
    void notify_configure_size(std::uint32_t w, std::uint32_t h) {
        if (w == width_ && h == height_) {
            return;
        }
        pending_size_ = Extent2D{w, h};
    }

    [[nodiscard]] wl_surface* surface() const { return surface_; }

private:
    // Adopt the size the compositor last proposed, repaint a placeholder if we own the pixels, and
    // tell the app. Runs only from on_surface_configured(), i.e. only after an ack.
    void apply_pending_size() {
        const Extent2D size = *pending_size_;
        pending_size_.reset();
        if (size.width == width_ && size.height == height_) {
            return;
        }
        width_ = size.width;
        height_ = size.height;
        // A rendererless window has to repaint itself at the new size or the compositor keeps
        // showing the old, smaller buffer — the very artefact this backend is fixing. A window with
        // a renderer is left alone: attaching here would fight its presents.
        maybe_paint_placeholder();
        Event e{};
        e.type = EventType::WindowResize;
        e.window = id_;
        e.resize.size = Extent2D{width_, height_};
        post_event(e);
    }

    // The single gate every placeholder paint passes through: we must want one, no renderer may
    // have claimed the surface, and a configure must already be acked.
    void maybe_paint_placeholder() {
        if (!placeholder_settled_ || !configured_ ||
            renderer_attached_.load(std::memory_order_relaxed)) {
            return;
        }
        paint_placeholder();
    }

    // Attach an opaque, window-sized buffer so the compositor maps the surface. XRGB8888 is the one
    // format wl_shm is required to support, and being opaque it also spares the compositor a
    // blending pass for a window that is only ever a flat backdrop.
    void paint_placeholder() {
        if (g_shm == nullptr || width_ == 0 || height_ == 0) {
            return; // no wl_shm global, or no size yet: nothing sensible to attach
        }
        const std::size_t stride = static_cast<std::size_t>(width_) * 4u;
        const std::size_t bytes = stride * height_;
        if (bytes > static_cast<std::size_t>(INT32_MAX)) {
            return; // wl_shm_create_pool takes an int32 size; a negative one is a protocol error
        }

        // Reallocate only when the window grew; shrinking reuses the existing mapping. The pool is
        // rebuilt with it because a wl_shm_pool is bound to the fd's size at creation.
        if (bytes > pool_bytes_) {
            release_placeholder();
            const int fd = make_shm_fd(bytes);
            if (fd < 0) {
                return;
            }
            void* data = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (data == MAP_FAILED) {
                ::close(fd);
                return;
            }
            pool_ = wl_shm_create_pool(g_shm, fd, static_cast<std::int32_t>(bytes));
            ::close(fd); // the pool holds its own reference; our copy is done
            pool_data_ = data;
            pool_bytes_ = bytes;
        }
        if (pool_ == nullptr || pool_data_ == nullptr) {
            return;
        }

        if (buffer_ != nullptr) {
            wl_buffer_destroy(buffer_); // one buffer per size; the pool memory outlives it
        }
        buffer_ = wl_shm_pool_create_buffer(pool_,
                                            0,
                                            static_cast<std::int32_t>(width_),
                                            static_cast<std::int32_t>(height_),
                                            static_cast<std::int32_t>(stride),
                                            WL_SHM_FORMAT_XRGB8888);
        if (buffer_ == nullptr) {
            return;
        }

        // A flat neutral fill. Deliberately not black: a window that is exactly the desktop's
        // background colour is indistinguishable from not having opened, which is the failure this
        // whole path exists to make impossible to mistake.
        std::uint32_t* px = static_cast<std::uint32_t*>(pool_data_);
        const std::size_t count = static_cast<std::size_t>(width_) * height_;
        for (std::size_t i = 0; i < count; ++i) {
            px[i] = 0x00181c24u; // XRGB: x=0, R=0x18, G=0x1c, B=0x24
        }

        wl_surface_attach(surface_, buffer_, 0, 0);
        // wl_surface_damage, not damage_buffer: the latter is a v4 request and the compositor bind
        // accepts older versions, where calling it is a fatal `invalid_method`. The two differ only
        // in coordinate space (surface vs buffer), and with no buffer scale ever set on this
        // surface the two spaces are identical — so the deprecated one is exactly right and
        // version-safe.
        wl_surface_damage(
            surface_, 0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_));
        wl_surface_commit(surface_);
        wl_display_flush(g_display);
    }

    void release_placeholder() {
        if (buffer_ != nullptr) {
            wl_buffer_destroy(buffer_);
            buffer_ = nullptr;
        }
        if (pool_ != nullptr) {
            wl_shm_pool_destroy(pool_);
            pool_ = nullptr;
        }
        if (pool_data_ != nullptr) {
            ::munmap(pool_data_, pool_bytes_);
            pool_data_ = nullptr;
        }
        pool_bytes_ = 0;
    }

    WindowId id_;
    // Cursor mode: what was asked for, what the compositor actually gave, and the constraint object
    // itself (null whenever we are not locked, for any reason).
    CursorMode desired_ = CursorMode::Normal;
    CursorMode active_ = CursorMode::Normal;
    zwp_locked_pointer_v1* locked_ = nullptr;
    bool lock_diagnosed_ = false; // the "why not" above is said once, not once per pointer enter
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool should_close_ = false;

    // Placeholder state. `renderer_attached_` is mutable because native_handle() is const by the
    // Window interface, and the observation it records — "someone asked for the raw surface" — is
    // about this object's future, not about the value it returned. It is atomic because that method
    // is documented as a stable getter, so a caller is entitled to use it off-thread.
    mutable std::atomic<bool> renderer_attached_{false};
    bool placeholder_settled_ = false;
    bool configured_ = false;              // an xdg_surface.configure has been acked
    std::optional<Extent2D> pending_size_; // proposed by xdg_toplevel.configure, not yet adopted
    wl_shm_pool* pool_ = nullptr;
    wl_buffer* buffer_ = nullptr;
    void* pool_data_ = nullptr;
    std::size_t pool_bytes_ = 0;
};

// ── Input listener callbacks ─────────────────────────────────────────────────────
// Each is a plain C callback matching the protocol's listener signature; the structs that collect
// them are defined just below. registry_global caps the wl_seat bind at version 5 (see below), and
// wl_pointer/wl_keyboard inherit that negotiated version from the wl_seat they are created from —
// so any event "since" that version or earlier is fair game for the compositor to send, and the
// listener slot for it must be non-null or libwayland aborts with "listener function for opcode N
// of <interface> is NULL" the first time it arrives. Only events introduced *above* the bound
// version (wl_pointer's axis_value120 at v8, axis_relative_direction at v9) are actually
// unreachable and safe to leave null.

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

// Reachable at the bound version (since 4) but there is no client-side repeat timer yet — kb_key
// always reports repeat = false — so the rate/delay the compositor hands us has nowhere to go.
void kb_repeat_info(void*, wl_keyboard*, std::int32_t, std::int32_t) {}

// Value-initialize the listener (every member null) and then set only the events we act on, plus
// the reachable-but-unused ones (repeat_info) as explicit no-ops so libwayland never dispatches
// through a null slot. Members beyond the bound version stay null; see the comment above.
const wl_keyboard_listener g_keyboard_listener = [] {
    wl_keyboard_listener l{};
    l.keymap = kb_keymap;
    l.enter = kb_enter;
    l.leave = kb_leave;
    l.key = kb_key;
    l.modifiers = kb_modifiers;
    l.repeat_info = kb_repeat_info;
    return l;
}();

void ptr_enter(void*,
               wl_pointer*,
               std::uint32_t serial,
               wl_surface* surface,
               wl_fixed_t sx,
               wl_fixed_t sy) {
    g_pointer_focus = window_for_surface(surface);
    g_pointer_x = wl_fixed_to_double(sx);
    g_pointer_y = wl_fixed_to_double(sy);
    g_pointer_have_last = false; // first motion after entering reports zero delta
    // Hiding a cursor needs a serial from a pointer event and the pointer must be over the surface,
    // so entering is the first moment a requested Hidden/Locked can actually be honoured — a mode
    // set before the pointer ever arrived would otherwise be silently lost (m15.5).
    g_pointer_enter_serial = serial;
    if (g_pointer_focus != nullptr) {
        g_pointer_focus->reapply_cursor_mode();
    }
}

void ptr_leave(void*, wl_pointer*, std::uint32_t, wl_surface*) {
    g_pointer_focus = nullptr;
}

void ptr_motion(void*, wl_pointer*, std::uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    if (g_pointer_focus == nullptr) {
        return;
    }
    // While locked the compositor pins the cursor, so absolute motion is either absent or a
    // constant — and forwarding it would post a MouseMove with a real position and a zero delta
    // between every relative event, which reads downstream as "the mouse stopped". The relative
    // pointer is the only source in this mode.
    if (g_pointer_focus->is_locked()) {
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

// frame/axis_source/axis_stop/axis_discrete are all reachable at the bound version (since 5) but
// we don't do frame-grouped or source-aware scroll handling — ptr_axis already emits one
// MouseWheel event per axis event, which is enough for the engine today. Explicit no-ops so
// libwayland never dispatches through a null slot (see the comment above kb_keymap).
void ptr_frame(void*, wl_pointer*) {}

void ptr_axis_source(void*, wl_pointer*, std::uint32_t) {}

void ptr_axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}

void ptr_axis_discrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}

const wl_pointer_listener g_pointer_listener = [] {
    wl_pointer_listener l{};
    l.enter = ptr_enter;
    l.leave = ptr_leave;
    l.motion = ptr_motion;
    l.button = ptr_button;
    l.axis = ptr_axis;
    l.frame = ptr_frame;
    l.axis_source = ptr_axis_source;
    l.axis_stop = ptr_axis_stop;
    l.axis_discrete = ptr_axis_discrete;
    return l;
}();

// RELATIVE MOTION — the other half of the lock (m15.5). The compositor sends this whenever the
// pointer moves, whether or not anything is constrained, so it is gated on the focused window
// actually being locked: unlocked, `ptr_motion` already reports movement with a delta, and posting
// both would double every mouse movement in the engine's accumulator.
//
// `dx`/`dy` are pointer-accelerated; `dx_unaccel`/`dy_unaccel` are the raw device deltas. The
// ACCELERATED pair is the right default for a camera: it is what the user's own pointer-speed and
// acceleration settings produce everywhere else on their desktop, so a look that used the raw
// values would ignore the configuration the user already chose.
void relative_motion(void*,
                     zwp_relative_pointer_v1*,
                     std::uint32_t,
                     std::uint32_t,
                     wl_fixed_t dx,
                     wl_fixed_t dy,
                     wl_fixed_t,
                     wl_fixed_t) {
    if (g_pointer_focus == nullptr || !g_pointer_focus->is_locked()) {
        return;
    }
    Event e{};
    e.type = EventType::MouseMove;
    e.window = g_pointer_focus->id();
    // The position where the lock caught it, unchanged for as long as the lock holds — because
    // that is literally where the pointer is. `ptr_motion` is suppressed while locked, so nothing
    // else advances these, and a caller reading x/y during free look correctly sees a pointer that
    // is not moving. It is not a meaningful cursor location to draw a UI at; that is what the mode
    // is for.
    e.mouse_move.x = static_cast<float>(g_pointer_x);
    e.mouse_move.y = static_cast<float>(g_pointer_y);
    e.mouse_move.dx = static_cast<float>(wl_fixed_to_double(dx));
    e.mouse_move.dy = static_cast<float>(wl_fixed_to_double(dy));
    post_event(e);
}

const zwp_relative_pointer_v1_listener g_relative_pointer_listener = {
    .relative_motion = relative_motion,
};

void ensure_relative_pointer() {
    if (g_relative_pointer != nullptr || g_relative_pointer_manager == nullptr ||
        g_pointer == nullptr) {
        return;
    }
    g_relative_pointer =
        zwp_relative_pointer_manager_v1_get_relative_pointer(g_relative_pointer_manager, g_pointer);
    zwp_relative_pointer_v1_add_listener(g_relative_pointer, &g_relative_pointer_listener, nullptr);
}

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
        // One relative pointer per wl_pointer, created here rather than per lock: it is a property
        // of the DEVICE, and creating it lazily inside set_cursor_mode would mean the first lock
        // silently produced no motion until the next round trip.
        ensure_relative_pointer();
    } else if (!has_pointer && g_pointer != nullptr) {
        if (g_relative_pointer != nullptr) {
            zwp_relative_pointer_v1_destroy(g_relative_pointer);
            g_relative_pointer = nullptr;
        }
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

void surface_configure(void* data, xdg_surface* surface, std::uint32_t serial) {
    // Ack first, then let the window act on the batch — that order is what xdg-shell requires of
    // every commit answering a configure, and it is the whole reason painting lives here rather
    // than in toplevel_configure or the pump.
    xdg_surface_ack_configure(surface, serial);
    static_cast<WaylandWindow*>(data)->on_surface_configured();
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
        static_cast<WaylandWindow*>(data)->notify_configure_size(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    }
}

void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<WaylandWindow*>(data)->notify_close();
}

const xdg_toplevel_listener g_toplevel_listener = [] {
    xdg_toplevel_listener l{};
    l.configure = toplevel_configure;
    l.close = toplevel_close;
    return l;
}();

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
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        g_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        g_pointer_constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        g_relative_pointer_manager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
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
    if (g_shm != nullptr) {
        wl_shm_destroy(g_shm);
        g_shm = nullptr;
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

    // Give any window nobody is rendering to its placeholder buffer, so it actually maps. This runs
    // after dispatch so the first pass acts on a size the compositor has already configured.
    for (auto& [surface, window] : g_windows()) {
        (void)surface;
        window->settle_placeholder();
    }
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
