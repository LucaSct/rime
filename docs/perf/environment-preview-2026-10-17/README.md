<!-- SPDX-License-Identifier: Apache-2.0 -->
# Environment preview and rejected collision filter

Release, RTX 3060, 2026-10-17. These are bounded hardware diagnostics, not a
qualified frame-rate baseline or evidence that the city replication gate passes.
Default play leaves the new effects disabled. No GPU collision implementation or
safe collision speedup is delivered by this brick.

## Collision attribution

The generated city capture uses `--city --hip --multi-collapse --warmup 60
--frames 360 --physics-phases --contact-profile`. The generator and documented
semantics are in `../play-city-profile-2026-10-17/`. Per-world clocks and counters
are default-off in production. Instrumentation itself adds overhead. The server
and client worlds overlap, so their durations cannot be added as frame time.

In 360 steps per world, plain exact collision accounts for 77.3%/77.6% of the
attributed server/client contact buckets. Compound posing is 1.3%/1.4%; compound
midphase is 0.3%. These percentages cover the listed buckets, not the entire
physics step. Raw counters, timings, frame witnesses and source/build provenance
are retained in `contact-collapse/`.

An independent GPT-6 Astra read-only review suggested compound pose reuse first.
The measurement changes that priority: it is a small target in this workload.
The review also identified missing magazine articulation and proposed conservative
GPU shadow filtering. Its report is retained; recommendations are not proofs.
Bounded implementation work used cheaper workhorse agents with local review.
No paid devpass premium call was made.

## Rejected optimization

The opt-in tight-AABB experiment in `2b7af70` and `556166e` skipped roughly a
third of plain exact calls, but changed 209/720 per-world-step hashes. It was
reverted by `8783a5e` and `31ac589`. Unit tests alone did not catch this failure.
Do not reuse its timing difference as a speedup: its contacts and later workload
changed. Match CSV rows by `(world, frame, step)`, never by append order.

The shadow variant retained every collision call and matched all 720 baseline
hashes. It logged hull/hull zero-depth contacts despite positive world-AABB
separation: the first client/server pairs are separated by 18.45/5.41 mm; further
observations reach 23.40 mm. `ghost-contact-hulls.json` preserves first-pair hull
vertices, topology, normals, poses and observations. It is input for a small GJK/
EPA reproducer, not a root-cause determination. No relationship to the previously
observed burst replication defect is established.

`cull-off/`, `cull-on/` and `cull-shadow-hulls/` retain raw contact and frame CSVs,
summaries and provenance. Compressed shadow source/build commands preserve the
diagnostic; it links a replacement world object before the engine archive.
Generated source snapshots retain the exact profiled sample/render translation
units, including the then-uncommitted integration edits. Its preparation script
targets the historical experiment source, not the reverted
production API. Reconstruct at those commits in an isolated worktree. No binary
or third-party video/photo is committed. `artifact-sha256.json` fingerprints the
retained diagnostic artifacts.

## Preview boundaries and verification

`the_block --play --effects-preview` enables bounded ambient smoke, scripted
fire light/smoke and a destruction-event consumer. The shared pool is 256 particles.
Sorted alpha billboards use approximate incident RGB, with no volume simulation,
self-shadowing or soft depth intersections. Fire is cosmetic; it has no fuel,
spread or authoritative damage. The conservative CPU water basin is separately
tested; the visible pond remains static authored geometry.

The hardware effects proof freezes simulation and toggles presentation with a
drift control; hashes must remain unchanged and particles/render drops are
counted. Its 640x360 GPU pass times are diagnostic, with blocking headless pacing,
not native FPS or a 1080p/collapse performance gate. All three reloads additionally
have complete 60 Hz reach/contact sweeps and hardware image checkpoints. Fixed
finger poses and reinserting the same magazine remain prototype limitations.

Final validation results and exact restart commands are recorded in the session
handoff. Qualified 1080p performance, collapse/network stress, and fresh cross-OS
sanitizer CI remain separate delivery gates; old PR checks cover old SHAs only.

Hardware proof results (Release, NVIDIA RTX 3060): all three reload sweeps and
rendered phases passed, with 66/66/84 magazine-contact frames and zero measured
wrist-to-contact gap. The contact sheet was inspected: articulation/framing is
improved, but fixed fingers, a low magazine action and a dominant right-hand MSR
remain visible art limitations; this is not a claim of final animation quality.
The effects proof passed with 4,052 changed pixels versus zero drift, 72 live/
requested/spawned particles, zero capacity/render drops and unchanged physics
hashes. Its 24 GPU smoke samples have p50 0.004096 ms and p99/max 0.382976 ms;
the short capture includes startup/upload effects. These are not a collapse cost
or an uninstrumented 1080p budget. Logs accompany these observations.

## Final wrap gate results

All 89 CTests passed on software Vulkan in 286.97 seconds, including the new
smoke proof and expanded viewmodel proof. Final Release build, exact pinned
format, SPDX and diff checks passed. Fresh sanitizer and cross-OS CI is pending.

The standard `scripts/perf.sh --sample the-block --commit` attempt on `b376d15`
failed and correctly refused automatic filing. The diagnostic is retained here as
`unqualified-1080p-b376d15.json`, not as a qualified baseline. At 1920x1080/600
frames, sim.block p99 was 7.397 ms against the 6.000 ms budget. Comparison was
refused because the current NVIDIA 615.78.08 driver differs from baseline
615.71.09. Graphics clock spread was 93.9% (210–1935 MHz); the clock-stability
gate also failed. No busy-box override or gate relaxation was used. This does
not establish a measured regression or a pass. Do not rebaseline merely to erase
the failure. Stabilize hardware and compare deliberately before merging.
