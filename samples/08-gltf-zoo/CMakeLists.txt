# 08-gltf-zoo — Milestone 6's "done when" (M6.10): import → cook → load → render real glTF models
# with textures, through the whole offline→runtime asset pipeline. A turntable gallery of the
# hand-authored models in assets/ (textured cube, normal-mapped MR sphere, rigged figure in a sampled
# static pose). --headless self-checks off-screen (CI/lavapipe); --serve streams it live over Track S0
# to the 04-remote-view client (the dev-server → Mac path). Links rime::app (fixed-tick loop + render
# + ecs, transitively) and rime::stream; rime::assets + rime::platform are named explicitly (the
# sample reads cooked RMA1 + the manifest itself).
add_executable(gltf_zoo main.cpp)

target_link_libraries(gltf_zoo
    PRIVATE rime::app rime::stream rime::assets rime::platform)

rime_apply_common_target_options(gltf_zoo)

# The cooked-assets directory the CTest fixture fills and the sample defaults to (overridable with
# --cooked). It lives in the build tree, one per preset, so dev/asan/tsan never share cooked bytes.
set(ZOO_COOKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(gltf_zoo PRIVATE RIME_ZOO_COOKED_DIR="${ZOO_COOKED_DIR}")

# The cook fixture: run `rime cook assets/ --out <cooked>` before the headless self-check, exactly as
# a user would (the recommended cook-at-test-fixture path — configure stays fast, and CI exercises the
# real CLI). It needs the rime CLI, which cargo builds first (build.sh / the CI workflow build both the
# C++ and the Rust before ctest runs); `cargo run` is a no-op rebuild when it is already built. The
# whole assets DIRECTORY is cooked in one invocation so a single manifest.txt indexes every model.
add_test(NAME gltf_zoo_cook
    COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
            cook samples/08-gltf-zoo/assets --out "${ZOO_COOKED_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(gltf_zoo_cook PROPERTIES FIXTURES_SETUP zoo_cook)

# M6's "done when," in CI: load the cooked models and self-check the shaded image (base-color texture
# and normal map each demonstrably reach the shader; a rigged model is CPU-posed). Green on lavapipe;
# a skip where no device exists unless RIME_REQUIRE_VULKAN is set. A small frame count keeps it quick
# under the sanitizers. FIXTURES_REQUIRED makes CTest run (and gate on) the cook first.
add_test(NAME gltf_zoo_headless
    COMMAND gltf_zoo --headless --frames 4 --cooked "${ZOO_COOKED_DIR}")
set_tests_properties(gltf_zoo_headless PROPERTIES FIXTURES_REQUIRED zoo_cook)
