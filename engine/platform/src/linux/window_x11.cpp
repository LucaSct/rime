// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "keymap_linux.hpp"
#include "linux_backend.hpp"
#include "platform_backend.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// X11 headers must come *after* ours: <X11/Xlib.h> #defines `None`, `Bool`, … and typedefs
// `Window`, which collide with our EventType::None / KeyMods::None and our Window class. Parsing
// our headers first keeps those enums and the class intact; we then #undef the one macro we still
// need to spell (`None`) and refer to Xlib's window id as the global `::Window`. The #ifdef wrapper
// is also a barrier that stops clang-format (IncludeBlocks: Regroup) hoisting these above ours.
#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#undef None

// X11 (Xlib) window backend. The classic Xlib client: open a Display, create a window, opt into
// WM_DELETE_WINDOW so the close box is a message we handle (not a kill), select the event masks we
// care about, and translate each XEvent into our POD Event in the pump. Keys carry the physical
// evdev code (X11 keycode = evdev + 8 -> key_from_evdev); typed text comes from XLookupString.
//
// HiDPI on X11 has no single honest source (Xft.dpi, RandR scales, per-monitor differences), so
// content_scale() reports 1.0 for now and framebuffer == logical; proper scaling is a later brick.
namespace rime::platform {
namespace {

Display* g_display = nullptr;
Atom g_wm_delete = 0;

class X11Window;

// Maps each X11 window id to its C++ wrapper so the pump can route an XEvent to the right window. A
// function-local static -> constructed on first use, with no static-init-order concerns.
std::unordered_map<::Window, X11Window*>& g_windows() {
    static std::unordered_map<::Window, X11Window*> windows;
    return windows;
}

KeyMods mods_from_state(unsigned int state) {
    KeyMods m = KeyMods::None;
    if ((state & ShiftMask) != 0) {
        m |= KeyMods::Shift;
    }
    if ((state & ControlMask) != 0) {
        m |= KeyMods::Ctrl;
    }
    if ((state & Mod1Mask) != 0) {
        m |= KeyMods::Alt;
    }
    if ((state & Mod4Mask) != 0) {
        m |= KeyMods::Super;
    }
    return m;
}

class X11Window final : public Window {
public:
    explicit X11Window(const WindowDesc& desc)
        : id_(detail::allocate_window_id()), width_(desc.width), height_(desc.height) {
        const int screen = DefaultScreen(g_display);
        const ::Window root = RootWindow(g_display, screen);
        window_ = XCreateSimpleWindow(g_display,
                                      root,
                                      0,
                                      0,
                                      width_,
                                      height_,
                                      0,
                                      BlackPixel(g_display, screen),
                                      BlackPixel(g_display, screen));

        // Close box -> ClientMessage we handle, rather than the server killing our connection.
        XSetWMProtocols(g_display, window_, &g_wm_delete, 1);

        // The events we translate: keys, buttons, motion, structure (resize), and focus.
        XSelectInput(g_display,
                     window_,
                     KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | StructureNotifyMask | FocusChangeMask | ExposureMask);

        if (!desc.resizable) {
            // Pin the size by setting equal min/max hints (the WM enforces it).
            XSizeHints* hints = XAllocSizeHints();
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = static_cast<int>(width_);
            hints->min_height = hints->max_height = static_cast<int>(height_);
            XSetWMNormalHints(g_display, window_, hints);
            XFree(hints);
        }

        apply_title(desc.title);
        g_windows().emplace(window_, this);
    }

    ~X11Window() override {
        g_windows().erase(window_);
        // A window may outlive platform::shutdown() (a teardown-order mistake, but the destructor
        // must survive it, not segfault). If the X connection is already closed, XCloseDisplay
        // freed this window's server-side resources with it — so XDestroyWindow here would both
        // deref a null Display* and double-free. Skipping it is correct, not just defensive. This
        // path is display-gated, so CI's off-screen proof never reached it (an Xvfb run / the S0
        // windowed client is what surfaced the crash).
        if (g_display != nullptr) {
            // Release the pointer BEFORE the window goes: an active grab outliving its window is
            // how an X session ends up with a desktop that will not respond to the mouse, and this
            // destructor is the only place that can prevent it (m15.5).
            if (active_ == CursorMode::Locked) {
                XUngrabPointer(g_display, CurrentTime);
            }
            if (blank_cursor_ != 0) {
                XFreeCursor(g_display, blank_cursor_);
            }
            XDestroyWindow(g_display, window_);
            XFlush(g_display);
        }
    }

    void set_title(std::string_view title) override {
        apply_title(title);
        XFlush(g_display);
    }

    void set_size(Extent2D size) override {
        width_ = size.width;
        height_ = size.height;
        XResizeWindow(g_display, window_, width_, height_);
        XFlush(g_display);
    }

    [[nodiscard]] Extent2D framebuffer_size() const override { return Extent2D{width_, height_}; }

    [[nodiscard]] Extent2D logical_size() const override { return Extent2D{width_, height_}; }

    [[nodiscard]] float content_scale() const override { return 1.0f; }

    [[nodiscard]] bool should_close() const override { return should_close_; }

    void request_close() override { notify_close(); }

    void show() override {
        XMapWindow(g_display, window_);
        XFlush(g_display);
    }

    [[nodiscard]] NativeWindow native_handle() const override {
        // M3's Vulkan backend builds a surface from Display* + the window XID
        // (VK_KHR_xlib_surface).
        return NativeWindow{WindowSystem::Xlib,
                            g_display,
                            reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_))};
    }

    [[nodiscard]] WindowId id() const override { return id_; }

    // Translate one XEvent targeting this window. Called by the backend pump after it has matched
    // the event's window id to this instance.
    void handle_event(XEvent& ev) {
        switch (ev.type) {
            case ClientMessage:
                if (static_cast<Atom>(ev.xclient.data.l[0]) == g_wm_delete) {
                    notify_close();
                }
                break;
            case ConfigureNotify: {
                const auto w = static_cast<std::uint32_t>(ev.xconfigure.width);
                const auto h = static_cast<std::uint32_t>(ev.xconfigure.height);
                if (w != width_ || h != height_) {
                    width_ = w;
                    height_ = h;
                    Event e{};
                    e.type = EventType::WindowResize;
                    e.window = id_;
                    e.resize.size = Extent2D{width_, height_};
                    post_event(e);
                }
                break;
            }
            case FocusIn:
            case FocusOut: {
                Event e{};
                e.type = EventType::WindowFocus;
                e.window = id_;
                e.focus.focused = (ev.type == FocusIn);
                focus_changed(e.focus.focused);
                post_event(e);
                break;
            }
            case KeyPress:
            case KeyRelease:
                handle_key(ev.xkey, ev.type == KeyPress);
                break;
            case ButtonPress:
            case ButtonRelease:
                handle_button(ev.xbutton, ev.type == ButtonPress);
                break;
            case MotionNotify:
                handle_motion(ev.xmotion);
                break;
            default:
                break;
        }
    }

    // POINTER CAPTURE, XLIB ONLY (m15.5). Xlib has no relative-motion source of its own — XI2 raw
    // events do, but pulling in libXi for one feature is a dependency this backend does not
    // otherwise need — so Locked is the classic warp-and-recentre: hide the cursor, grab the
    // pointer so it cannot leave, and after every motion warp it back to the middle. dx/dy are then
    // the distance from the middle, which is exactly the relative motion a camera wants.
    CursorMode set_cursor_mode(CursorMode mode) override {
        desired_ = mode;
        apply_cursor_mode();
        return active_;
    }

    [[nodiscard]] CursorMode cursor_mode() const override { return active_; }

private:
    // An empty 1x1 cursor. X11 has no "hide the pointer" call; the idiom is to define a cursor made
    // of nothing, which is what every toolkit does. Created once, on first use.
    ::Cursor invisible_cursor() {
        if (blank_cursor_ != 0) {
            return blank_cursor_;
        }
        char nothing[8] = {};
        Pixmap pm = XCreateBitmapFromData(g_display, window_, nothing, 1, 1);
        XColor black{};
        blank_cursor_ = XCreatePixmapCursor(g_display, pm, pm, &black, &black, 0, 0);
        XFreePixmap(g_display, pm);
        return blank_cursor_;
    }

    void warp_to_centre() {
        // 0L, not `None`: this file #undef s that macro at the top (it collides with our own enum
        // names), and the src argument means "relative to wherever the pointer already is".
        XWarpPointer(g_display,
                     0L,
                     window_,
                     0,
                     0,
                     0,
                     0,
                     static_cast<int>(width_ / 2),
                     static_cast<int>(height_ / 2));
        XFlush(g_display);
    }

    void apply_cursor_mode() {
        // Focus is the gate on Locked, not a suggestion: a grabbed pointer belongs to a window the
        // user is not looking at, which reads as a frozen desktop. So an unfocused window drops to
        // the strongest mode it can honestly hold, and `focus_changed` puts it back.
        const CursorMode want =
            (desired_ == CursorMode::Locked && !focused_) ? CursorMode::Normal : desired_;
        if (want == active_) {
            return;
        }
        if (active_ == CursorMode::Locked) {
            XUngrabPointer(g_display, CurrentTime);
        }
        switch (want) {
            case CursorMode::Normal:
                XUndefineCursor(g_display, window_);
                break;
            case CursorMode::Hidden:
                XDefineCursor(g_display, window_, invisible_cursor());
                break;
            case CursorMode::Locked: {
                XDefineCursor(g_display, window_, invisible_cursor());
                // ConfineTo the window AND owner_events=True, so our own key/button events keep
                // arriving normally while the pointer cannot leave.
                const int grabbed =
                    XGrabPointer(g_display,
                                 window_,
                                 True,
                                 PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                 GrabModeAsync,
                                 GrabModeAsync,
                                 window_,
                                 invisible_cursor(),
                                 CurrentTime);
                if (grabbed != GrabSuccess) {
                    // Named, because the three ways this fails want three different responses and
                    // the return value alone cannot tell them apart: another client owns the
                    // pointer (a menu, a drag, a screen locker), the window is not viewable yet, or
                    // this code is wrong. Once — it is retried on every focus change.
                    if (!grab_diagnosed_) {
                        grab_diagnosed_ = true;
                        RIME_WARN("x11: XGrabPointer refused ({}) — another client holds the "
                                  "pointer, so the cursor cannot be locked",
                                  grabbed);
                    }
                    // Another client holds the pointer (a menu, a drag, a screen locker). Report
                    // what we actually have rather than what was asked for — the whole point of the
                    // return value.
                    XDefineCursor(g_display, window_, invisible_cursor());
                    active_ = CursorMode::Hidden;
                    XFlush(g_display);
                    return;
                }
                warp_to_centre();
                break;
            }
        }
        if (want == CursorMode::Locked && active_ != CursorMode::Locked) {
            // Acquiring the grab is otherwise invisible: no event, no error, and the only
            // observable is a camera that starts behaving.
            RIME_INFO("x11: pointer grabbed — free look is live");
        }
        active_ = want;
        XFlush(g_display);
    }

    // Called from the FocusIn/FocusOut arm: re-establish or stand down the grab.
    void focus_changed(bool focused) {
        focused_ = focused;
        apply_cursor_mode();
    }

    void apply_title(std::string_view title) {
        const std::string t(title);
        XStoreName(g_display, window_, t.c_str()); // legacy WM_NAME (Latin-1)
        const Atom net_name = XInternAtom(g_display, "_NET_WM_NAME", False);
        const Atom utf8 = XInternAtom(g_display, "UTF8_STRING", False);
        XChangeProperty(g_display,
                        window_,
                        net_name,
                        utf8,
                        8,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(t.c_str()),
                        static_cast<int>(t.size()));
    }

    void handle_key(XKeyEvent& ke, bool down) {
        Event e{};
        e.type = down ? EventType::KeyDown : EventType::KeyUp;
        e.window = id_;
        // X11 keycodes are evdev + 8 by the long-standing XKB convention.
        e.key.key = detail::key_from_evdev(ke.keycode >= 8 ? ke.keycode - 8 : 0);
        e.key.mods = mods_from_state(ke.state);
        e.key.repeat = false; // X11 auto-repeat is release+press pairs; not distinguished yet
        post_event(e);

        if (down) {
            // XLookupString yields the typed bytes (Latin-1/ASCII). Full UTF-8/IME needs an XIM
            // input context, a later refinement; ASCII text already drives the sample.
            char buf[16] = {};
            KeySym keysym = 0;
            const int n = XLookupString(&ke, buf, sizeof(buf) - 1, &keysym, nullptr);
            if (n > 0 && static_cast<unsigned char>(buf[0]) >= 0x20 &&
                static_cast<unsigned char>(buf[0]) != 0x7F) {
                Event te{};
                te.type = EventType::TextInput;
                te.window = id_;
                for (int i = 0; i < n && i < static_cast<int>(sizeof(te.text.utf8) - 1); ++i) {
                    te.text.utf8[i] = buf[i];
                }
                post_event(te);
            }
        }
    }

    void handle_button(XButtonEvent& be, bool down) {
        // Buttons 4-7 are the scroll wheel on X11; emit a wheel event once (on press).
        if (be.button >= 4 && be.button <= 7) {
            if (!down) {
                return;
            }
            Event e{};
            e.type = EventType::MouseWheel;
            e.window = id_;
            e.wheel.dy = (be.button == 4) ? 1.0f : (be.button == 5) ? -1.0f : 0.0f;
            e.wheel.dx = (be.button == 6) ? -1.0f : (be.button == 7) ? 1.0f : 0.0f;
            post_event(e);
            return;
        }
        MouseButton button = MouseButton::Left;
        switch (be.button) {
            case Button1:
                button = MouseButton::Left;
                break;
            case Button2:
                button = MouseButton::Middle;
                break;
            case Button3:
                button = MouseButton::Right;
                break;
            case 8:
                button = MouseButton::X1;
                break;
            case 9:
                button = MouseButton::X2;
                break;
            default:
                return;
        }
        Event e{};
        e.type = EventType::MouseButton;
        e.window = id_;
        e.button.button = button;
        e.button.mods = mods_from_state(be.state);
        e.button.down = down;
        post_event(e);
    }

    void handle_motion(XMotionEvent& me) {
        const auto x = static_cast<float>(me.x);
        const auto y = static_cast<float>(me.y);
        if (active_ == CursorMode::Locked) {
            // Relative to the middle, then put it back. The warp itself generates a MotionNotify at
            // exactly the middle, and THAT is the event this swallows: a zero delta is either the
            // echo of our own warp or a motion that moved nothing, and both are nothing to report.
            // (Trying to match the echo by position instead would also swallow a real motion that
            // happened to land dead centre — same outcome, more state.)
            const auto cx = static_cast<float>(width_ / 2);
            const auto cy = static_cast<float>(height_ / 2);
            const float dx = x - cx;
            const float dy = y - cy;
            if (dx == 0.0f && dy == 0.0f) {
                return;
            }
            warp_to_centre();
            Event le{};
            le.type = EventType::MouseMove;
            le.window = id_;
            le.mouse_move.x = cx; // pinned: an absolute position is meaningless while locked
            le.mouse_move.y = cy;
            le.mouse_move.dx = dx;
            le.mouse_move.dy = dy;
            post_event(le);
            return;
        }
        Event e{};
        e.type = EventType::MouseMove;
        e.window = id_;
        e.mouse_move.x = x;
        e.mouse_move.y = y;
        e.mouse_move.dx = have_last_mouse_ ? x - last_mouse_x_ : 0.0f;
        e.mouse_move.dy = have_last_mouse_ ? y - last_mouse_y_ : 0.0f;
        last_mouse_x_ = x;
        last_mouse_y_ = y;
        have_last_mouse_ = true;
        post_event(e);
    }

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

    WindowId id_;
    ::Window window_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    float last_mouse_x_ = 0.0f;
    float last_mouse_y_ = 0.0f;
    bool have_last_mouse_ = false;
    bool should_close_ = false;
    // What the caller asked for, and what we actually hold. They differ whenever the window is
    // unfocused or another client owns the pointer, and `cursor_mode()` reports the second.
    CursorMode desired_ = CursorMode::Normal;
    CursorMode active_ = CursorMode::Normal;
    bool focused_ = false;
    ::Cursor blank_cursor_ = 0;
    bool grab_diagnosed_ = false; // the refusal above is said once, not once per focus change
};

bool x11_init() {
    XInitThreads(); // we pump on one thread, but X requires this before any threaded use later
    g_display = XOpenDisplay(nullptr);
    if (g_display == nullptr) {
        return false; // no X server (e.g. a pure-Wayland or headless session) -> caller falls back
    }
    g_wm_delete = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    return true;
}

void x11_shutdown() {
    if (g_display != nullptr) {
        XCloseDisplay(g_display);
        g_display = nullptr;
    }
}

void x11_pump() {
    while (XPending(g_display) > 0) {
        XEvent ev;
        XNextEvent(g_display, &ev);
        const auto it = g_windows().find(ev.xany.window);
        if (it != g_windows().end()) {
            it->second->handle_event(ev);
        }
    }
}

std::unique_ptr<Window> x11_create_window(const WindowDesc& desc) {
    return std::make_unique<X11Window>(desc);
}

} // namespace

namespace detail {

const LinuxBackend& x11_backend() {
    static const LinuxBackend backend{x11_init, x11_shutdown, x11_pump, x11_create_window};
    return backend;
}

} // namespace detail
} // namespace rime::platform
