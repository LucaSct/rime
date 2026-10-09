# SDF specular occlusion: what the cone costs

**Measured, RTX 3060.** Release preset, validation off, no sanitizer, 1920x1080, clear steady sky,
no analytic lights, a roughness-0.6 metal plane outside a six-wall box. Eight warmup frames
discarded, median of the next 40; three independent runs of the probe. Raw output:
`rtx3060-release-probe.txt` (one of the three; the other two agreed to within 0.008 ms).

| Reader | Off median (ms) | On median (ms) | Added (ms) | Graph off → on (ms) |
|---|---:|---:|---:|---:|
| Forward (SSR off) | 0.0512 | 0.1597 | **+0.109** | 0.160 → 0.267 |
| SSR resolve (SSR on) | 0.1976 | 0.2847 | **+0.087** | 0.344 → 0.431 |

ADR-0078 section 2 estimated 0.1–0.35 ms for this technique. **The measurement lands at the
bottom of that range**, so the estimate holds — and the switch nevertheless stays OFF, because
cost was never the only open question (see the ADR's limits: 16 samples and an 8 m reach cannot
guarantee distant or thin blockers, and curved surfaces are unproven).

**What this number is not.** The metal covers part of the lower frame, so this is a *scene*
measurement, not a full-screen bound: every pixel that reads sky specular pays sixteen clipmap
samples, and a frame that is all rough metal would pay proportionally more. Clocks were not
pinned and CPU contention from outside the session was not measured; the three-run agreement is
the only stability evidence here. The unchanged sky and geometry plus the warmups are what keep
the bake and compose work out of the figure (*inferred* — it follows from the frame loop, and is
not separately measured).

## The software diagnostic, and why it is kept

`lavapipe-release-probe.txt` holds the same probe on llvmpipe (LLVM 23.1.1, 256 bits), where the
same change costs **+1.294 ms** (forward) and **+1.334 ms** (SSR resolve) — an order of magnitude
more. It is filed because it was measured first, in an environment that exposed no `/dev/dri` or
`/dev/nvidia0`, and reading it as this brick's cost would have put the technique a factor of ten
over its budget. A software rasterizer's per-pixel loop cost does not predict a GPU's; a figure
measured on lavapipe is a correctness diagnostic, not a performance one.

(Both raw logs have had this checkout's absolute path prefix stripped; nothing else is edited.)

## Reproduce

```bash
scripts/build.sh --preset release --cpp-only --no-tests
RIME_REQUIRE_VULKAN=1 RIME_PERF_PROBE=1 \
  ./build/release/bin/rime_render_tests -tc='SDF specular occlusion: per-pass GPU cost*'
```

Without `RIME_PERF_PROBE` the probe reports that it is skipping and the case passes with zero
assertions — it is a measurement, not a gate. Add
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json` to reproduce the software row instead.
Probe implementation: `tests/render/sky_specular_test.cpp:1496`.
