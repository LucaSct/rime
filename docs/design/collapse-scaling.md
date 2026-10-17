# Collapse scaling: measured priorities and independent review

2026-10-17, city PR #331. This is a next-brick plan, not a promise that collapse
now meets budget. Two independent **GPT-6 Astra, high** read-only Codex sessions
reviewed the existing hot paths and the representation/authority design.
The root reviewed their recommendations and collected additional phase data.

## What the new measurement establishes

An external diagnostic records all existing physics phases and opt-in island
costs separately for each server/client physics step. It uses the same three
normal charges, 60 warmup ticks and 360 measured ticks as the earlier capture.
All requested buildings lose parts; the peak population and peer mismatch
pattern match the earlier diagnostic. No physics arithmetic was changed.

| phase | server p50 / p99, ms | client p50 / p99, ms |
|---|---:|---:|
| whole physics step | 6.114 / 10.376 | 6.143 / 10.481 |
| contacts | 4.349 / 7.488 | 4.042 / 7.201 |
| solve | 1.439 / 2.395 | 1.783 / 3.299 |
| constraints | 0.142 / 0.305 | 0.125 / 0.253 |
| cache commit | 0.139 / 0.340 | 0.131 / 0.357 |

These are phase distributions, not components that may be added by percentile.
Over the full run, contacts account for 69.3%/66.3% of summed server/client step
time. The largest *summed-island-work* server step has 7.152 ms contacts versus
1.808 ms solve wall time; its most expensive island is 1.804 ms. The corresponding
client step has 7.012 ms contacts versus 2.227 ms solve and a 2.205 ms island.
Island durations overlap on workers; summing them is not wall-frame time.

This is a blocking, instrumented hardware attribution capture on a shared,
unaffinitized machine, not a new performance baseline or a cross-run speedup.
The phase sink adds synchronization and per-island clock/storage work. It is
optional and absent from production. Render, server and client timings also
overlap. Raw traces and provenance are retained with the performance report.

The evidence makes **contact generation the first optimization target**.
Parallelizing only the solver cannot eliminate most of the observed step cost.
Body count alone would not have justified that conclusion.

## What exists already, and the first safe brick

Independent islands already dispatch through the job system; the sparse-tail
gate already avoids pointless dispatch for one active island. Recommending
"add island parallelism" would duplicate existing code. Solver tensors and
velocity rows have also already been optimized.

The hot-path review found concrete scaling risks in `physics/src/world.cpp`:
every non-static body queries both trees, including sleepers; dynamic pairs
are emitted twice and then canonicalized; compound children are posed and
bounded again for each body pair; child pairs are enumerated before their
existing AABB rejection; the contact cache warms impulses but does not reuse
computed contact geometry. Several step-local vectors and the cache map also
allocate repeatedly. These are inspected code paths, not measured individual
costs yet.

**Next brick: separate broadphase traversal/key sort, compound child preparation,
child rejection and exact narrowphase costs within contacts.** Add opt-in
timings and counters for raw/unique body pairs, child-pair candidates, rejected
children, posed children and exact convex calls. Keep production ordering and
arithmetic identical. Capture single/three-building scenes and fixed piles at
increasing populations, including sleeping piles disturbed by an impact.

Then take the best measured exact-work optimization: retain scratch capacity,
avoid duplicate key emissions without changing canonical output, cache posed
children once per body/tick, or introduce a conservative child BVH. Invalidate
on movement, fracture/rebuild, shape-generation change, removal and CCD policy.
Prove per-tick state hashes, contact/event ordering and sleep decisions agree
for the supported worker configurations and with profiling off/on. A change
that alters arithmetic or manifold reuse needs a resettable fixed-workload
quality benchmark; chaotic collapse counts cannot validate its performance.

The first Astra review ranked reusable scratch as the safest early candidate.
I retain it as a candidate, but do **not** select it over contact work without
allocation attribution. The second review emphasized representation and network
authority; the new phase data does not invalidate that long-term requirement.

## How more detail can scale

1. Budget **render leaves, authoritative collision bodies and cosmetic debris**
   separately. Existing unsupported connected parts already form compounds;
   extend that model rather than turn every visual chip into a new body.
2. Preserve structural identities, openings, mass/inertia, momentum and support
   connectivity when refining or aggregating collision. Test doorway sweeps,
   projectile holes and support removal across transitions. Existing compounds
   have a finite child limit and no nested representation; hierarchy needs a
   designed extension, not unlimited children in the current API.
3. Keep settled rubble as cover with persistent collision and deterministic
   wake/reactivation. Current optional freezing destroys bodies; simply enabling
   it is not a gameplay-safe performance fix. Retire truly cosmetic chips freely.
4. Make representation/lifecycle transitions server-authoritative and keyed by
   stable identity plus tick/version. Late join needs complete checkpoints and
   ordered transitions. Local visual detail may vary; damage and cover may not.
5. Continue solver scaling only after attribution: exact dependency scheduling
   must preserve shared-body update order, while graph coloring/reordered
   constraints usually changes the numerical result. Worker-count determinism
   is distinct from reproducing the old solver bit for bit.

Do not lower iteration counts, force active piles asleep, delete cover or increase
the support-release cap as a substitute for optimization. The three-building
capture still does not prove peer convergence after quiescence. Fixing rendering
or collision cost does not fix that separate correctness question.

## Concrete contemporary code and technical sources

* **Erin Catto, October 12, 2026: [Contact Recycling](https://box2d.org/posts/2026/10/contact-recycling/),
  [implementation PR](https://github.com/erincatto/box2d/pull/1038),
  [contact.c](https://github.com/erincatto/box2d/blob/main/src/contact.c),
  [MIT license](https://github.com/erincatto/box2d/blob/main/LICENSE).** Reuses
  contact geometry when relative motion permits, updating separation. This
  directly addresses repeated collision work in mostly settled piles. Rime's
  impulse cache is not this geometry cache. It is an experiment requiring
  shape/generation invalidation, rotation/rolling/CCD controls and quality tests;
  a 2D implementation is not a ready-made 3D replacement.
* **Jorrit Rouwe's Jolt: [LargeIslandSplitter.cpp](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/LargeIslandSplitter.cpp),
  [MIT license](https://github.com/jrouwe/JoltPhysics/blob/master/LICENSE).** Actual
  production-oriented 3D source partitions large contact/constraint work into
  compatible batches and retains a nonparallel fallback. Study its dependency
  handling for a later solver experiment; do not assume its ordering matches
  Rime's existing sequential impulses.
* **Catto's [determinism article](https://box2d.org/posts/2024/08/determinism/)
  and [simulation-islands article](https://box2d.org/posts/2023/10/simulation-islands/).**
  Concrete developer accounts of stable contact ordering, thread-local bitsets
  and persistent island management. Race freedom alone does not establish
  deterministic ordering. These principles transfer; a wholesale 2D-engine
  transplant does not.
* **[AMD FEMFX](https://gpuopen.com/archived/femfx/).** An archived deformable
  fracture library is a useful comparison for a different material model, not
  evidence that finite elements will make Rime's rigid rubble cheaper. It is
  deliberately outside this first contact optimization brick.

No third-party source was imported. The water/smoke research and their own code
references live in the [separate effects milestone plan](environment-effects-milestones.md).
