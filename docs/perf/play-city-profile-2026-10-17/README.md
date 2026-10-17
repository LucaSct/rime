<!-- SPDX-License-Identifier: Apache-2.0 -->
# External city pass profile

This generator copies and instruments `main.cpp`, `render_graph.cpp`, and `passes.cpp` into an output directory. It never edits production files or starts the executable. `--compile` compiles the three generated translation units with their actual optimized `compile_commands.json` entries and links the wrapper plus the two replacement render objects before `librime_render.a`. All other production objects remain linked. A source/build hash mismatch fails generation.

```sh
python3 docs/perf/play-city-profile-2026-10-17/prepare.py \
  --root "$PWD" --build-dir /home/next/projects/rime/build/release \
  --out-dir /tmp/rime-city-ads --ads --compile
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/run1 --ads --frames 120
```

Compile once and select each run at launch with `--out-dir`, `--ads` or `--hip`, and `--control` or `--city`. Use a separate output directory for each run. `--control` disables the generated demo's city assignment while keeping support enabled. Generator options set executable defaults; runtime options override them. The default is 60 warmup and 120 measured frames; runtime `--warmup N` and `--frames N` override them. The copied play path retains the MSR, sunset, terrain, lighting, viewmodel controls, and the optical view. Headless `app.step(1./60)` blocks on the GPU each frame and uses fixed yaw/pitch and no movement or firing.

The executable writes `frames.csv`, `cpu-passes.csv`, `gpu-passes.csv`, `graph-stages.csv`, and `summary.json` only after the run. Generation writes `provenance.json`, including source, generated source, header, linked-input and binary SHA-256 values (binary after `--compile`), plus the compile/link command arrays; a runtime output directory receives a copy. The summary records the actual run options. CPU pass milliseconds cover the entire graph pass loop, including barriers, rendering setup, body, timestamps and label ending. Mesh candidate and indexed draw counts cover only calls through `record_draws`; terrain, fullscreen work, and other draw paths are outside those counts. Graph stage CPU times separately cover compile and physical assignment. GPU milliseconds come only from `Application::on_frame_timings` after a completed blocking submission. Repeated terrain passes retain `fold_repeats`, so GPU terrain rows aggregate while CPU rows remain per executed pass. `frames.csv` reports actual executed passes, timed slots, and overflow when executed passes exceed `RenderGraph::max_timed_passes()`. Missing GPU timestamps, a missing measured frame, pass-count mismatch, or overflow fail the run. `perf_gate_pass` is null: this diagnostic is not a performance gate.

Headless blocking submission changes pacing. These measurements attribute work to passes; they are **never a native FPS or performance baseline**. Compare only runs with matching provenance and identical options.
