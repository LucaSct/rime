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
    src/audio.cpp
    src/bank.cpp
    src/mixer.cpp
    src/wav.cpp)

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

# ── The platform sink (m13.4) ─────────────────────────────────────────────────────────────────────
# ALSA when its development headers are present, a "no device" stub otherwise. QUIET and optional on
# purpose: ADR-0035 asked for "a Linux sink first … CI is deaf either way", so a missing libasound
# must not fail a build. This is why no new entry appears in conanfile.py — the sink is a capability
# the host may or may not have, not a dependency the engine requires.
#
# Selecting at BUILD time rather than dlopen-ing at runtime is deliberate: it keeps ALSA's enum
# constants coming from ALSA's own headers instead of being hardcoded to talk to a library we never
# included, which is a silent ABI assumption waiting to rot.
find_package(ALSA QUIET)
if(ALSA_FOUND)
    target_sources(rime_audio PRIVATE src/sink_alsa.cpp)
    target_link_libraries(rime_audio PRIVATE ALSA::ALSA)
    message(STATUS "rime_audio: ALSA sink enabled")
else()
    target_sources(rime_audio PRIVATE src/sink_null.cpp)
    message(STATUS "rime_audio: no ALSA headers — audio sink will report no device")
endif()
