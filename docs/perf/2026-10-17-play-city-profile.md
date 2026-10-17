<!-- SPDX-License-Identifier: Apache-2.0 -->
# City draw submission and collapse attribution — 2026-10-17

Follow-up to [the native city captures](2026-10-17-play-city.md). This is optimized
hardware attribution on the RTX 3060, using **blocking headless submissions**, not
a native frame-rate baseline or a performance-gate pass. Ollama was unloaded with
the user's authorization before every `free-*`, `tall-*`, and `cull-*` run below.
Owned builds, tests and the interactive demo were stopped during captures. The
machine is shared; GPU clocks and CPU thread placement were not pinned. Background
process snapshots accompany the native runs, so these are not idle-machine gates.

The [generator and instructions](play-city-profile-2026-10-17/README.md) retain the
same terrain, sunset, lighting and optical view as play. CPU timings cover actual
graph execution/command recording; GPU timings come from completed timestamp
queries. The generated translation units are retained compressed beside the
generator, and each [run](play-city-profile-2026-10-17/runs/) retains raw compressed
CSVs, runtime options, source/link hashes and startup logs. Every recorded frame
has GPU timestamps, with no timing-slot overflow or executed/timed-pass mismatch.
`analyze.py` sums matching passes **within each frame** before computing quantiles;
[analysis.json](play-city-profile-2026-10-17/analysis.json) is its output. The field
`frame_cpu_ms` is wall time around blocking `app.step`, including GPU wait.

## Idle work and geometry growth

After unloading Ollama, the original expanded city submits 5,022 mesh draws to
each shadow cascade, compared with 2,120 for the eight-building control. Three
cascades in each of two views mean 30,132 indexed shadow draws per ADS frame.
The main and optical CSM CPU medians are 2.758 and 2.850 ms respectively; GPU
medians are only 0.346 and 0.304 ms. CPU submission is a distinct bottleneck from
shadow fill cost. Camera-visible prefixes cannot replace shadow caster lists:
off-camera geometry can cast a visible shadow.

The art pass keeps the original eight buildings and varies the added 24 from
two to six storeys. Added slab entities rise from 264 to 409. With lamp diffusers,
the full draw list rises to 6,774 and the main visible prefix to 6,533. Startup
support validation reports 32 supported buildings, 6,540 nodes, 728 ground roots,
zero quarantines, zero unreachable parts and zero missing geometry. These are
support facts, not draw counts or a performance claim.

## Conservative cascade culling experiment

The first experiment enabled culling in both views. All entries retain their
original UBO/material index; deformed vertex streams are retained conservatively.
The actual light frustum is used, with finite-input checks and an outward margin.
Cascade fit, shadow distance, resolution, render features and damage are unchanged.

| diagnostic | CSM CPU p50 / p99, ms | indexed CSM draws/frame |
|---|---:|---:|
| ADS, culling off | 8.286 / 8.700 | 40,644 |
| ADS, culling on in both views | 5.766 / 5.953 | 27,095 |
| hip, culling off | 2.799 / 3.086 | 20,322 |
| hip, culling on | 3.641 / 4.393 | 20,322 |

Each run has 60 warmup and 180 measured frames. The corrected hip control exposed
the cost of testing casters when every object remains inside the main cascades.
An earlier launcher mislabeled an enabled hip repeat as disabled; that run was
excluded and the disabled/enabled pair rerun, with actual options recorded in JSON.
**Production therefore opts in only for the optical view; the engine default is
off.** No claim that both-view culling improves hip performance is justified.

Final ADS and hip LDR PPM bytes are identical with culling enabled/disabled in
their respective pairs. The GPU fixture independently proves exact HDR equality,
actual caster rejection, and sensitivity to removing an off-camera blocker. Its
first outside-caster location was still inside the default cascade; moving it
outside corrected the fixture without changing the shadow assertions.

## Collapse is additional simulation work

`tall-before-*` and `tall-collapse-*` use the same taller city and one normal charge,
fired before the first measured step. No synthetic 1,000-operation burst is used.
The deterministic capture requests one charge against 16 slabs and observes
accepted operations and debris. Over 180 measured ticks:

| diagnostic | PreSim p50 / p99, ms | max awake bodies | largest active island |
|---|---:|---:|---:|
| idle hip | 2.899 / 3.147 | 0 | 0 |
| charge hip | 8.155 / 9.274 | 473 | 417 |
| idle ADS | 2.972 / 3.229 | 0 | 0 |
| charge ADS | 8.308 / 9.534 | 473 | 417 |

The charge generates up to 486 debris roster entries. The roster is append-only,
not a live-body count. Server/client physics-step medians during collapse are
roughly 3.4/3.3 ms, versus hundredths of a millisecond idle. They overlap with other
work and must not be added to frame time. This does not measure sustained collapse
of many buildings, prove settling at long duration, or fix replication burst limits.

## Review and verification scope

Workhorse Codex agents implemented the bounded city, weapon and instrumentation
leaves. Astra reviewed cascade clipping, bounds, UBO indexing and deferred
ownership, then the descriptor-cache design. Claude Opus implemented weapon-policy
proofs, reachable reload motion and the descriptor-cache experiment; Claude Sonnet
implemented effects probes and identified the HUD's local-order hash false alarm.
The root agent reviewed the diffs and owns execution of the tests and measurements.

The first integrated reload overstretched the support wrist by up to 33.98 mm.
The corrected action stays on its authored hand path within measured rig reach;
all three full reload sweeps and the sniper bolt have zero invalid/clamped poses.
The gun meshes remain rigid, without separately animated magazines or bolt handles;
reload is presentation and firing lockout in an infinite-ammo sample.

The complete 87-test lavapipe run passed 86 tests initially, including rendered
reload, live manual-sniper transport and effects checks. The only failure assumed
that exceeding a descriptor pool's maxSets must allocate another pool. Vulkan
1.1+ permits allocations to succeed there; Lavapipe did so. Correcting that
driver-specific counter assertion retained every pixel and eviction check, and
the failed RHI suite then passed. Full RHI and renderer suites also pass on the
RTX 3060. The descriptor fixture explicitly enables validation in optimized builds.
New test translation units pass Clang syntax checks; CI's exact engine/tests
format set passes. [Verification logs](play-city-profile-2026-10-17/verification/)
retain the initial failure as well as the successful rerun. This does not close
M18 gate 7 or authorize city merge.

## Vulkan descriptor reuse and native-window comparison

The backend promotes at most two uniform buffers to dynamic descriptors and caches
immutable descriptor sets per encoder. Resource identity, explicit ranges, image
layouts and sampler identity remain part of the key. Tables are invalidated when
their encoder releases its fence-retired pools. No RHI API, shader quality setting,
physics algorithm or damage budget changes. `RIME_VK_DESCRIPTOR_CACHE=0` provides
the uncached control; `RIME_VK_DESCRIPTOR_STATS=1` emits allocation witnesses.

An ADS counter frame records 40,286 draws in either mode. The uncached mode makes
40,286 allocations/updates across 158 pools; caching makes 334 allocations/updates,
39,952 hits and two pools. Counter-only runs are separate from timed captures.
Hardware pixel/readback proofs pass 5,132 assertions with caching on and off. ADS,
hip and single-charge capture PPMs are byte-identical across cache modes.

The new renderer's uncached mode also canonicalizes binding payloads, so it alone
is insufficient evidence of improvement over the old implementation. Interleaved
runs of the retained original renderer versus the cached implementation give graph
pass CPU p50 totals of 15.988→8.290 ms and 13.556→8.109 ms. These are sums within
each frame, not sums of pass medians. The variability is material on this 9950X3D's
two L3 domains; no exact regression ratio or qualified baseline is asserted.

The more directly useful check is native Wayland presentation, 1280×720 mailbox,
1,981 frames per run with 600-frame hip, ADS and turning phases. Runtime cache
order is off/on/on/off using one executable and the final taller city:

| run | hip p50 / p99, ms | ADS p50 / p99, ms | turning p50 / p99, ms |
|---|---:|---:|---:|
| off 1 | 12.092 / 13.199 | 17.560 / 20.608 | 9.391 / 12.349 |
| on 1 | 5.310 / 8.426 | 12.082 / 12.407 | 5.238 / 8.269 |
| on 2 | 5.330 / 8.593 | 12.096 / 15.473 | 5.246 / 8.271 |
| off 2 | 12.051 / 15.386 | 17.485 / 22.689 | 9.521 / 15.053 |

All four captures pass the strict phase, zero-shot/charge, fixed-extent, terrain
residency and live optical-feed checks. Their [raw native artifacts](play-city-profile-2026-10-17/native/)
include interval alignment, GPU clock samples, source/binary hashes and process
snapshots. This is evidence of a useful native improvement, with the higher
geometry workload enabled. It is **720p idle movement/aiming evidence**, not the
1080p block gate, a multi-building collapse guarantee, or evidence of clock-pinned
comparability.

Claude Sonnet also found that the existing `destruction_net::shared_state_hash`
already compares by wire identity. The root used that routine in the HUD and
declined the redundant new comparator implementation. The known burst-load
replication limitation remains visible rather than being hidden by the HUD fix.

## Three buildings in succession: physics remains a blocker

`multi-collapse-hip` and `multi-collapse-ads` each warm up for 60 ticks and measure
360 ticks. Normal charges target distinct buildings at measured ticks 0, 60 and
120. Each offers 16 slabs; target live-part counts decrease from 420→57, 180→30
and 180→30 respectively. Thus every intended building is affected. Runtime CLI
bounds reject captures shorter than 180 or longer than 360 ticks.

| diagnostic | PreSim p50 / p99 / max, ms | peak awake bodies | largest active island |
|---|---:|---:|---:|
| three charges, hip | 29.110 / 55.840 / 65.405 | 1,322 | 880 |
| three charges, ADS | 15.159 / 33.160 / 41.949 | 1,322 | 880 |

The different physics timings for identical deterministic workloads underline the
uncontrolled CPU placement/load; they do not establish an ADS simulation benefit.
Both runs record identical body counts and peer-agreement patterns. Server/client
step medians are 14.322/12.774 ms in hip and 7.781/7.171 ms in ADS. Neither run
meets the frame budget even before accounting for rendering. Renderer reuse does
not fix the collision/constraint workload of many active fragments.

Both runs have 195 mismatched peer-hash frames, including 28 of the final 60.
The last measured frame still disagrees. Delivery and destruction remain active,
so this bounded capture cannot distinguish delivery lag from permanent divergence
or prove eventual convergence. It also does not establish that every mismatch is
the previously reproduced burst defect. The existing 96-support-releases-per-tick
workaround remains unchanged. Profiling the large contact islands and checking
peer convergence after quiescence are follow-up engine work; #331 stays in draft.

## Standard 1080p gate attempt

After the diagnostics and tests finished, `scripts/perf.sh --preset release
--sample the-block --frames 600 --width 1920 --height 1080 --commit` was attempted
on clean tracked tree `c6f5d91`. It refused with exit 4: a Python process was using
101% CPU. The [refusal log](play-city-profile-2026-10-17/verification/perf-gate-final.log)
is retained. No busy-box override, CPU-affinity substitution, performance JSON or
new qualified baseline was produced. The mandatory hardware gate remains open.
