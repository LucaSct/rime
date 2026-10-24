// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/app/launch.hpp"

namespace rime::app {
[[nodiscard]] RunReport run_network_client(const LaunchOptions& options,
                                           const GameDefinition& definition,
                                           const RunHooks& hooks);
}
