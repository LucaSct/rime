# rime_app — the application framework & fixed-tick frame loop (M5.7, ADR-0023) — plus rime_hello,
# the Milestone-0 launcher it grew out of.
#
# rime_app ties the engine's modules into a runnable whole: it owns the JobSystem, the ECS World +
# sim Schedule, the FixedTimestep, and — optionally — an rhi::Device + render::RenderGraph, and runs
# the loop that steps the simulation at a FIXED tick decoupled from the render frame (the multiplayer
# seam, ADR-0023). It depends on module *interfaces* only, so removing a feature module still leaves
# a buildable app (guardrail #2).

add_library(rime_app STATIC
    src/application.cpp)

# Namespaced alias so dependents link rime::app (matching the other modules).
add_library(rime::app ALIAS rime_app)

target_include_directories(rime_app
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC because the app's public headers name these modules' types: World/Schedule (ecs), JobSystem
# (core), Event (platform), and RenderGraph*/Device* in FrameContext (render, which brings rhi + core
# transitively). rime::render is always a valid target — RIME_RHI_VULKAN only gates the backend
# *inside* rhi, so an app on a backend-less build links fine and simply gets a null device.
target_link_libraries(rime_app
    PUBLIC rime::core rime::ecs rime::platform rime::render)

rime_apply_common_target_options(rime_app)

# SDK install/export (M6.8): headers + archive into the rime-targets export set. rime_hello below is
# a demo launcher, not SDK surface, so it is deliberately NOT installed.
rime_install_module(rime_app)

# ── rime_app_game — the engine-owned `main` of a shipped game (m20.1, ADR-0046 §1) ─────────────
# `GameDefinition` (what a game supplies), the launch-mode parser, and `run_game` (the one loop every
# mode shares). Its own target rather than more sources in rime_app because it needs rime::scene (a
# definition may name an entry `.rscene`), and rime_app deliberately does not: a sim-only tool that
# links rime_app should not acquire the scene format because games can have entry scenes.
#
# It links rime::render because `play` owns a SceneRenderer — and that is also why `dedicated`
# cannot be proved GPU-free AT LINK TIME in this brick: ADR-0046 §1 wants ONE binary that can do
# both, so the render path is in the executable by design. The guarantee is instead that the device
# factory is never ENTERED (`RunReport::device_factory_calls`, asserted by the tests) and that the
# Vulkan loader is only ever dlopen'd inside it (volk), so a dedicated process never maps libvulkan
# — which `hello_game --dedicated-proof` checks in a live process on Linux.
add_library(rime_app_game STATIC
    src/content_root.cpp
    src/game_definition.cpp
    src/launch.cpp)
add_library(rime::app_game ALIAS rime_app_game)

target_include_directories(rime_app_game
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(rime_app_game
    PUBLIC rime::app rime::ecs rime::platform rime::render
    PRIVATE rime::scene rime::rhi rime::core fmt::fmt)

rime_apply_common_target_options(rime_app_game)
rime_install_module(rime_app_game)

# rime_game_content(<target> <content-dir>) — m20.2 (ADR-0076-m20.2): declare a game's content.
#
# Two things, both build-tree facts the game cannot know any other way:
#   1. compile definitions RIME_GAME_CONTENT_DIR (the content in the SOURCE tree) and
#      RIME_GAME_BINARY_DIR (where the build writes the executable). The game copies them into
#      `GameDefinition::dev_content_dir` / `dev_binary_dir`, which is the content root's DEV fallback
#      — and it is only ever taken by a binary still sitting in RIME_GAME_BINARY_DIR, so these
#      absolute paths are inert in a bundle (`rime/app/content_root.hpp`);
#   2. an export record, build/<preset>/export/<config>/<target>.export, naming the executable and
#      the content directory, which `scripts/export-game.sh` reads instead of guessing where a
#      target's binary and content live;
#   3. NO build RPATH. CMake gives an executable a RUNPATH of every library directory it linked
#      from; with the Conan toolchain that was `~/.conan2/p/b/fmt…/p/lib:~/.conan2/p/b/volk…/p/lib`
#      — directories holding only static archives, so dead weight, but a path into one machine's
#      package cache baked into a binary we ship, and a place a same-named .so would be loaded from.
#      The export's dependency check found it. Skipped for the IN-TREE binary too, so the binary the
#      proof runs in-tree is byte-for-byte the one it bundles: a game that needs a non-system
#      shared library then fails here, on the developer's machine, rather than on a player's.
function(rime_game_content target content_dir)
    get_filename_component(_rime_content "${content_dir}" ABSOLUTE)
    set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH TRUE)
    target_compile_definitions(${target} PRIVATE
        RIME_GAME_CONTENT_DIR="${_rime_content}"
        RIME_GAME_BINARY_DIR="$<TARGET_FILE_DIR:${target}>")
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/export/$<CONFIG>/${target}.export"
        CONTENT "target=${target}\nexecutable=$<TARGET_FILE:${target}>\ncontent=${_rime_content}\n")
endfunction()

# ── rime_hello — the launcher (M0 seed, now the --scene loader) ───────────────────────────────────
# Proves the engine builds, links its modules, and runs — the seed rime_app grew from. Since M9.2 it
# also loads a `.rscene` via rime::scene (registering the transform + render component set a scene
# can name), so it links ecs/scene/render too. render is always a valid link target on a backend-less
# build (RIME_RHI_VULKAN only gates the backend inside rhi), so no GPU is required.
add_executable(rime_hello
    main.cpp)

target_link_libraries(rime_hello
    PRIVATE rime::core rime::ecs rime::scene rime::render fmt::fmt)

# ── rime-engine — the editor's live host process (M9.3) ───────────────────────────────────────────
# `rime-engine --editor-host <socket> [--viewport]` serves the editor channel (schema + snapshot +
# edits) against a live World over the s1.4 local socket — the process the Rust editor (tools/editor)
# launches and connects to (ADR-0016). The channel half is GPU-free; `--viewport` additionally renders
# the scene through the render graph (rime::app + rime::render + rime::rhi), captures each frame
# (rime::stream), LZ4-compresses it, and streams it. On a GPU-less host it degrades to channel-only.
# rime::physics is the m9.7 Play feature's side-table (a PhysicsWorld/PhysicsSync the viewport host
# owns for the duration of a play session) — deliberately linked HERE, not into the rime_app library,
# so rime::app itself stays physics-agnostic (guardrail #2: a sim-only Application still builds with
# physics removed). Not SDK surface — a runnable tool, deliberately not installed.
# The editor host as a LIBRARY (m15.2): the serving logic, parameterised by which component set it
# registers. It links neither worldkit nor blockkit — a host supplies its component set through a
# callback, which is what lets a game build its own binary without the engine knowing the game
# exists. See include/rime/app/editor_host_app.hpp.
add_library(rime_editorhost_app STATIC
    editor_host_app.cpp
    stream_input.cpp)
add_library(rime::editorhost_app ALIAS rime_editorhost_app)

target_include_directories(rime_editorhost_app
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# rime::stream is PUBLIC as of m18: the public header names `stream::Codec` (kEditorHostCodecs, the
# list the viewport offers a client), so every consumer of this library needs stream's headers. A
# PRIVATE link would compile this target and break each of its users.
# rime::platform joins the PUBLIC set with m18's input path: `rime/app/stream_input.hpp` names
# platform::Event / Key / MouseButton in its public interface. It already arrived transitively
# (rime::stream links it PUBLIC), but a public header naming a module's types and a PRIVATE link
# is exactly the pairing that breaks the day the transitive edge is tightened.
target_link_libraries(rime_editorhost_app
    PUBLIC rime::ecs rime::stream rime::platform
    PRIVATE rime::editorhost rime::scene rime::assets rime::app rime::physics rime::ground
            rime::render
            rime::rhi rime::core fmt::fmt)

rime_apply_common_target_options(rime_editorhost_app)
rime_install_module(rime_editorhost_app)

# The ENGINE's host: the engine's component profile and nothing else.
add_executable(rime_engine
    rime_engine_main.cpp)

set_target_properties(rime_engine PROPERTIES OUTPUT_NAME rime-engine)

# NOT rime::blockkit any more (m15.2). One game's content module was compiled into the engine's
# editor host, which is the thing that made "a second game without forking the engine" false.
target_link_libraries(rime_engine
    PRIVATE rime::editorhost_app rime::worldkit rime::ecs)

rime_apply_common_target_options(rime_engine)

rime_apply_common_target_options(rime_hello)

# ── rime-blockgen — the vision demo's procedural assembly (m13.2c) ────────────────────────────────
# `rime-blockgen --out block.rscene` writes the whole block — 8 buildings, 140 destructible slabs and
# crates, the street furniture and the dusk lighting rig — as a `.rscene`. ADR-0035 called this "a
# procedural assembly script"; it is C++ rather than Rust tooling because the format keys components
# by C++ reflection type_hash (see engine/blockkit/block.hpp). GPU-free: it fills a World and
# serializes it, and never opens a device. A runnable tool, so deliberately not installed.
add_executable(rime_blockgen
    block_gen_main.cpp)

set_target_properties(rime_blockgen PROPERTIES OUTPUT_NAME rime-blockgen)

target_link_libraries(rime_blockgen
    PRIVATE rime::blockkit rime::scene rime::ecs rime::core)

rime_apply_common_target_options(rime_blockgen)
