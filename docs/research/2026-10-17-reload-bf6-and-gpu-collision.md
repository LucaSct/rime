<!-- SPDX-License-Identifier: Apache-2.0 -->
# Reload references, BF6 asset observations and GPU collision scope

Owner request: inspect reload photographs and https://www.youtube.com/watch?v=rKHv9CBno0g,
then improve the prototype without importing proprietary assets. Observations below come
from downloaded 720p video frames and the cited photographs, not an asset dump.

## Reference inspection

DANNYonPC’s 38-second video is titled **2 secret reload animations in Battlefield 6
(PW7A2 + P18) Easter egg**, uploaded 2026-03-18. Sampled all 38 seconds at 1 Hz and
inspected the contact sheets; closer timing should use the original 60 Hz frames.
At about 3–16 seconds the compact weapon is canted, with detached magazine gestures;
the exaggerated toss at about 7/12/13 seconds is deliberately an Easter egg. At about
23–37 seconds the pistol sequence includes exaggerated hand movement and inspection.
These are not evidence of ordinary reload technique, nor references for our MSR’s bolt.
The camera/background stays readable while the hands/weapon move independently.

A [US Army National Guard photograph](https://www.dvidshub.net/image/7247655/idaho-army-national-guard-annual-training-2022-charlie-company-weapons-training)
was downloaded and inspected: the firing hand supports the receiver while the support
hand contacts the magazine; the gun is raised/canted and the elbows remain bent. The
photograph is third-person, so its projection is not a first-person pose template.
The [BF6 rifle image](https://www.gamesradar.com/games/battlefield/battlefield-redsec-assault-rifles/)
was also inspected: the magazine-hand interaction is large and visible while the muzzle
can leave the frame. Showing the entire barrel is not the framing criterion; the actual
action/contact is. Several image-search hits depict filling loose magazines instead of
reloading a held weapon and were not used as contact references.

Our previous pose raised/canted the entire gun and independently moved a wrist while
all weapon geometry, including the magazine, retained one model matrix. Better offsets
cannot fix that missing articulation. The next implementation uses whole-component
magazine geometry, an authored seating/contact frame, and ordered approach, grasp,
extraction, seating and return phases. The firing wrist stays in receiver coordinates.
The source finger pose is fixed; wrist contact alone does not prove a convincing grasp.
A removal/reinsertion demonstration is not a fresh-magazine exchange.

## What is visible in BF6’s assets

* Weapons separate receiver, detachable magazine, optic, mount/rail and small controls.
  Those silhouettes and contact relationships remain readable during movement. Strong
  camouflage is localized to the weapon; optic lenses, controls and gloves retain their
  own appearance. Different surface highlights distinguish coated metal and polymer.
* Gloves show stitching, seams, padding and fabric grain at different scales. Wrinkles
  and the opposed thumb make a hand read as a grasp rather than a wrist point. Our
  generic fixed fingers and the MSR’s solid source materials remain visible limitations.
* The range reuses shutter bays, tables, chairs and overhead fixtures. Local numbers,
  paint bands, worn edges, dirt, pipework and uneven clutter break the repetition. Large
  clean planes support mid-scale structure and small details, instead of random noise
  at every scale. Light strips have housings and sit in believable ceiling structure.
* Bright light pools and reflected highlights articulate otherwise dark surfaces;
  exposure preserves the range behind the detailed weapon. This clip contains no useful
  outdoor collapse, water or dense-smoke test, so it cannot validate those systems.

Trim sheets, tiled material layers, decals, baked normals and animation rigs are plausible
ways to produce this result, **inferences** rather than facts established by the video.
Triangle counts, texture sizes, collision meshes, LOD policy and original shaders cannot
be recovered reliably from these frames. Do not claim them. Priorities for Rime: correct
hand/part contact, independent material response, authored modular building details and
material-directed destruction effects before adding more uniform geometry/noise.

EA’s [BF6 engineering account](https://www.ea.com/amp/news/how-battlefield-6-redefined-destruction)
explicitly describes persistent ruins, part/debris networking prioritization, and surface/
seam emitters driven by debris activation and shockwaves with material-specific VFX/audio.
That supports reusable event consumers and separate simulation/visual budgets; it does
not establish how every visible prop in this clip was authored.

## CPU ownership and GPU opportunity

The initial process observation found a `ghrunner/the_block` CI process at roughly
366 percent of one logical CPU, separate from the interactive demo. On this 32-logical-
CPU machine that is about 11.4 percent of total logical capacity. A later three-second
/proc delta sample found CI `rime_render_tes` at 132 percent of one CPU (4.13 percent
of the machine), while no player `the_block` was running. Lifetime ps percentages are
not instantaneous benchmarks. Do not attribute all desktop CPU use to play.

At 21:46 a separate Ollama load appeared: Qwen3.6 35B, with about 10 GB resident on
the GPU. Following the owner’s standing unload instruction it was unloaded through
Ollama’s API at 21:51; /api/ps then returned an empty model list. Do not infer that
Ollama was continuously loaded during the earlier captures. Fresh collision A/B
captures start after the confirmed unload.

The earlier fixed collapse diagnostic measured contact generation at 66–69 percent of
physics step duration, in both local server/client worlds. Those worlds overlap; their
times cannot be added to make a main-thread frame time. New opt-in instrumentation
separates pair queries/canonicalization, compound posing, midphase, exact collision,
terrain and cache work. It is diagnostic and reads clocks; performance baselines must
use the production/default-off path and identical physics workloads.

[NVIDIA PhysX GPU documentation](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/GPURigidBodies.html)
demonstrates real GPU broadphase, contact generation and constraint solving. Its CUDA
implementation requires a supported NVIDIA GPU, uses bounded buffers and retains some
CPU-only functionality, including scene queries. It is evidence that GPU collision is
feasible, not a switch available in Rime’s custom CPU solver or a cross-vendor RHI path.

Concrete code to study: [PhysX SnippetHelloGRB](https://github.com/NVIDIA-Omniverse/PhysX/blob/main/physx/snippets/snippethellogrb/SnippetHelloGRB.cpp)
shows scene initialization and GPU buffer/feature configuration. [Jolt StaticCompoundShape](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Collision/Shape/StaticCompoundShape.cpp)
is inspectable CPU child-tree traversal, useful when our measured Cartesian child tests
justify a midphase tree. Jolt is MIT licensed; retain attribution if code is ever adapted.
Neither library is being added as a dependency in this brick.

An independent GPT-6 Astra read-only consultation recommended per-step posed-child reuse,
then bounded canonical pair jobs if measurements justify them. It independently identified
the reload’s missing magazine articulation. Root agrees that these are candidates, but the new 360-step-per-world capture
changes the order: plain exact collision is 77.3/77.6 percent of attributed server/client
contact time, compound posing 1.3/1.4 percent, and compound midphase 0.3 percent.
The initial compound-cache recommendation is therefore a small win on this workload.
Prioritize understanding false broadphase candidates before plain exact collision.
A tested tight-bound filter was rejected, as detailed below.
Parallelizing before attribution risks spending cores on cheap work while server/client
already share workers.

The bounded GPU experiment should first run **in shadow**: upload conservative child
bounds through RHI compute, return a candidate bitset and compare with the CPU oracle.
Integer outward-rounded bounds are worth investigating for reproducible inclusion. Count
upload, dispatch, synchronization, readback and filtering together; stale/late/overflowed
results must fall back to the full current CPU candidate set. Keep server authority and
CPU narrowphase. A useful comparison needs small and large scenes, graphics running,
RTX3060 hardware plus software-Vulkan correctness, and identical candidate/contact/hash
witnesses. No GPU speedup or cross-device determinism has been demonstrated yet.

Reference video/photos and consultation transcript are local review artifacts under
/home/next/reports/rime-reload-environment-2026-10-17; third-party imagery is not committed.

## Rejected tight-bound experiment

An optional tight world-AABB filter skipped roughly one third of plain exact calls,
but changed 209 of 720 per-world-step hashes in the collapse capture. Compare rows
by `(world, frame, step)`; server/client CSV append order is not stable. Apparent
timing improvements are invalid because the contact workload changed.

A shadow variant kept every existing collision call and only logged would-be rejects.
It reproduced all 720 baseline hashes. Some hull/hull calls return one zero-depth
contact even with positive axis-aligned separation: the first client example has
18.45 mm of Y separation, and the first server example has 5.41 mm of Z separation.
Further observed examples reach 23.40 mm. This points to the GJK/EPA contact path,
but does not isolate its underlying numerical cause or establish a fix. Four local
hull snapshots plus poses are retained for a small reproducible fixture. No relation
to the existing burst replication divergence is established.

Both experimental production commits (`2b7af70`, `556166e`) were reverted. The safe,
default-off contact phase/work counters remain. Next: reproduce the separated pair
in isolation, establish correct contact semantics, then test conservative filtering
with identical contacts and per-step hashes before measuring a speedup. Increasing
the margin until this one scene passes would conceal the correctness question.
