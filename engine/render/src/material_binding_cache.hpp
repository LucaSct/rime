// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "rime/rhi/types.hpp"

namespace rime::render::detail {

// Pending texture attachments persist between draws (ADR-0020). A geometry partition can avoid
// resolving the same texture/sampler repeatedly, without sorting or changing a draw. This cache
// lives ONLY inside one record_draws invocation: pipeline switches, other passes and callbacks
// are outside that interval and therefore always start with five unknown bindings. Compare whole
// generational handles, not just slots; changing a sampler also changes every affected binding.
class MaterialBindingCache {
public:
    template <class Bind>
    void
    bind(std::size_t slot, rhi::TextureHandle texture, rhi::SamplerHandle sampler, Bind&& issue) {
        assert(slot < slots_.size());
        Slot& cached = slots_[slot];
        if (!cached.known || cached.texture != texture || cached.sampler != sampler) {
            issue(static_cast<std::uint32_t>(slot + 2), texture, sampler);
            cached = {texture, sampler, true};
        }
    }

private:
    struct Slot {
        rhi::TextureHandle texture;
        rhi::SamplerHandle sampler;
        bool known = false;
    };

    std::array<Slot, 5> slots_{};
};

} // namespace rime::render::detail
