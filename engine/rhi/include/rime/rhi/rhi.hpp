// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for the Render Hardware Interface. Include this to get the whole RHI surface;
// include the individual headers when you only need part of it. See docs/design/rhi.md for the
// design (the seam, the handle model, dynamic rendering) and docs/adr/0002 for why Vulkan sits
// behind this interface at all.
//
// The one rule that makes this a *seam* and not just a header: nothing under include/rime/rhi/ may
// include a Vulkan (or any backend) header. The Vulkan backend is confined to engine/rhi/src/vulkan
// and linked PRIVATE, so no consumer of rime::rhi can even transitively see <vulkan.h> (ADR-0002).

#include "rime/rhi/command_buffer.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/resources.hpp"
#include "rime/rhi/types.hpp"
