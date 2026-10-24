# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# Frozen narrowphase replay needs exact runtime hulls and private dispatch, not re-registration.
add_executable(contact_bench main.cpp)
target_include_directories(contact_bench PRIVATE ${PROJECT_SOURCE_DIR}/engine/physics/src)
target_link_libraries(contact_bench PRIVATE rime::physics fmt::fmt)
rime_apply_common_target_options(contact_bench)
