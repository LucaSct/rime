// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core diagnostics: logging, assertions, timing/profiling, and the
// work ledger. Include this for all of them, or include the individual headers under
// diagnostics/ for finer-grained dependencies.
#include "rime/core/diagnostics/assert.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/source_location.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
