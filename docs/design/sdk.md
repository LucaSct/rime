# The Rime SDK — CMake install / export (design note)

This note documents how Rime installs as a consumable **SDK** (Milestone 6.8): `cmake --install`
lays down public headers, the static libraries, and a CMake package config, so an out-of-tree
project does

```cmake
find_package(rime CONFIG REQUIRED)
add_executable(game main.cpp)
target_link_libraries(game PRIVATE rime::app rime::assets)   # same rime:: names as an in-tree build
```

and builds. It is the concrete form of the "Rime's SDK/package story lands here" commitment, and it
**arms the ICEM-migration trigger** — [ADR-0016](../adr/0016-shippable-engine-features.md) rule 5:
once the engine is installable, the in-repo editor/tools consume it the same way any downstream
project would. The runnable proof of everything below is [`scripts/sdk-smoke.sh`](../../scripts/sdk-smoke.sh)
driving [`tests/sdk_consumer/`](../../tests/sdk_consumer/), wired into CI.

## What installs

| Category | Installed to | Notes |
|----------|--------------|-------|
| Public headers | `<prefix>/include/rime/<module>/` | each module's `include/` tree, verbatim |
| Static libraries | `<prefix>/lib/librime_*.a` | one per module, plus the internal support libs |
| Package config | `<prefix>/lib/cmake/rime/` | `rime-config.cmake`, `rime-config-version.cmake`, `rime-targets*.cmake` |

The exported targets carry the **`rime::`** namespace with the module short-name
(`rime::core`, `rime::platform`, `rime::rhi`, `rime::ecs`, `rime::assets`, `rime::render`,
`rime::stream`, `rime::app`) — deliberately identical to the in-tree `add_library(rime::core ALIAS
…)` aliases (via each target's `EXPORT_NAME`), so code moves between an in-tree build and an
installed one without edits.

Two internal helper archives — `rime_vma` (the VMA implementation TU) and `rime_xdg_shell` (the
generated Wayland protocol glue) — are exported too. They are linked **PRIVATE** into their module,
but a *static* PRIVATE dependency still contributes symbols the final consumer executable must
resolve at link time, so the archive has to travel with the SDK or the consumer link fails with
undefined references. They carry no public headers.

## What does NOT install (the seams that must survive)

- **Backend internals.** `engine/rhi/src/vulkan/*` and the RHI's `PRIVATE` `src/` include dir never
  install: the "no Vulkan header escapes the backend" seam ([ADR-0002](../adr/0002-rhi-vulkan.md))
  holds across installation. Same for `engine/platform`'s private `platform_backend.hpp`.
- **Embedded shaders.** The build-time SPIR-V headers (`*.spv.h`, [ADR-0008](../adr/0008-offline-shaders.md))
  are `PRIVATE` `target_sources`; consumers see pass objects, never shader bytes.
- **Samples and tests.** Nothing under `samples/` or `tests/` is installed; `rime_hello` (the M0
  demo launcher) is not SDK surface either.

`sdk-smoke.sh` enforces the first two mechanically: it greps the installed `include/` tree for any
`*vulkan*` / `*.spv.h` / `platform_backend*` / `xdg-shell*` header and fails if one leaked.

## The dependency stance (v1)

Rime ships **static** libraries, so every third-party library the engine linked against becomes a
link-time requirement of the final consumer — even the ones Rime linked `PRIVATE`. `rime-config.cmake`
therefore `find_dependency()`-es each one (`fmt`, `Threads`, `libjpeg-turbo`, `lz4`, and — when the
Vulkan backend is built — `VulkanHeaders`/`volk`/`VulkanMemoryAllocator`; on Linux also `X11` and the
pkg-config `wayland-client`/`xkbcommon`) **before** pulling in `rime-targets.cmake`, so the
imported-target names that file references already exist.

For those `find_dependency` calls to succeed, **the consumer configures with the same Conan toolchain
Rime was built with** — Rime does not vendor or re-package its dependencies in v1. In practice:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=<rime-build>/conan_toolchain.cmake \
  -Drime_DIR=<prefix>/lib/cmake/rime
```

This is a deliberate v1 simplification, not the end state. When a dependency is missing the failure is
legible — `find_package(rime)` stops at the offending `find_dependency` with a "Could not find a
package configuration file provided by <dep>" pointing at the exact `rime-config.cmake` line — which
`sdk-smoke.sh`'s deliberate-breakage check confirms once, by hand.

## Out of scope for v1 (seams noted, not built)

- **Shared/DLL builds.** Static only for now; a shared build changes symbol-visibility and the
  dependency-propagation story, so it is its own brick.
- **CPack / release archives**, and a **Conan package of Rime itself** (which would subsume the
  toolchain expectation above) — later.
- **Semver guarantees.** The version file answers `find_package(rime x.y)` with `SameMajorVersion`,
  but pre-1.0 the API is explicitly unstable (a [VISION](../../VISION.md) non-goal). The compatibility
  promise firms up at 1.0.

## Files

- Top-level [`CMakeLists.txt`](../../CMakeLists.txt) — `rime_install_module()` /
  `rime_install_support_lib()` funnels, the `rime-targets` export, and the config/version generation.
- [`cmake/rime-config.cmake.in`](../../cmake/rime-config.cmake.in) — the package config template.
- [`tests/sdk_consumer/`](../../tests/sdk_consumer/) — the out-of-tree consumer (its own CMake project).
- [`scripts/sdk-smoke.sh`](../../scripts/sdk-smoke.sh) — install → configure → build → run + the
  hygiene guards.
