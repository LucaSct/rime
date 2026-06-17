// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep <windows.h> from defining min()/max() macros that break std::min/max.
#endif
#include <windows.h>
// <windowsx.h> provides GET_X_LPARAM / GET_Y_LPARAM, which sign-extend mouse coords correctly
// (a bare LOWORD() truncates to an unsigned WORD and breaks on multi-monitor negative positions).
#include <windowsx.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "platform_backend.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// Win32 (Windows) window backend.
//
// The shape mirrors every classic Win32 app, with three engine-specific choices worth naming:
//   1. We register one window class whose WndProc is the single funnel for OS messages. Each HWND
//      stores its owning C++ Win32Window* in GWLP_USERDATA (set during WM_NCCREATE), so the static
//      WndProc can recover the instance and translate the message into our POD Event.
//   2. We are per-monitor-DPI-aware v2 (set once in native_init). Sizes are therefore real pixels;
//      content_scale() reports the monitor's scale so the framebuffer can be sized correctly.
//   3. We do not call a blocking message loop — native_pump() drains with PeekMessageW so the
//   engine
//      keeps the frame loop, exactly as the Cocoa backend pumps NSEvents by hand.
//
// Keys are identified by their *scancode* (the physical key), not the layout-dependent virtual-key,
// so WASD stays WASD on any layout — the same physical-key promise the Cocoa backend keeps.

using namespace rime::platform;

namespace {

constexpr const wchar_t* kWindowClassName = L"RimeWindowClass";

// UTF-8 (our string vocabulary) -> UTF-16 (what the Win32 *W APIs speak).
std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
    return wide;
}

// Modifier state at the moment an event is delivered, read from the live key state.
KeyMods current_mods() {
    KeyMods m = KeyMods::None;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        m |= KeyMods::Shift;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        m |= KeyMods::Ctrl;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        m |= KeyMods::Alt;
    }
    if (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) {
        m |= KeyMods::Super;
    }
    return m;
}

// Map a Win32 "Set 1" scancode (physical key) to our Key. The `extended` flag (lParam bit 24)
// distinguishes the duplicate keys: right-hand modifiers, the navigation cluster vs. the keypad,
// and the keypad's Enter/Divide. Unmapped scancodes fall through to Unknown.
Key key_from_scancode(WORD sc, bool extended) {
    if (extended) {
        switch (sc) {
            case 0x1C:
                return Key::KPEnter;
            case 0x1D:
                return Key::RightCtrl;
            case 0x35:
                return Key::KPDivide;
            case 0x38:
                return Key::RightAlt;
            case 0x47:
                return Key::Home;
            case 0x48:
                return Key::Up;
            case 0x49:
                return Key::PageUp;
            case 0x4B:
                return Key::Left;
            case 0x4D:
                return Key::Right;
            case 0x4F:
                return Key::End;
            case 0x50:
                return Key::Down;
            case 0x51:
                return Key::PageDown;
            case 0x52:
                return Key::Insert;
            case 0x53:
                return Key::Delete;
            case 0x5B:
                return Key::LeftSuper;
            case 0x5C:
                return Key::RightSuper;
            case 0x5D:
                return Key::Menu;
            default:
                return Key::Unknown;
        }
    }
    switch (sc) {
        case 0x01:
            return Key::Escape;
        case 0x02:
            return Key::Num1;
        case 0x03:
            return Key::Num2;
        case 0x04:
            return Key::Num3;
        case 0x05:
            return Key::Num4;
        case 0x06:
            return Key::Num5;
        case 0x07:
            return Key::Num6;
        case 0x08:
            return Key::Num7;
        case 0x09:
            return Key::Num8;
        case 0x0A:
            return Key::Num9;
        case 0x0B:
            return Key::Num0;
        case 0x0C:
            return Key::Minus;
        case 0x0D:
            return Key::Equal;
        case 0x0E:
            return Key::Backspace;
        case 0x0F:
            return Key::Tab;
        case 0x10:
            return Key::Q;
        case 0x11:
            return Key::W;
        case 0x12:
            return Key::E;
        case 0x13:
            return Key::R;
        case 0x14:
            return Key::T;
        case 0x15:
            return Key::Y;
        case 0x16:
            return Key::U;
        case 0x17:
            return Key::I;
        case 0x18:
            return Key::O;
        case 0x19:
            return Key::P;
        case 0x1A:
            return Key::LeftBracket;
        case 0x1B:
            return Key::RightBracket;
        case 0x1C:
            return Key::Enter;
        case 0x1D:
            return Key::LeftCtrl;
        case 0x1E:
            return Key::A;
        case 0x1F:
            return Key::S;
        case 0x20:
            return Key::D;
        case 0x21:
            return Key::F;
        case 0x22:
            return Key::G;
        case 0x23:
            return Key::H;
        case 0x24:
            return Key::J;
        case 0x25:
            return Key::K;
        case 0x26:
            return Key::L;
        case 0x27:
            return Key::Semicolon;
        case 0x28:
            return Key::Apostrophe;
        case 0x29:
            return Key::Grave;
        case 0x2A:
            return Key::LeftShift;
        case 0x2B:
            return Key::Backslash;
        case 0x2C:
            return Key::Z;
        case 0x2D:
            return Key::X;
        case 0x2E:
            return Key::C;
        case 0x2F:
            return Key::V;
        case 0x30:
            return Key::B;
        case 0x31:
            return Key::N;
        case 0x32:
            return Key::M;
        case 0x33:
            return Key::Comma;
        case 0x34:
            return Key::Period;
        case 0x35:
            return Key::Slash;
        case 0x36:
            return Key::RightShift;
        case 0x37:
            return Key::KPMultiply;
        case 0x38:
            return Key::LeftAlt;
        case 0x39:
            return Key::Space;
        case 0x3A:
            return Key::CapsLock;
        case 0x3B:
            return Key::F1;
        case 0x3C:
            return Key::F2;
        case 0x3D:
            return Key::F3;
        case 0x3E:
            return Key::F4;
        case 0x3F:
            return Key::F5;
        case 0x40:
            return Key::F6;
        case 0x41:
            return Key::F7;
        case 0x42:
            return Key::F8;
        case 0x43:
            return Key::F9;
        case 0x44:
            return Key::F10;
        case 0x45:
            return Key::NumLock;
        case 0x46:
            return Key::ScrollLock;
        case 0x47:
            return Key::KP7;
        case 0x48:
            return Key::KP8;
        case 0x49:
            return Key::KP9;
        case 0x4A:
            return Key::KPSubtract;
        case 0x4B:
            return Key::KP4;
        case 0x4C:
            return Key::KP5;
        case 0x4D:
            return Key::KP6;
        case 0x4E:
            return Key::KPAdd;
        case 0x4F:
            return Key::KP1;
        case 0x50:
            return Key::KP2;
        case 0x51:
            return Key::KP3;
        case 0x52:
            return Key::KP0;
        case 0x53:
            return Key::KPDecimal;
        case 0x57:
            return Key::F11;
        case 0x58:
            return Key::F12;
        default:
            return Key::Unknown;
    }
}

class Win32Window final : public Window {
public:
    explicit Win32Window(const WindowDesc& desc) : id_(detail::allocate_window_id()) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!desc.resizable) {
            style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
        // Translate the desired *client* size into the *window* size that includes the frame, at
        // the system DPI (the window does not exist yet, so there is no per-window DPI to ask).
        RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());
        const std::wstring title = widen(desc.title);
        hwnd_ = CreateWindowExW(0,
                                kWindowClassName,
                                title.c_str(),
                                style,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                rect.right - rect.left,
                                rect.bottom - rect.top,
                                nullptr,
                                nullptr,
                                instance,
                                this);
        // hwnd_ stays null on failure; native_create_window() turns that into a null Window.
    }

    ~Win32Window() override {
        if (hwnd_ != nullptr) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
    }

    [[nodiscard]] bool valid() const { return hwnd_ != nullptr; }

    void set_title(std::string_view title) override { SetWindowTextW(hwnd_, widen(title).c_str()); }

    void set_size(Extent2D size) override {
        RECT rect{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
        const auto style = static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE));
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForWindow(hwnd_));
        SetWindowPos(hwnd_,
                     nullptr,
                     0,
                     0,
                     rect.right - rect.left,
                     rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    [[nodiscard]] Extent2D framebuffer_size() const override {
        RECT r{};
        GetClientRect(hwnd_, &r);
        return Extent2D{static_cast<std::uint32_t>(r.right - r.left),
                        static_cast<std::uint32_t>(r.bottom - r.top)};
    }

    [[nodiscard]] Extent2D logical_size() const override {
        const Extent2D fb = framebuffer_size();
        const float scale = content_scale();
        return Extent2D{static_cast<std::uint32_t>(static_cast<float>(fb.width) / scale),
                        static_cast<std::uint32_t>(static_cast<float>(fb.height) / scale)};
    }

    [[nodiscard]] float content_scale() const override {
        return static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    }

    [[nodiscard]] bool should_close() const override { return should_close_; }

    void request_close() override { notify_close(); }

    void show() override {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }

    [[nodiscard]] NativeWindow native_handle() const override {
        // M3's Vulkan backend builds a surface from the HINSTANCE + HWND (VK_KHR_win32_surface).
        return NativeWindow{WindowSystem::Win32, GetModuleHandleW(nullptr), hwnd_};
    }

    [[nodiscard]] WindowId id() const override { return id_; }

    // The single WndProc for our window class: recover the instance from GWLP_USERDATA and turn the
    // message into engine events. Declared static (a plain C callback) but a member so it can touch
    // the instance's private state, mirroring how the Cocoa delegate calls back into CocoaWindow.
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* self = static_cast<Win32Window*>(cs->lpCreateParams);
            self->hwnd_ = hwnd; // so event handlers below can query the window during creation
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        return self->handle_message(msg, wparam, lparam);
    }

private:
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_CLOSE:
                notify_close();
                return 0; // do not let DefWindowProc destroy the window; the loop ends via the flag

            case WM_SIZE: {
                Event e{};
                e.type = EventType::WindowResize;
                e.window = id_;
                e.resize.size = framebuffer_size();
                post_event(e);
                return 0;
            }

            case WM_SETFOCUS:
            case WM_KILLFOCUS: {
                Event e{};
                e.type = EventType::WindowFocus;
                e.window = id_;
                e.focus.focused = (msg == WM_SETFOCUS);
                post_event(e);
                return 0;
            }

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                const auto sc = static_cast<WORD>((lparam >> 16) & 0xFF);
                const bool extended = ((lparam >> 24) & 1) != 0;
                Event e{};
                e.type = down ? EventType::KeyDown : EventType::KeyUp;
                e.window = id_;
                e.key.key = key_from_scancode(sc, extended);
                e.key.mods = current_mods();
                e.key.repeat = down && ((lparam >> 30) & 1) != 0; // bit 30: previous key state
                post_event(e);
                // Let DefWindowProc run too, so WM_CHAR (text) is still synthesized for WM_KEYDOWN
                // and system keys (Alt+F4 etc.) keep working.
                return DefWindowProcW(hwnd_, msg, wparam, lparam);
            }

            case WM_CHAR: {
                // wParam is one UTF-16 code unit. Emit printable characters as UTF-8 TextInput,
                // skipping control codes. (Surrogate pairs arrive as two messages; the BMP path
                // below covers the common case and the lead surrogate is simply dropped for now.)
                const auto unit = static_cast<wchar_t>(wparam);
                if (unit >= 0x20 && unit != 0x7F) {
                    Event e{};
                    e.type = EventType::TextInput;
                    e.window = id_;
                    WideCharToMultiByte(CP_UTF8,
                                        0,
                                        &unit,
                                        1,
                                        e.text.utf8,
                                        static_cast<int>(sizeof(e.text.utf8) - 1),
                                        nullptr,
                                        nullptr);
                    post_event(e);
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                const auto x = static_cast<float>(GET_X_LPARAM(lparam));
                const auto y = static_cast<float>(GET_Y_LPARAM(lparam));
                Event e{};
                e.type = EventType::MouseMove;
                e.window = id_;
                e.mouse_move.x = x;
                e.mouse_move.y = y;
                // Win32 has no relative motion in WM_MOUSEMOVE, so we difference against the last
                // absolute position (good enough for UI; raw input for FPS-camera mode is M2.x).
                e.mouse_move.dx = have_last_mouse_ ? x - last_mouse_x_ : 0.0f;
                e.mouse_move.dy = have_last_mouse_ ? y - last_mouse_y_ : 0.0f;
                last_mouse_x_ = x;
                last_mouse_y_ = y;
                have_last_mouse_ = true;
                post_event(e);
                return 0;
            }

            case WM_LBUTTONDOWN:
                post_mouse_button(MouseButton::Left, true);
                return 0;
            case WM_LBUTTONUP:
                post_mouse_button(MouseButton::Left, false);
                return 0;
            case WM_RBUTTONDOWN:
                post_mouse_button(MouseButton::Right, true);
                return 0;
            case WM_RBUTTONUP:
                post_mouse_button(MouseButton::Right, false);
                return 0;
            case WM_MBUTTONDOWN:
                post_mouse_button(MouseButton::Middle, true);
                return 0;
            case WM_MBUTTONUP:
                post_mouse_button(MouseButton::Middle, false);
                return 0;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP: {
                const MouseButton b =
                    (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? MouseButton::X1 : MouseButton::X2;
                post_mouse_button(b, msg == WM_XBUTTONDOWN);
                return TRUE; // XBUTTON messages are documented to return TRUE when handled
            }

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                                    static_cast<float>(WHEEL_DELTA);
                Event e{};
                e.type = EventType::MouseWheel;
                e.window = id_;
                e.wheel.dx = (msg == WM_MOUSEHWHEEL) ? delta : 0.0f;
                e.wheel.dy = (msg == WM_MOUSEWHEEL) ? delta : 0.0f;
                post_event(e);
                return 0;
            }

            default:
                return DefWindowProcW(hwnd_, msg, wparam, lparam);
        }
    }

    void post_mouse_button(MouseButton button, bool down) {
        Event e{};
        e.type = EventType::MouseButton;
        e.window = id_;
        e.button.button = button;
        e.button.mods = current_mods();
        e.button.down = down;
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
    HWND hwnd_ = nullptr;
    float last_mouse_x_ = 0.0f;
    float last_mouse_y_ = 0.0f;
    bool have_last_mouse_ = false;
    bool should_close_ = false;
};

} // namespace

namespace rime::platform::detail {

void native_init() {
    // Per-monitor DPI-aware v2: the process (not Windows' bitmap scaler) owns pixel-accurate sizing
    // and is told about DPI changes — the right baseline for a renderer. Set before any window.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style =
        CS_HREDRAW | CS_VREDRAW | CS_OWNDC; // OWNDC: a private device context for the swapchain
    wc.lpfnWndProc = &Win32Window::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);
}

void native_shutdown() {
    UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
}

void native_pump() {
    // Non-blocking drain: translate (for WM_CHAR) and dispatch every queued message to our WndProc,
    // which posts the corresponding engine events. PM_REMOVE pops them so we never spin.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

std::unique_ptr<Window> native_create_window(const WindowDesc& desc) {
    auto window = std::make_unique<Win32Window>(desc);
    if (!window->valid()) {
        return nullptr;
    }
    return window;
}

} // namespace rime::platform::detail
