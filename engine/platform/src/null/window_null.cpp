// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <memory>
#include <string>
#include <string_view>

#include "platform_backend.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// The null (headless) window backend. It owns no OS resource: it just records the requested
// size/title and tracks should_close, so the full window/event/loop API can be exercised with no
// window server — which is how the event queue and (M2.3) input state machine get deterministic
// unit tests, and how CI stays green on display-less runners. Synthetic input is delivered through
// post_event(); native_handle() reports WindowSystem::Null so nothing tries to make a surface.
namespace rime::platform {
namespace {

class NullWindow final : public Window {
public:
    explicit NullWindow(const WindowDesc& desc)
        : size_{desc.width, desc.height}, title_(desc.title), id_(detail::allocate_window_id()) {}

    void set_title(std::string_view title) override { title_.assign(title); }

    void set_size(Extent2D size) override { size_ = size; }

    [[nodiscard]] Extent2D framebuffer_size() const override { return size_; }

    [[nodiscard]] Extent2D logical_size() const override { return size_; }

    [[nodiscard]] float content_scale() const override { return 1.0f; }

    [[nodiscard]] bool should_close() const override { return should_close_; }

    void request_close() override {
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

    void show() override {}

    [[nodiscard]] NativeWindow native_handle() const override {
        return NativeWindow{WindowSystem::Null, nullptr, nullptr};
    }

    [[nodiscard]] WindowId id() const override { return id_; }

private:
    Extent2D size_;
    std::string title_;
    WindowId id_;
    bool should_close_ = false;
};

} // namespace

namespace detail {

std::unique_ptr<Window> null_create_window(const WindowDesc& desc) {
    return std::make_unique<NullWindow>(desc);
}

} // namespace detail
} // namespace rime::platform
