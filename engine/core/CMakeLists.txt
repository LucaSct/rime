# rime_core — the foundation module.
#
# Today it carries only engine version info, but every other module will sit on top of
# it (see the layer cake in docs/ARCHITECTURE.md). Keeping it real (a proper library
# target with public headers) from day one means the rest of the engine has something
# correct to depend on.

# fmt (and other third-party deps) are found once at the top level (see /CMakeLists.txt),
# which makes the fmt::fmt imported target available here. We just link what we use.
add_library(rime_core STATIC
    src/version.cpp
    src/diagnostics/log.cpp
    src/diagnostics/assert.cpp
    src/diagnostics/profile.cpp)

# Namespaced alias so dependents link `rime::core`, matching our C++ namespace and
# signalling "this is a target, not a raw library name."
add_library(rime::core ALIAS rime_core)

# Public headers under include/ are exposed to anything that links rime_core, so they
# can `#include <rime/core/...>`. Private sources see them too.
target_include_directories(rime_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# fmt is PUBLIC because our public headers use it: the logging macros expand to fmt::format
# at the call site, and version_string formats with it. Keeping the dependency public is
# therefore required, not just convenient.
target_link_libraries(rime_core
    PUBLIC fmt::fmt)

# Assertions (RIME_ASSERT / RIME_ASSERT_MSG / RIME_VERIFY) are active in Debug-style builds
# and compiled out otherwise; RIME_PANIC is always on. PUBLIC so everyone linking rime::core
# (other modules, the tests) gets the same assert behavior as the library itself.
target_compile_definitions(rime_core
    PUBLIC $<$<CONFIG:Debug>:RIME_ENABLE_ASSERTS=1>)

# Project-wide build policy (C++20 + warning set); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_core)
