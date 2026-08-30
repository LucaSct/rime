# rime_blockkit — the vision demo's block, as content (m13.2c, ADR-0035 §1).
#
# The building prefab, the procedural assembly, and the palette that decides what the whole thing
# looks like. It turns parameters into an ECS world: placements, roles, `Destructible` intents,
# lights and a camera — which `rime::scene` then writes as a `.rscene`.
#
# WHY THIS IS AN ENGINE MODULE AND NOT RUST TOOLING. ADR-0035 asked for "a procedural assembly
# script"; the Rust-owns-tooling rule would put it under tools/. It cannot go there: `.rscene` keys
# every component by its C++ reflection type_hash, and a Rust emitter would have to reproduce those
# across a language boundary — a drifted hash is a clean load error on this side and an invisible
# mis-author on that one. See block.hpp.
#
# Layer-cake position: a CONTENT module. It depends on ecs (the world it fills), destruction and
# render (the components it writes — interfaces only, never internals) and core (math). Nothing
# depends on it: delete the directory and the engine still builds (guardrail 2). It deliberately
# does NOT depend on physics or rhi — assembly is pure data, which is what lets m13.2c's proof run
# GPU-free on every CI OS and under the sanitizers.

add_library(rime_blockkit STATIC
    src/block.cpp
    src/palette.cpp)

# Namespaced alias so dependents link `rime::blockkit`, matching our C++ namespace.
add_library(rime::blockkit ALIAS rime_blockkit)

target_include_directories(rime_blockkit
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# All PUBLIC because the headers name these modules' types: World/LocalTransform (ecs),
# MaterialRegistry/MeshRegistry/MaterialId (render), Vec3/Transform (core, also transitively). The
# `Destructible` component blockkit writes comes from rime::destruction. rime::render is always a
# valid link target — RIME_RHI_VULKAN gates the backend *inside* rhi, not the module.
target_link_libraries(rime_blockkit
    PUBLIC rime::core rime::ecs rime::render rime::destruction)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_blockkit)

# DELIBERATELY NOT INSTALLED. Every engine module calls rime_install_module() to put its headers and
# archive into the SDK export set; this one does not, for the same reason `rime_hello` does not — it
# is the vision demo's CONTENT, not engine surface a downstream project should be able to link. The
# SDK shipping one particular game's building prefab would be a promise we do not want to keep.
#
# If a second content module ever appears, that is the moment to carve out a content tier with an
# ADR rather than let demo content accrete inside engine/.
