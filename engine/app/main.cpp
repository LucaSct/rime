// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Milestone-0 launcher stub. Today it only proves the toolchain works: it links the
// `core` module and prints the engine name and version. Over the coming milestones this
// grows into the real application entry point (window creation, the main loop, module
// loading, ...). See docs/ARCHITECTURE.md, the `app` module.

#include <fmt/core.h>

#include "rime/core/version.hpp"

int main() {
    fmt::print("{} engine v{}\n", rime::core::engine_name(), rime::core::version_string());
    fmt::print("Hello from the frost. (Milestone 0: the build lives.)\n");
    return 0;
}
