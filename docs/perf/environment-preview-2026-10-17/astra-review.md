**1. Build reload around explicit magazine and hand contact transforms.**

The current failure is structural: [play_viewmodel.cpp:573](/home/next/projects/rime/samples/99-the-block/play_viewmodel.cpp:573) raises/cants the receiver, line 591 independently translates wrists, and line 635 gives every gun part the same transform. [play_weapon_motion.hpp:102](/home/next/projects/rime/samples/99-the-block/play_weapon_motion.hpp:102) supplies overlapping envelopes, without magazine attachment phases.

My smallest useful implementation is **one complete M4 reload**, using the reported `Magazine_Mag` component, before extending to merged SCAR/MSR geometry. Replace reload’s large receiver offsets with a calibrated, reachable receiver pose; preserve the firing wrist’s constant receiver-local grip transform.

For extraction:

- Introduce authored component IDs and a versioned selection table tied to an exact cooked-geometry fingerprint. For M4, validate the named component’s mapping into cooked triangle ranges; these files do not establish whether names survive cooking.
- For merged assets, author triangle membership from the existing local artwork. Disconnected topology can assist selection, but UV seams can fragment components and welded geometry can join unrelated surfaces. Bounds should validate selection, never define an uncontrolled runtime cut.
- At load, verify fingerprint, triangle IDs/counts, materials and expected bounds before mutation. Partition whole triangles; copy complete vertex attributes with index remapping, preserving material ranges. Validate that the partition neither loses nor duplicates triangles. Reject changed artwork explicitly.
- If extraction exposes missing magazine surfaces or an invalid receiver opening, author the split and missing surfaces in the existing asset. Do not conceal damage through clipping. The existing guarded extractor at [play_viewmodel.cpp:46](/home/next/projects/rime/samples/99-the-block/play_viewmodel.cpp:46) is a useful transactional precedent, not sufficient magazine geometry.

Author a magazine seating transform, extraction axis, and **magazine-to-wrist grasp transform**, calibrated against the actual palm/finger surface—not the wrist origin alone. While grasped:

`weapon_from_support_wrist = weapon_from_magazine × magazine_from_wrist`

Use explicit phases: support hand leaves foregrip → reaches stationary magazine → establishes grasp → pulls magazine clear along its insertion axis → returns and seats it → releases → returns to foregrip. This first slice demonstrates coherent removal/reinsertion; a replacement from a pouch can follow. It follows the supplied normal-motion description, without borrowing juggling behavior.

Keep shoulders fixed. Validate the complete trajectory, including ADS exit, bob and transitions, against measured reach with margin and [IK position/orientation errors](/home/next/projects/rime/samples/99-the-block/play_arm_ik.hpp:20). During grasp, never shorten only the wrist path: revise magazine and hand motion together. Endpoint reach checks alone cannot exclude crossing the inner unreachable sphere. Add component-specific draw transforms and temporal history; update the loader’s current range/material indexing assumptions.

Uncertainty: I inspected no geometry, so exact selections, grasp coordinates, finger closure and exposed surfaces remain unverified.

**2. Optimize demonstrated CPU work before committing collision to GPU.**

Your reported 66–69% of a 6–10 ms step implies roughly **4–6.9 ms in contacts**. It does not attribute broadphase, child processing, collision routines or cache work. GPU idleness is a point-in-time observation. The [prior consultation](/home/next/projects/rime/docs/perf/collapse-scaling-2026-10-17/consultations/astra-capacity.md) likewise supplies hypotheses, not attribution.

After the instrumentation lands, my next two CPU changes would be:

1. **Cache posed children once per world contact pass.** [world.cpp:639](/home/next/projects/rime/engine/physics/src/world.cpp:639) already avoids posing inside the Cartesian loop, but still allocates two vectors and recomputes poses, hull resolution and bounds for every parent pair. Use reusable contiguous storage indexed by body generation and child index. Preserve tight bounds plus swept variants: either parent’s CCD flag currently activates sweeping for both sides. Rebuild each pass initially, avoiding complicated invalidation. Benefit depends on repeated compound participation.

2. **Parallelize canonical pair evaluation with bounded jobs.** [world.cpp:543](/home/next/projects/rime/engine/physics/src/world.cpp:543) processes pairs serially. Freeze input/cache reads; use per-job scratch, outputs and counters; concatenate contiguous pair ranges in original order, preserving child order and warm-start identity. Audit called routines for hidden mutable state. Share one worker budget across concurrent server/client worlds; benchmark total latency and p99, not isolated throughput. Require identical contacts and hashes. A child BVH becomes preferable if instrumentation shows Cartesian culling dominates.

For GPU, start with a capped, client-only **shadow child-AABB rejection pass through Vulkan RHI**, retaining CPU narrowphase and canonical CPU server authority. Upload CPU-produced conservative bounds; return an indexed candidate bitset. Budget upload, dispatch, synchronization, readback and CPU filtering together. Late results or overflow trigger full CPU work; never reuse stale rejection.

Compare against CPU candidates across devices. Quantized outward-rounded integer bounds can reduce floating-point disagreement; GPU contact floats must not enter authoritative hashes. Software Vulkan should exercise correctness with CPU fallback; hardware measurements determine usefulness. RHI compute/readback capability remains unverified within the permitted files.