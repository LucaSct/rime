# rime_platform — the thin OS abstraction (window, input, filesystem, timers, threads).
#
# The seam that makes M2 portable lives in *this file*: OS-specific code is in src/<platform>/
# files that are compiled exclusively on their target OS (selected below), so a backend .cpp is
# built wholesale on its platform and not at all elsewhere. That is what keeps the public headers
# (and the rest of the engine) free of OS #ifdefs — adding a platform means adding a backend dir,
# not branching a header. (See docs/ARCHITECTURE.md and docs/adr/0006-native-windowing.md.)

# OS-agnostic sources — built on every platform.
add_library(rime_platform STATIC
    src/platform.cpp
    src/clock.cpp)

# Namespaced alias so dependents link rime::platform (matching rime::core).
add_library(rime::platform ALIAS rime_platform)

# Public headers: the OS-agnostic interface, visible to dependents and to our own backends.
target_include_directories(rime_platform
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Depends only on the layer below (the layer cake). Threads::Threads arrives transitively
# (rime::core links it PUBLIC) and is what the pthread-based backends below need.
target_link_libraries(rime_platform
    PUBLIC rime::core)

# ── Per-OS backend selection ──────────────────────────────────────────────────────
# Each branch contributes only its OS's sources. As more of M2 lands (window, input, base dirs)
# each backend dir grows; the selection logic stays here, one place, and the headers stay clean.
if(APPLE)
    target_sources(rime_platform PRIVATE
        src/cocoa/threads_cocoa.cpp)
elseif(WIN32)
    target_sources(rime_platform PRIVATE
        src/win32/threads_win32.cpp)
elseif(UNIX)
    target_sources(rime_platform PRIVATE
        src/linux/threads_posix.cpp)
endif()

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_platform)
