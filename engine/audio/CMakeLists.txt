# rime_audio — the audio seam (M8.4).
#
# The smallest audio module that gives the engine a stable boundary to call — play(sound, position,
# gain) — while the real mixer is still unwritten. Ships one interface (AudioBackend) and a null
# backend that logs calls instead of making sound, so audio is testable and demoable headless. The
# real spatializing backend is a later track (au1) that swaps NullAudioBackend without touching a
# call site.
#
# Layer-cake position: depends on core only (it needs core::Vec3 for positions), and NOTHING depends
# on it — a removable feature module (guardrail 2): the engine still builds with it gone.

add_library(rime_audio STATIC
    src/audio.cpp)

# Namespaced alias so dependents link `rime::audio`, matching our C++ namespace.
add_library(rime::audio ALIAS rime_audio)

target_include_directories(rime_audio
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC because the seam header (audio.hpp) exposes core::Vec3 in play()'s signature.
target_link_libraries(rime_audio
    PUBLIC rime::core)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_audio)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_audio)
