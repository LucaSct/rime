# Corrected live-play workload: measured comparison

The corrected sample removes the large CPU declaration penalty and skips the invisible second view at MSR hip. These are controlled serialized headless diagnostics, not live-FPS acceptance or a ratified performance baseline.

Same scene:952×514, RTX3060, explicit NVIDIA ICD; initial eye(-6,1.3,0), yaw−π/2, pitch0.04; sunset, environment,304 streamed tiles,1888/2120 parent cull.30 simulation seed ticks,30 render warmup frames,120 measured frames per case. Each active renderer updates512 DDGI probes×128rays,4m spacing. ADS scope remains512×512. No rendering features were disabled. Static fully settled hip/ADS viewmodels; no input motion. All cases exited0.

| Case | Before frame mean / p95 ms | Corrected mean / p95 ms | Before declare ms | Corrected declare ms | Corrected full GPU span ms | Timed/executed |
|---|---:|---:|---:|---:|---:|---:|
|SCAR hip|11.53 / 12.24|6.42 / 6.97|5.66|0.761|2.086|324/324|
|MSR hip|64.05 / 72.30|6.40 / 7.00|42.90|0.754|2.106|326/326|
|MSR ADS|66.03 / 71.51|10.75 / 11.40|41.56|1.147|3.758|643/643|

MSR hip before value uses the instrumented repeat64.05ms; initial untouched run was64.84ms. Corrected zero-tick rows use the separate GPU-span repeat; first corrected pass gave6.39/6.37/11.01ms, consistent with the table.

Per-view CPU boundaries: MSR hip before optical5.25ms then parent37.33ms; corrected optical0, parent0.461ms. ADS corrected optical0.429ms, parent0.445ms. Pass naming's repeated suffix searches explain the former parent's nonlinear cost; the rebuilt implementation removes it. Hip now records scope_renderer absent and only parent512probes/65,536rays. ADS still records both views, each512/65,536, preserving real magnification work.

| Corrected case | Ticks/frame | Frame mean / p95 ms | Measured simulation mean ms |
|---|---:|---:|---:|
|scar_hip_t1|1|7.22 / 7.96|0.820|
|msr_hip_t1|1|7.27 / 7.64|0.777|
|msr_ads_t1|1|11.95 / 13.63|0.790|
|scar_hip_t3|3|8.62 / 9.02|2.236|
|msr_ads_t3|3|13.31 / 14.32|2.250|

Corrected GPU timing capacity is actually1024passes (2048queries), and every executed pass is timed. Read-only first-start/last-end GPU query differences produce the table's full intervals; no query was overwritten. Full pass sums are2.085/2.105/3.755ms, closely matching spans2.086/2.106/3.758ms. Largest parent rows: SSR≈0.302ms, forwardPBR≈0.257ms hip/0.338ms ADS, DDGItrace≈0.084ms per view. Original128-pass GPU data cannot support a before/after whole-GPU comparison.

Limitations: offscreen submit_blocking serializes CPU and GPU, excluding live presentation, input, compositor pacing, and pipelined overlap. Corrected captures had a resident idle llama-server (PID781566) using VRAM: pre-capture10.84GB versus original414MB. pmon showed no inference SM use; no competing game/build/test process was active at corrected capture start, and root reserved CPU/GPU during capture. Desktop activity remained. Clock stability was not sampled across frames; spot reads ranged1837MHz before capture to210MHz during an idle gap. Do not treat these runs as clock-controlled benchmark ratification.

Artifacts: `diagnostic.json` contains all original/corrected raw-field means/medians/p95/max; *.frames.csv and *.gpu.csv retain individual frames/pass distributions. after-provenance.txt captures exact dirty-source snapshot hashes and linked archive hashes, and main-after.cpp is the preserved production snapshot. live_scene_perf_after.cpp and live_scene_perf_span.cpp add only external CPU boundaries/query reads. Original executable/source/logs remain intact. The diagnostic agent made no repository edits. These files were copied into the repository by the implementation owner.


The timing captures precede the SCAR transparent-glass repair; its final sight adds a world-HDR snapshot and a lens pass. The scope and pass-name corrections measured here are unchanged. Final real-art tests run the repaired sights as well.

The standard `scripts/perf.sh --sample the-block --commit` was also run. It refused filing: graphics clocks ranged 210–1,942 MHz (94.3% spread), and the authored destruction workload exceeded its existing 6 ms simulation p99 budget (6.492 ms). `standard-rejected.json` preserves that rejected report. It is not an accepted baseline and must not be used to claim the old performance gate passed. This authored tape is separate from the live-play diagnostic.
