# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# Fixed-workload CPU physics solver benchmark; no renderer or job system.
add_executable(solver_bench main.cpp)
target_link_libraries(solver_bench PRIVATE rime::physics fmt::fmt)
rime_apply_common_target_options(solver_bench)
