// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for brick M3.1: the RHI brings up a Vulkan device on whatever GPU is present and reports a
// sane adapter. Defining the macro below makes this TU provide doctest's main() for the
// rime_rhi_tests executable (exactly one file per exe does).
//
// "No device" is a *skip*, not a failure, so the suite still passes on a machine with no Vulkan at
// all (a dev box without a driver). CI sets RIME_REQUIRE_VULKAN to flip that: there a missing
// device is a hard failure, so a broken lavapipe/MoltenVK setup can't masquerade as green.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>

#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/rhi.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi device comes up and reports an adapter") {
    rime::rhi::DeviceDesc desc{};
    desc.app_name = "rime-rhi-test";

    auto device = rime::rhi::create_device(desc);
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping (set RIME_REQUIRE_VULKAN to enforce)");
        return;
    }

    const auto& adapter = device->adapter();
    RIME_INFO("rhi test: device on '{}' (api_version={:#x})", adapter.name, adapter.api_version);
    CHECK_FALSE(adapter.name.empty());
    CHECK(adapter.api_version != 0u); // the backend already required >= Vulkan 1.3 to get here
}
