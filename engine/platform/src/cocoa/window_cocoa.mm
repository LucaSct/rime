// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "platform_backend.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// Cocoa (macOS) window backend.
//
// Three things make macOS the most divergent of our targets, and each shapes the code below:
//   1. NSApplication and all window/event calls must happen on the *main* thread — so the engine
//      keeps the loop there and we pump events manually with nextEventMatchingMask:, never
//      [NSApp run] (which would take the loop away from us).
//   2. There is no Vulkan: the content view is layer-backed by a CAMetalLayer, and native_handle()
//      hands that layer upward so M3/MoltenVK can build a metal surface (VK_EXT_metal_surface).
//      The platform layer itself never touches Vulkan — only QuartzCore.
//   3. Cocoa is retained-mode and event-callback driven. We translate each NSEvent into our POD
//      Event in the pump (and window-level events in the NSWindowDelegate), then forward the event
//      with [NSApp sendEvent:] so menus, shortcuts, and window management still work.
//
// All Cocoa code lives in this one translation unit so the C++ window object, its Objective-C
// delegate, and the pump can see one another without cross-TU plumbing. Built with -fobjc-arc, so
// object lifetimes are automatic (see CMakeLists.txt).

using namespace rime::platform;

namespace {

// macOS reports a layout-independent virtual key code (kVK_*) per key. We map the well-known
// ANSI/positional codes to our physical Key enum here; unmapped keys fall through to Unknown
// (the table is completed as needed in M2.3). Hardcoding the values avoids a Carbon dependency.
Key key_from_keycode(unsigned short kc) {
    switch (kc) {
        // Letters (by physical position).
        case 0x00:
            return Key::A;
        case 0x0B:
            return Key::B;
        case 0x08:
            return Key::C;
        case 0x02:
            return Key::D;
        case 0x0E:
            return Key::E;
        case 0x03:
            return Key::F;
        case 0x05:
            return Key::G;
        case 0x04:
            return Key::H;
        case 0x22:
            return Key::I;
        case 0x26:
            return Key::J;
        case 0x28:
            return Key::K;
        case 0x25:
            return Key::L;
        case 0x2E:
            return Key::M;
        case 0x2D:
            return Key::N;
        case 0x1F:
            return Key::O;
        case 0x23:
            return Key::P;
        case 0x0C:
            return Key::Q;
        case 0x0F:
            return Key::R;
        case 0x01:
            return Key::S;
        case 0x11:
            return Key::T;
        case 0x20:
            return Key::U;
        case 0x09:
            return Key::V;
        case 0x0D:
            return Key::W;
        case 0x07:
            return Key::X;
        case 0x10:
            return Key::Y;
        case 0x06:
            return Key::Z;
        // Number row.
        case 0x1D:
            return Key::Num0;
        case 0x12:
            return Key::Num1;
        case 0x13:
            return Key::Num2;
        case 0x14:
            return Key::Num3;
        case 0x15:
            return Key::Num4;
        case 0x17:
            return Key::Num5;
        case 0x16:
            return Key::Num6;
        case 0x1A:
            return Key::Num7;
        case 0x1C:
            return Key::Num8;
        case 0x19:
            return Key::Num9;
        // Whitespace / editing.
        case 0x31:
            return Key::Space;
        case 0x24:
            return Key::Enter;
        case 0x30:
            return Key::Tab;
        case 0x33:
            return Key::Backspace;
        case 0x35:
            return Key::Escape;
        case 0x75:
            return Key::Delete;
        case 0x72:
            return Key::Insert; // "Help" on Apple keyboards, mapped to Insert
        // Navigation.
        case 0x7B:
            return Key::Left;
        case 0x7C:
            return Key::Right;
        case 0x7E:
            return Key::Up;
        case 0x7D:
            return Key::Down;
        case 0x73:
            return Key::Home;
        case 0x77:
            return Key::End;
        case 0x74:
            return Key::PageUp;
        case 0x79:
            return Key::PageDown;
        // Punctuation.
        case 0x1B:
            return Key::Minus;
        case 0x18:
            return Key::Equal;
        case 0x21:
            return Key::LeftBracket;
        case 0x1E:
            return Key::RightBracket;
        case 0x2A:
            return Key::Backslash;
        case 0x29:
            return Key::Semicolon;
        case 0x27:
            return Key::Apostrophe;
        case 0x32:
            return Key::Grave;
        case 0x2B:
            return Key::Comma;
        case 0x2F:
            return Key::Period;
        case 0x2C:
            return Key::Slash;
        // Modifiers + locks.
        case 0x38:
            return Key::LeftShift;
        case 0x3C:
            return Key::RightShift;
        case 0x3B:
            return Key::LeftCtrl;
        case 0x3E:
            return Key::RightCtrl;
        case 0x3A:
            return Key::LeftAlt;
        case 0x3D:
            return Key::RightAlt;
        case 0x37:
            return Key::LeftSuper;
        case 0x36:
            return Key::RightSuper;
        case 0x39:
            return Key::CapsLock;
        // Function keys.
        case 0x7A:
            return Key::F1;
        case 0x78:
            return Key::F2;
        case 0x63:
            return Key::F3;
        case 0x76:
            return Key::F4;
        case 0x60:
            return Key::F5;
        case 0x61:
            return Key::F6;
        case 0x62:
            return Key::F7;
        case 0x64:
            return Key::F8;
        case 0x65:
            return Key::F9;
        case 0x6D:
            return Key::F10;
        case 0x67:
            return Key::F11;
        case 0x6F:
            return Key::F12;
        // Keypad.
        case 0x52:
            return Key::KP0;
        case 0x53:
            return Key::KP1;
        case 0x54:
            return Key::KP2;
        case 0x55:
            return Key::KP3;
        case 0x56:
            return Key::KP4;
        case 0x57:
            return Key::KP5;
        case 0x58:
            return Key::KP6;
        case 0x59:
            return Key::KP7;
        case 0x5B:
            return Key::KP8;
        case 0x5C:
            return Key::KP9;
        case 0x41:
            return Key::KPDecimal;
        case 0x4B:
            return Key::KPDivide;
        case 0x43:
            return Key::KPMultiply;
        case 0x4E:
            return Key::KPSubtract;
        case 0x45:
            return Key::KPAdd;
        case 0x4C:
            return Key::KPEnter;
        case 0x51:
            return Key::KPEqual;
        default:
            return Key::Unknown;
    }
}

KeyMods mods_from_flags(NSEventModifierFlags f) {
    KeyMods m = KeyMods::None;
    if (f & NSEventModifierFlagShift) {
        m |= KeyMods::Shift;
    }
    if (f & NSEventModifierFlagControl) {
        m |= KeyMods::Ctrl;
    }
    if (f & NSEventModifierFlagOption) {
        m |= KeyMods::Alt;
    }
    if (f & NSEventModifierFlagCommand) {
        m |= KeyMods::Super;
    }
    return m;
}

} // namespace

// Forward declaration so the delegate's @interface can name the C++ window type it points back to.
@class RimeMetalView;

// Window-level events (close/resize/focus) arrive as delegate callbacks. The delegate holds a
// type-erased pointer back to its C++ CocoaWindow (a non-owning raw pointer; the CocoaWindow owns
// the delegate, and NSWindow's delegate reference is weak, so there is no retain cycle).
@interface RimeWindowDelegate : NSObject <NSWindowDelegate> {
    void* _cpp;
}
- (instancetype)initWithCppWindow:(void*)cpp;
- (void*)cppWindow;
@end

// The content view's one job is to back itself with a CAMetalLayer (so M3 can make a surface) and
// to accept first-responder status so it receives key events. Empty keyDown:/keyUp: overrides
// swallow the default "unhandled key" beep — we have already translated the key in the pump.
@interface RimeMetalView : NSView
@end

@implementation RimeMetalView

- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent*)event {
    (void)event;
}

- (void)keyUp:(NSEvent*)event {
    (void)event;
}

@end

namespace {

class CocoaWindow final : public Window {
public:
    explicit CocoaWindow(const WindowDesc& desc) : id_(detail::allocate_window_id()) {
        @autoreleasepool {
            NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable;
            if (desc.resizable) {
                style |= NSWindowStyleMaskResizable;
            }
            const NSRect rect = NSMakeRect(0, 0, desc.width, desc.height);
            window_ = [[NSWindow alloc] initWithContentRect:rect
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            window_.releasedWhenClosed = NO; // ARC owns the window; do not double-free on close
            window_.title = @(std::string(desc.title).c_str());
            window_.acceptsMouseMovedEvents = YES;
            [window_ center];

            view_ = [[RimeMetalView alloc] initWithFrame:rect];
            view_.wantsLayer = YES;
            layer_ = static_cast<CAMetalLayer*>(view_.layer);
            layer_.contentsScale = window_.backingScaleFactor;
            window_.contentView = view_;
            [window_ makeFirstResponder:view_];

            delegate_ = [[RimeWindowDelegate alloc] initWithCppWindow:this];
            window_.delegate = delegate_;
        }
    }

    ~CocoaWindow() override {
        @autoreleasepool {
            window_.delegate = nil; // sever the (weak) link before we and the delegate die
            [window_ orderOut:nil];
            [window_ close];
        }
    }

    void set_title(std::string_view title) override {
        @autoreleasepool {
            window_.title = @(std::string(title).c_str());
        }
    }

    void set_size(Extent2D size) override {
        @autoreleasepool {
            [window_ setContentSize:NSMakeSize(size.width, size.height)];
        }
    }

    [[nodiscard]] Extent2D framebuffer_size() const override {
        const NSRect backing = [view_ convertRectToBacking:view_.bounds];
        return Extent2D{static_cast<std::uint32_t>(backing.size.width),
                        static_cast<std::uint32_t>(backing.size.height)};
    }

    [[nodiscard]] Extent2D logical_size() const override {
        const NSRect b = view_.bounds;
        return Extent2D{static_cast<std::uint32_t>(b.size.width),
                        static_cast<std::uint32_t>(b.size.height)};
    }

    [[nodiscard]] float content_scale() const override {
        return static_cast<float>(window_.backingScaleFactor);
    }

    [[nodiscard]] bool should_close() const override { return should_close_; }

    void request_close() override { notify_close(); }

    void show() override {
        @autoreleasepool {
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }

    [[nodiscard]] NativeWindow native_handle() const override {
        // Hand M3 the CAMetalLayer (for VK_EXT_metal_surface). __bridge: no ownership transfer.
        return NativeWindow{WindowSystem::Cocoa, nullptr, (__bridge void*)layer_};
    }

    [[nodiscard]] WindowId id() const override { return id_; }

    // --- called from the Objective-C delegate / pump (same TU) ---
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

    void notify_resize() {
        Event e{};
        e.type = EventType::WindowResize;
        e.window = id_;
        e.resize.size = framebuffer_size();
        post_event(e);
    }

    void notify_focus(bool focused) {
        Event e{};
        e.type = EventType::WindowFocus;
        e.window = id_;
        e.focus.focused = focused;
        post_event(e);
    }

    [[nodiscard]] NSView* view() const { return view_; }

private:
    WindowId id_;
    NSWindow* window_ = nil;
    RimeMetalView* view_ = nil;
    RimeWindowDelegate* delegate_ = nil;
    CAMetalLayer* layer_ = nil;
    bool should_close_ = false;
};

// Recover the C++ window for an NSEvent's target window (used to tag events with their WindowId
// and to convert mouse coordinates into the view's space).
CocoaWindow* cocoa_window_for(NSWindow* w) {
    if (w != nil && [w.delegate isKindOfClass:[RimeWindowDelegate class]]) {
        return static_cast<CocoaWindow*>([static_cast<RimeWindowDelegate*>(w.delegate) cppWindow]);
    }
    return nullptr;
}

void post_mouse_button(NSEvent* e, MouseButton button, bool down) {
    CocoaWindow* cw = cocoa_window_for(e.window);
    Event ev{};
    ev.type = EventType::MouseButton;
    ev.window = cw ? cw->id() : WindowId{};
    ev.button.button = button;
    ev.button.mods = mods_from_flags(e.modifierFlags);
    ev.button.down = down;
    post_event(ev);
}

void post_mouse_move(NSEvent* e) {
    CocoaWindow* cw = cocoa_window_for(e.window);
    Event ev{};
    ev.type = EventType::MouseMove;
    ev.window = cw ? cw->id() : WindowId{};
    if (cw != nullptr) {
        NSView* v = cw->view();
        const NSPoint local = [v convertPoint:e.locationInWindow fromView:nil];
        const float scale = cw->content_scale();
        ev.mouse_move.x = static_cast<float>(local.x) * scale;
        // Cocoa's origin is bottom-left; flip to a top-left origin for graphics conventions.
        ev.mouse_move.y = static_cast<float>(v.bounds.size.height - local.y) * scale;
    }
    ev.mouse_move.dx = static_cast<float>(e.deltaX);
    ev.mouse_move.dy = static_cast<float>(e.deltaY);
    post_event(ev);
}

// Translate one NSEvent into our event(s). Window-level events come through the delegate instead.
void translate_event(NSEvent* e) {
    const WindowId wid = [&] {
        CocoaWindow* cw = cocoa_window_for(e.window);
        return cw ? cw->id() : WindowId{};
    }();

    switch (e.type) {
        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
            Event ev{};
            ev.type = (e.type == NSEventTypeKeyDown) ? EventType::KeyDown : EventType::KeyUp;
            ev.window = wid;
            ev.key.key = key_from_keycode(e.keyCode);
            ev.key.mods = mods_from_flags(e.modifierFlags);
            ev.key.repeat = (e.type == NSEventTypeKeyDown) ? e.isARepeat : false;
            post_event(ev);
            if (e.type == NSEventTypeKeyDown) {
                NSString* chars = e.characters;
                if (chars.length > 0) {
                    const unichar c = [chars characterAtIndex:0];
                    if (c >= 0x20 && c != 0x7F) { // skip control characters
                        Event te{};
                        te.type = EventType::TextInput;
                        te.window = wid;
                        std::strncpy(te.text.utf8, chars.UTF8String, sizeof(te.text.utf8) - 1);
                        post_event(te);
                    }
                }
            }
            break;
        }
        case NSEventTypeFlagsChanged: {
            // One event fires when a modifier's state changes; the new modifierFlags tell us
            // whether the changed key is now down or up.
            const Key k = key_from_keycode(e.keyCode);
            const NSEventModifierFlags f = e.modifierFlags;
            bool down = false;
            switch (k) {
                case Key::LeftShift:
                case Key::RightShift:
                    down = (f & NSEventModifierFlagShift) != 0;
                    break;
                case Key::LeftCtrl:
                case Key::RightCtrl:
                    down = (f & NSEventModifierFlagControl) != 0;
                    break;
                case Key::LeftAlt:
                case Key::RightAlt:
                    down = (f & NSEventModifierFlagOption) != 0;
                    break;
                case Key::LeftSuper:
                case Key::RightSuper:
                    down = (f & NSEventModifierFlagCommand) != 0;
                    break;
                case Key::CapsLock:
                    down = (f & NSEventModifierFlagCapsLock) != 0;
                    break;
                default:
                    break;
            }
            Event ev{};
            ev.type = down ? EventType::KeyDown : EventType::KeyUp;
            ev.window = wid;
            ev.key.key = k;
            ev.key.mods = mods_from_flags(f);
            ev.key.repeat = false;
            post_event(ev);
            break;
        }
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            post_mouse_move(e);
            break;
        case NSEventTypeLeftMouseDown:
            post_mouse_button(e, MouseButton::Left, true);
            break;
        case NSEventTypeLeftMouseUp:
            post_mouse_button(e, MouseButton::Left, false);
            break;
        case NSEventTypeRightMouseDown:
            post_mouse_button(e, MouseButton::Right, true);
            break;
        case NSEventTypeRightMouseUp:
            post_mouse_button(e, MouseButton::Right, false);
            break;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp: {
            // Button numbers: 2 = middle, 3 = X1, 4 = X2.
            MouseButton b = MouseButton::Middle;
            if (e.buttonNumber == 3) {
                b = MouseButton::X1;
            } else if (e.buttonNumber >= 4) {
                b = MouseButton::X2;
            }
            post_mouse_button(e, b, e.type == NSEventTypeOtherMouseDown);
            break;
        }
        case NSEventTypeScrollWheel: {
            Event ev{};
            ev.type = EventType::MouseWheel;
            ev.window = wid;
            ev.wheel.dx = static_cast<float>(e.scrollingDeltaX);
            ev.wheel.dy = static_cast<float>(e.scrollingDeltaY);
            post_event(ev);
            break;
        }
        default:
            break;
    }
}

} // namespace

@implementation RimeWindowDelegate

- (instancetype)initWithCppWindow:(void*)cpp {
    if ((self = [super init])) {
        _cpp = cpp;
    }
    return self;
}

- (void*)cppWindow {
    return _cpp;
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    static_cast<CocoaWindow*>(_cpp)->notify_close();
    return NO; // we keep the window alive; the app loop ends via should_close()
}

- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    static_cast<CocoaWindow*>(_cpp)->notify_resize();
}

- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    static_cast<CocoaWindow*>(_cpp)->notify_focus(true);
}

- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    static_cast<CocoaWindow*>(_cpp)->notify_focus(false);
}

@end

namespace rime::platform::detail {

void native_init() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // A regular app shows in the Dock and can take focus. finishLaunching (instead of run:)
        // lets us drive the loop ourselves while NSApp is properly started.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }
}

void native_shutdown() {
    // Nothing to tear down explicitly: ARC releases windows as their CocoaWindow owners die, and
    // NSApplication does not need an explicit shutdown for our manual-pump model.
}

void native_pump() {
    @autoreleasepool {
        NSEvent* e = nil;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil) {
            translate_event(e);
            [NSApp sendEvent:e]; // let Cocoa handle menus/shortcuts/window management too
        }
    }
}

std::unique_ptr<Window> native_create_window(const WindowDesc& desc) {
    return std::make_unique<CocoaWindow>(desc);
}

} // namespace rime::platform::detail
