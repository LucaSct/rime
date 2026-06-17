// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core modules: the IModule interface and the ModuleManager that loads
// modules at runtime, resolving dependencies. This is the seam that makes the engine extensible
// and lets feature modules be added or removed without the core depending on their concrete types.
// Include this, or the individual headers under modules/. Design: docs/design/module-system.md.
#include "rime/core/modules/module.hpp"
#include "rime/core/modules/module_manager.hpp"
