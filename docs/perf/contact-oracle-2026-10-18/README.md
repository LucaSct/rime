<!-- SPDX-License-Identifier: Apache-2.0 -->
# Frozen contact oracle and conservative filter experiment

This follows the GJK repair at `35435d2`. Production collision code is unchanged
by this brick. The reusable `samples/contact_bench` tool supplies the fixed-input
oracle needed before another filter can be trusted; it also builds an external
cached-filter world for a full trajectory check.

## Captured workload and sensitivity

The existing city diagnostic fired three ordinary building charges over 360
measured frames after 60 requested warmup frames. Physics startup adds another
30 steps per world: the hash ledger contains 450 server and 450 client steps.
Recording selected every twentieth contact build in each world, before looking
at results. The complete ledger is 900 builds, 44 selected builds, 3,318,610 plain
queries, 166,374 written/selected queries, 302 runtime hull dictionary entries,
and zero cap drops. Compounds and terrain are not frozen here.

The exact reference reproduces all 166,374 results, including 48,879 contacts,
with zero field differences. Actual city pairs have no speculative contacts;
the unit control deliberately exercises approaching CCD pairs and negative
penetrations. One bit flipped in one expected normal causes one mismatch and
refuses timing. The exact matcher is therefore sensitive to last-bit changes.

| policy | skipped exact calls | reference-result differences |
|---|---:|---:|
| unfiltered | 0 | 0 |
| tight world AABBs | 58,887 | **11 — rejected** |
| existing 1 mm child margin plus outward ULP | 58,008 | **0** |

The 1 mm margin was selected from the existing compound policy before the
capture was examined, not enlarged until the scene passed. An outward ULP
retains one additional call relative to the unrounded policy. These are
reference contact semantics; this experiment does not claim all near-touch
contacts represent mathematically intersecting solids. Tight bounds remain
unsafe even after the two October 17 ghost contacts were repaired.

`city.contacts.gz` contains the full selected inputs and expected contacts.
Decompress into a scratch directory, then run the CLI's default verification.
The format and commands are documented in
[`samples/contact_bench/README.md`](../../../samples/contact_bench/README.md).

## Recording and full-world identity

Recording off/on matches **900/900** world hashes. A separately compiled external
variant caches bounds lazily per body for each contact build, applies the fixed
margin/outward-rounding policy only to the exact call, and preserves the
speculative fallback. Its hashes also match **900/900** against the unfiltered
reference, including every startup/warmup step, not just the selected corpus.

The 720 measured contact-profile rows have the same 3,318,450 plain pairs and
992,631 produced manifolds. Exact calls, including compound dispatch, fall from
3,365,643 to 2,205,878: **1,159,765 fewer calls**. Pair ordering, contact outputs,
body trajectories and the existing late peer disagreement are unchanged in this
run. Both versions still mismatch on 145 presentation frames, including 33 of
the last 60. This does not close the city burst-convergence gate.

The compressed hash/contact/frame ledgers, summaries and source/build commands
are retained for all three runs. Their wall-clock pass/frame values are
instrumented diagnostics collected alongside functional tests; they are not
performance measurements. Match rows by semantic keys, not append order.

## Controlled CPU measurement

After builds/tests stopped, `measure.py` ran the immutable corpus on CPU 0 of
the Ryzen 9 9950X3D in ABBA order, 21 corpus repetitions per run. Its live quiet
guard passed, no competing process crossed its threshold, and clock-median
drift stayed below the predeclared 5% limit. Input/contact/work identities match
in all four runs; the raw report contains CPU/cache/governor, clock samples,
corpus and binary SHA-256.

| policy/order | corpus p50 ms | corpus p99/max ms |
|---|---:|---:|
| reference A1 | 110.653 | 111.082 |
| filter B1 | 104.248 | 104.532 |
| filter B2 | 104.320 | 104.562 |
| reference A2 | 110.881 | 111.287 |

This is about **6% less whole-corpus CPU time** with the more expensive
per-pair bound computation. It includes dispatch, bounding and checksum work;
it excludes parsing and integration. The external full-world variant caches
bounds, so these timings do not price that cache or the complete frame. They
are neither a native-FPS result nor a passing M18 budget. Two ABBA arms and 21
repetitions each describe this fixed workload, not every debris scene or scale.

## Verification and next boundary

Four new format/oracle/concurrent-recorder cases pass **211 assertions** with
GCC, ASan/UBSan and Clang TSan. Negative cases cover truncation, footer/accounting,
cap drops, duplicate keys, unsupported version/types, nonfinite values, invalid
topology and mutation of contact bits. Full software-Vulkan CTest is **88/89**
in 295.69 s, with the same three chaotic block locality/collapse claims red as
the parent. Physics, wall, destruction/network, effects, environment and
viewmodel checks pass. No assertion was relaxed. Root reviewed the diff; no
independent premium consultation ran.

The next runtime brick can reuse this oracle and the external variant's policy,
but needs public-seam cache invalidation/CCD tests, the exact final production
source's hash proof, and a cost measurement that includes its actual cache.
No rejected tight-filter timing or altered trajectory is used as a speedup.
