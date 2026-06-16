// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// `core` is the foundation module every other layer builds on (see the layer cake in
// docs/ARCHITECTURE.md). This first header is intentionally tiny: in Milestone 0 its
// job is to prove the build pipeline. The version info it exposes is real, though —
// tooling, crash reports, and the editor's "About" box will all read it.
namespace rime::core {

/// A semantic engine version (major.minor.patch).
struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
};

/// The engine's compile-time version. We are pre-1.0, so `major` stays 0 and the API
/// is explicitly unstable (see VISION.md, "Non-goals"). `constexpr` so it is free at
/// runtime and usable in static contexts.
inline constexpr Version kVersion{0, 0, 1};

/// The engine's display name (e.g. for window titles and logs). Never allocates.
[[nodiscard]] std::string_view engine_name() noexcept;

/// The version formatted as "major.minor.patch", e.g. "0.0.1".
[[nodiscard]] std::string version_string();

} // namespace rime::core
