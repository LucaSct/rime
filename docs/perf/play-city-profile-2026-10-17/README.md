<!-- SPDX-License-Identifier: Apache-2.0 -->
# External city pass profile

This generator copies and instruments `main.cpp`, `render_graph.cpp`, and `passes.cpp` into an output directory. It never edits production files or starts the executable. `--compile` compiles the three generated translation units with their actual optimized `compile_commands.json` entries and links the wrapper plus the two replacement render objects before `librime_render.a`. All other production objects remain linked. A source/build hash mismatch fails generation.

```sh
python3 docs/perf/play-city-profile-2026-10-17/prepare.py \
  --root "$PWD" --build-dir /home/next/projects/rime/build/release \
  --out-dir /tmp/rime-city-ads --ads --compile
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/run1 --ads --frames 120
```

Compile once and select each run at launch with `--out-dir`, `--ads` or `--hip`, and `--control` or `--city`. Use a separate output directory for each run. `--control` disables the generated demo's city assignment while keeping support enabled. Generator options set executable defaults; runtime options override them. The default is 60 warmup and 120 measured frames; runtime `--warmup N` and `--frames N` override them. The copied play path retains the MSR, sunset, terrain, lighting, viewmodel controls, and the optical view. Headless `app.step(1./60)` blocks on the GPU each frame and uses fixed yaw/pitch and no movement or ordinary trigger input.

The executable writes `frames.csv`, `cpu-passes.csv`, `gpu-passes.csv`, `graph-stages.csv`, and `summary.json` only after the run. Generation writes `provenance.json`, including source, generated source, header, linked-input and binary SHA-256 values (binary after `--compile`), plus the compile/link command arrays; a runtime output directory receives a copy. The summary records the actual run options. CPU pass milliseconds cover the entire graph pass loop, including barriers, rendering setup, body, timestamps and label ending. Mesh candidate and indexed draw counts cover only calls through `record_draws`; terrain, fullscreen work, and other draw paths are outside those counts. Graph stage CPU times separately cover compile and physical assignment. GPU milliseconds come only from `Application::on_frame_timings` after a completed blocking submission. Repeated terrain passes retain `fold_repeats`, so GPU terrain rows aggregate while CPU rows remain per executed pass. `frames.csv` reports actual executed passes, timed slots, and overflow when executed passes exceed `RenderGraph::max_timed_passes()`. Missing GPU timestamps, a missing measured frame, pass-count mismatch, or overflow fail the run. `perf_gate_pass` is null: this diagnostic is not a performance gate.

Headless blocking submission changes pacing. These measurements attribute work to passes; they are **never a native FPS or performance baseline**. Compare only runs with matching provenance and identical options.

## Collapse diagnostic

Compile the generated profile once as above, then run a bounded collapse capture in a fresh output directory:

```sh
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/collapse1 \
  --city --hip --collapse --warmup 60 --frames 180
```

`--collapse` is a runtime option. It calls the normal `Demo::fire_charge()` once, immediately before the first measured `app.step`, and requires 120–360 measured frames and at most 600 warmup frames. `--warmup 0` is supported as a control; the sample's own startup handshake and scope warmup still happen before frame zero. Negative counts and an absent charge effect fail validation. Run a separate capture without `--collapse` for an idle control using the same warmup, frame count, and other options.

`frames.csv` adds wall time for the whole `app.step` (including the charge call on its frame), the fixed PreSim callback (including weapon selection, cooldown, busy gate, and `Demo::step_play`), Demo work after `Session::tick`, Session PostSim, and the existing server/client/join timings. Server and client work overlaps, so these values must not be added to frame time. It also records the charge request and `charge_fired` state, slabs offered the blast, committed damage operations, server/client debris roster counts, and both worlds' body, awake/sleeping, contact, and island statistics. Debris rosters are append-only, not live-body counts. Accepted committed operations and debris growth are effect witnesses; they do not identify which individual slab fractured. This diagnostic retains `perf_gate_pass: null` and does not establish a baseline.

Add `--physics-phases` to an executable run to enable per-island timing and two extra CSVs. `physics-phases.csv` has `frame,world,step,name,ms`: each row is one existing closed physics zone, with its full `physics.server.*` or `physics.client.*` name. `physics-islands.csv` has `frame,world,step,island,bodies,contacts,ns,warm_ns,velocity_ns,store_ns,position_ns`; `contacts` is the number of contact manifolds, and the timing columns are nanoseconds. `step` is zero-based within one frame and one world, so every client step, including any catch-up step, has its own rows. An empty island set has a phase `step` row and no island rows. The summary records `physics_phases`; without the flag the extra files are absent and island timers stay off.

Analyze each `(frame, world, step)` separately: sum its island `ns` values for total solve cost and take its maximum `ns` for the serial island cost. Divide that maximum by that same step's total only when the total is nonzero. Do not combine server and client, or take a maximum from one step over a total from another. The server and client run concurrently; physics subphases nest inside the outer `step` zone and may overlap work on other threads. Their durations therefore cannot be added to obtain frame time. The sink takes a mutex and island timing reads extra clocks, so use matching `--physics-phases` settings when comparing timed runs; this output is diagnostic rather than a baseline.

Add `--contact-profile` alongside `--physics-phases` for one `contact-profile.csv` row per closed server or client physics step, including client catch-up steps:

```sh
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/contact-collapse \
  --city --hip --multi-collapse --warmup 60 --frames 240 \
  --physics-phases --contact-profile
```

The option enables each world's `ContactProfile` after startup, before captured steps. It requires `--physics-phases` because the outer `physics.*.step` close is the safe point where that world's stepping thread copies the completed snapshot. Without the option, there is no contact CSV and no contact-profile clock reads. `summary.json` records `contact_profile` and the row count; `provenance.json` records tool support and the phase-recorder requirement. Capture validation requires one contact row per recorded step and checks `raw_candidate_keys = unique_pairs + duplicate_keys`, `unique_pairs = plain_pairs + compound_pairs + heightfield_pairs`, and `culled_child_pairs <= tested_child_pairs`.

The CSV exports every count and nanosecond field of `ContactProfile`. `pair_query_ns` covers tree queries and raw key generation; `pair_canonicalize_ns` covers sorting, deduplication, and output pairs. `plain_exact_ns` and `plain_speculative_ns` time the convex routines for plain pairs. `compound_pose_ns` covers child pose and AABB preparation. `compound_midphase_ns` is whole child Cartesian-loop time after subtracting the separately timed compound exact, speculative, and warm-cache regions; it includes AABB rejections, loop overhead, manifold setup, feature folding, and output append. `heightfield_ns` includes terrain generation, terrain speculation, and terrain cache lookup. `warmcache_ns` covers only plain and compound cache lookups. These buckets are disjoint but do not sum to the outer `contacts` zone because dispatch, allocation, and clock overhead remain between scopes. The instrument adds clock reads around narrowphase calls, one read pair around each compound child loop, and mutex/CSV work; use it for attribution, not an unprofiled performance baseline.

`--plain-contact-cull` enables the experimental tight-AABB check for plain non-CCD candidates on both worlds. It is off by default and can run without `--contact-profile` for a timing control. With `--contact-profile`, the CSV adds `plain_tight_tests` and `plain_tight_rejections` in `ContactProfile` order, plus the decimal `world_hash` sampled on the same stepping thread at the outer step close. Compare `(frame, world, step, world_hash)` rows from on/off captures with the same input schedule; parse the hash as a 64-bit integer, not a floating-point number. The summary records the runtime cull flag and provenance records its default-off support. A matching hash trace establishes that the cull left the physics state identical for that captured workload; it does not establish a speedup by itself.

## Directional caster A/B and image check

Directional cascade caster culling is enabled by default for the optical view. The current play source fixes main-view culling off. Select the optical off control at runtime, keeping every other option and the source/build provenance equal:

```sh
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/culling-on \
  --city --ads --warmup 60 --frames 120 --ppm /tmp/rime-city-ads/culling-on.ppm
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/culling-off \
  --city --ads --warmup 60 --frames 120 --shadow-culling-off \
  --ppm /tmp/rime-city-ads/culling-off.ppm
```

`--shadow-culling-on` restores the optical default in a later run. The generated path keeps main-view culling off and applies the runtime setting only to the optical renderer. These captures must be identified as scope-only culling; older both-view-on captures are not a like-for-like comparison. `summary.json` reports `main_shadow_caster_culling` and `optic_shadow_caster_culling` separately. `shadow-casters.csv` records each cascade's considered, submitted, and culled draw counts for the main view and for an active optical feed, sampled after graph execution. The optional `--ppm PATH` reads and writes the final LDR frame after the timed loop. It should be inspected alongside the counts to catch an image difference; the capture does not compare pixels or prove identical images. The summary records the culling setting and whether the PPM write completed. These headless runs remain descriptive diagnostics with no performance gate claim.

## Three-building collapse diagnostic

To observe sustained load from several buildings, run the same generated executable with a fresh output directory:

```sh
/tmp/rime-city-ads/profile_play --out-dir /tmp/rime-city-ads/multi-collapse \
  --city --hip --multi-collapse --warmup 60 --frames 240
```

`--multi-collapse` includes the single-collapse measurement fields and requires 180–360 measured frames, with at most 600 warmup frames. It offers three ordinary charges to distinct existing building IDs `{hero_south, hero_north, 0}` at measured simulation ticks 0, 60, and 120. Each charge uses the sample's default radius 11, damage 9, and impulse 7. Its corner comes from still-standing ground-storey slab transforms on that building: minimum X and the Z nearest the street, at height 1.2. Missing or duplicate buildings, no live target, fewer than three requests, or a charge with no committed operation on its frame fail the capture. This path does not raise a physics cap or create synthetic damage operations. The original `--collapse` still calls `Demo::fire_charge()` once.

`frames.csv` records each charge's requested building, actual measured tick, and slab target count. The summary additionally requires each target building's live-part count to decrease on its charge frame; operations from an earlier collapse cannot satisfy that witness. It also records `peers_agree` from the server and client shared destruction state hashes after each timed frame. The summary gives the number of mismatched frames, first and last mismatch, and mismatch count in the last 60 frames, plus per-charge targets and before/after live-part counts. A mismatch during a burst is recorded, not used as a capture failure: transient delivery lag alone does not prove permanent desynchronization. Hashing and the optional final image readback happen outside measured `frame_cpu_ms`. This is a bounded diagnostic, not a baseline, cure, or performance gate.
