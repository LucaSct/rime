<!-- SPDX-License-Identifier: Apache-2.0 -->
# Collapse Scaling Observation on macOS Apple Silicon (M1 Pro) — 2026-11-09

## Environment
- **Platform:** macOS 27.3 (Darwin 27.3.0, arm64)
- **Hardware:** Apple M1 Pro (integrated GPU, 16 GB unified memory)
- **Toolchain:** Apple Clang 21.0.0, CMake 4.4.5, Ninja 1.13.2, Rust 1.99.1
- **RHI / Graphics:** Vulkan 1.3.357 via MoltenVK 1.4.2 over Metal 3 argument buffers
- **Configuration:** `release` preset (RelWithDebInfo, `-O2 -g -DNDEBUG`)

## Observation
1. **Interactive Demo & Editor Execution:**
   - Both standalone game runtimes (`the_block`, `hello_game`, `thermal-courtyard`) and editor workflows (`./scripts/city-editor.sh`, `./scripts/city-v2.sh`) execute successfully on Apple Silicon hardware with active GPU rendering via MoltenVK.
   - Initial static and moderate scenes render smoothly with full PBR shading, DDGI probes, and shadow mapping.

2. **Heavy Collapse Latency:**
   - When large-scale structural fracture cascades occur (e.g., detonating multiple buildings in `the_block` or `city-v2` generating hundreds of concurrent active tumbling debris parts), interactive frame rate exhibits significant stutter and latency.
   - This directly mirrors the profile established on Linux / RTX 3060 (`docs/design/collapse-scaling.md` and `docs/perf/2026-10-17-collapse-phase-profile.md`), where contact generation accounts for ~66–69% of physics step time during heavy cascades.
   - The observation confirms that the collapse bottleneck is platform-independent and inherent to the broadphase/narrowphase contact generation and constraint solver scaling during dense fracture cascades rather than driver- or OS-specific overhead.

## Associated Build Fix
- Under Release builds (`-DNDEBUG`), `RIME_ASSERT` in `engine/core/src/jobs/job_system.cpp` compiled out, leaving `kMaxJobSegments` unused and failing under `-Werror -Wunused-const-variable`. Marked `[[maybe_unused]]`.
