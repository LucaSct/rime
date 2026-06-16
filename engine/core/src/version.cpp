// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/version.hpp"

#include <fmt/format.h>

namespace rime::core {

std::string_view engine_name() noexcept {
    // "Rime" — the feathery frost that forms from freezing fog. See VISION.md for the
    // story behind the name.
    return "Rime";
}

std::string version_string() {
    // fmt is our first third-party dependency, pulled in by Conan. Formatting the
    // version through it is a deliberate end-to-end smoke test of the dependency
    // pipeline; fmt also becomes the basis of the engine's logging in Milestone 1.
    return fmt::format("{}.{}.{}", kVersion.major, kVersion.minor, kVersion.patch);
}

} // namespace rime::core
