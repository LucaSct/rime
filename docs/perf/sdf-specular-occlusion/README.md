# SDF specular occlusion: diagnostic software timing

**Measured:** this is a lavapipe diagnostic, not a hardware performance baseline. The environment
exposes no `/dev/dri` or `/dev/nvidia0`; the adapter selected by the probe is llvmpipe (LLVM 23.1.1,
256 bits). See `lavapipe-release-probe.txt:3`. The RTX 3060 cost and compliance with ADR-0078's
0.1–0.35 ms estimate could not be determined here; no hardware baseline JSON is filed.

**Measured:** Release preset (RelWithDebInfo), validation off, no sanitizer, 1920x1080, clear steady
sky, no analytic lights, roughness 0.6 metal outside a six-wall box. For each SSR setting, discard
8 warmup frames and report the median of 40 measured frames, SDF occlusion off then on. Warmups precede the measured frames. The metal covers part of the lower frame; this does
not claim a full-screen bound. Probe implementation: `tests/render/sky_specular_test.cpp:1496`.

| Reader | Off median (ms) | On median (ms) | Difference (ms) |
|---|---:|---:|---:|
| Forward, SSR off | 1.62184 | 2.91621 | +1.29437 |
| SSR resolve | 2.38659 | 3.72078 | +1.33419 |

**Measured:** the raw output and graph medians are in `lavapipe-release-probe.txt:10`. These software
increments exceed the ADR's estimate on this environment. No other build or GPU test was running
in this session during this final probe; clocks were not pinned and CPU contention from outside
this session was not measured. An earlier probe overlapped compilation and was substantially
slower; it is not the measurement recorded here.

**Inferred:** the unchanged sky/geometry and warmups exclude initial bake/compose work. Neither
software time nor its difference predicts RTX time. A hardware run is still
required before making a target-GPU budget claim; keeping the switch off is deliberate.

Reproduce the diagnostic after building the Release render tests:

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_PERF_PROBE=1 \
  ./build/release/bin/rime_render_tests -tc='SDF specular occlusion: per-pass GPU cost*'
```
