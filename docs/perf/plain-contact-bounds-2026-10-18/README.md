<!-- SPDX-License-Identifier: Apache-2.0 -->
# Cached plain-contact bounds

Implementation: `498b260`, following the frozen oracle at `e5b6e3e` and GJK
repair at `35435d2`. [ADR-0089](../../adr/0089-conservative-plain-contact-midphase.md)
records the proposed decision and remaining delivery gates.

## Runtime behavior and identity

Plain convex pairs now use the existing compound midphase's 1 mm margin plus
one outward ULP before their exact call. Tight bounds remain rejected. Tight and
padded bounds are computed lazily once per body slot per contact build. Validity
resets for every build, including inspections; allocated storage is reused.
Unsupported/unresolved shapes retain the original dispatch. Speculative CCD still
runs after a rejected current overlap. No GPU or asynchronous result is involved.

The replay's `cached` mode uses the same private cache implementation as the
runtime. On the immutable 166,374-query city corpus it retains all 48,879 exact
contacts with **zero bit differences**, skips 58,008 calls, and builds 25,900 body
bounds. Candidate verification compares complete exact/speculative manifolds,
including signed-zero bits and feature IDs, before timing.

The final production source and a compile-only unfiltered reference were then
run through three ordinary building charges. **900/900 world hashes match**;
the new reference also matches all 900 hashes from the oracle parent. This
includes startup/warmup, not just the selected corpus. Every measured manifold
count remains unchanged:

| 720 measured world steps | unfiltered reference | production |
|---|---:|---:|
| plain candidates | 3,318,450 | 3,318,450 |
| exact calls, including compound dispatch | 3,365,643 | 2,205,878 |
| produced manifolds | 992,631 | 992,631 |
| plain bounds rejected | 0 | 1,159,765 |
| body bounds built | 0 | 516,931 |

This removes about **35% of plain exact calls** without changing the measured
trajectories. The per-build counters and disjoint cache time are available through
`ContactProfile` and the external CSV collector. The functional capture timings
are instrumented, were collected alongside tests, and are not speed evidence.
Raw hashes/contact/frame ledgers and source/build provenance are retained here.

## Fixed-workload cost

The initial attempt correctly refused to start while other CPU work was active;
`cpu-refused-workload.json` preserves that refusal. Once the machine was quiet,
the guarded runner completed ABBA on CPU 0 of the Ryzen 9 9950X3D, with 21 corpus
repeats per arm. No quiet/clock override was used. The strict per-process and
aggregate CPU guards passed, and run-median clock drift stayed below 5%.

| policy/order | whole-corpus p50 ms | p99/max ms |
|---|---:|---:|
| reference A1 | 110.989 | 111.258 |
| cached B1 | 87.664 | 88.247 |
| cached B2 | 87.865 | 88.416 |
| reference A2 | 111.267 | 111.528 |

That is **about 21% less whole-corpus CPU time**, with matching input, work and
contact identities. Timing includes cache allocation/validity resets, lazy bounds,
dispatch and semantic checksum; it excludes parsing and physics integration.
The cache is reset on `(world, step)` boundaries. It sizes scratch space from
the maximum observed slot; the live world already owns that slot count. This
does not establish a 21% frame-rate improvement or measure every debris scale.
The input is the parent's `../contact-oracle-2026-10-18/city.contacts.gz`; SHA-256
and binary/source/CPU fingerprints are retained in the report and provenance.

## Verification

- Three runtime cases pass 964 assertions: public-seam teleport, generation reuse,
  dense compaction, empty builds and CCD controls, plus an unfiltered-reference
  sweep over sphere/box/capsule/hull combinations, rotations, offsets and gaps.
- All seven related cases pass **1,175 assertions** with ASan/UBSan and Clang TSan.
  These builds instrument the modified `world.cpp` and related test units; they
  link the unchanged core/ECS dependencies from the ordinary build. This is not
  a claim that the entire engine was rebuilt under each sanitizer locally.
- Full optimized build passes. Software-Vulkan CTest is **88/89 in 317.77 s**.
  The sole failed executable is `the_block_headless`, with the same three
  locality/collapse thresholds as its parent. Physics and all wall, effects,
  environment, viewmodel and destruction/network cases pass. No threshold changed.
- Final runtime, new test, replay and both generated world units pass Clang.
  Pinned engine/tests format, SPDX and diff checks pass. Root reviewed every hunk;
  no new independent consultation or premium spend.

Parent PR #333's actual Linux and ASan logs fail only the block thresholds.
Its Editor job reaches the authoring round trip and fails the same three
thresholds, rather than the channel/viewport smoke. Filtered failure evidence is
retained. Those old-SHA jobs do not cover this runtime implementation; fresh
candidate CI is still required, including cross-platform evidence.

## Standard hardware gate remains open

The normal `scripts/perf.sh --sample the-block --commit` attempt on clean
`498b260`, 1920x1080, 600 measured frames, **failed** (exit 1). Frame p99 was
about 16.25 ms, while **sim.block p99 8.074 ms exceeds 6.000 ms**. Baseline
comparison was refused: NVIDIA 615.78.08 differs from baseline 615.71.09.
Graphics clocks spanned 1185–1935 MHz (40.8% spread), so automatic filing was
refused too. No model was loaded in Ollama; observed GPU memory was 517 MiB at
preflight. No override, rebaseline or budget relaxation was used.

`unqualified-1080p-498b260.json` is retained inside this evidence directory,
not filed as a qualified baseline. The observed frame number does not close
M18's clock-qualified clause, and this run cannot establish a native speedup.
Only the separately controlled frozen CPU comparison supports the 21% claim.

The city still has 145 mismatched presentation frames, including 33 of its last
60; this optimization preserves that defect along with the rest of the state.
Next work is qualified hardware/remaining simulation cost and independent burst
replication diagnosis. Keep the stack draft; nothing here authorizes a merge.
