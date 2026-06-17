// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core jobs: the lock-free work-stealing deque and the work-stealing
// job system built on it. This is how the engine uses all the cores — submit work, join with a
// counter, or fan out with parallel_for. Include this, or the individual headers under jobs/.
// Design: docs/design/work-stealing-deque.md and docs/design/job-system.md.
#include "rime/core/jobs/chase_lev_deque.hpp"
#include "rime/core/jobs/job_system.hpp"
