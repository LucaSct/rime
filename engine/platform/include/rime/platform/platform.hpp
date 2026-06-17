// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::platform — the thin OS abstraction (window, input, filesystem,
// timers, threads). Include this for the whole module, or a leaf header for just what you need.
// As of M2.2 the module provides lifetime (init/shutdown + the main-thread contract), a monotonic
// clock, thread naming, and a native window + event pump behind the Window seam; polled input
// lands in M2.3 and filesystem/time in M2.4. See docs/ROADMAP.md and ADR-0006 (native windowing).
#include "rime/platform/clock.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/init.hpp"
#include "rime/platform/input.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"
#include "rime/platform/native_window.hpp"
#include "rime/platform/threads.hpp"
#include "rime/platform/window.hpp"
