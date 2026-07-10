# rime_assets — the runtime asset boundary (M6.1, ADR-0024).
#
# This is the *load* half of the asset model: it opens cooked RMA1 files, validates them completely
# (trust nothing off disk), and hands back typed, registry-owned assets. It contains no source-format
# parser — no glTF, no PNG, no STL — by design: all importing/cooking happens offline in the Rust
# tools/asset-pipeline (M6.2), and files are the boundary between them (ADR-0001, ADR-0024).
#
# Dependencies are deliberately narrow. rime::core is PUBLIC because the public headers expose core
# types (the content hash, the generational Handle/SlotMap, Vec3). rime::platform is PRIVATE: only
# registry.cpp uses it, to read a file's bytes — the parse path itself never touches the OS, which is
# what keeps the whole reader unit-testable with in-memory buffers and no device. Notably absent:
# rime::rhi and rime::render. Assets produce CPU data; the renderer *consumes* assets and owns GPU
# residency (wired at M6.6). The dependency never points the other way.
add_library(rime_assets STATIC
    src/cooked_reader.cpp
    src/registry.cpp
    src/manifest.cpp
    src/asset_server.cpp)

# Namespaced alias so dependents link rime::assets (matching the other modules).
add_library(rime::assets ALIAS rime_assets)

target_include_directories(rime_assets
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(rime_assets
    PUBLIC rime::core
    PRIVATE rime::platform)

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_assets)
