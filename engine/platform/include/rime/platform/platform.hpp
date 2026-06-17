// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::platform — the thin OS abstraction (window, input, filesystem,
// timers, threads). Include this for the whole module, or a leaf header for just what you need.
// As of M2.1 the module provides lifetime (init/shutdown + the main-thread contract), a
// monotonic clock, and thread naming; window/input land in M2.2–M2.3 and filesystem/time in
// M2.4. See docs/ROADMAP.md and ADR-0006 (native windowing).
#include "rime/platform/clock.hpp"
#include "rime/platform/init.hpp"
#include "rime/platform/threads.hpp"
