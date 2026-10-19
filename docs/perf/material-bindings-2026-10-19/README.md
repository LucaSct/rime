<!-- SPDX-License-Identifier: Apache-2.0 -->
# Invocation-local material attachments

Implementation: `19c70fd`, based on `7be38d2`. This brick removes repeated renderer
calls that attach the same material textures/sampler within one geometry partition.
It changes no draw ordering, draw count/range, UBO offset, sampler choice, mesh stream,
shader, contact semantics or budget threshold.

`record_draws` now keeps five last-attached `(texture, sampler)` pairs. Whole
handles include generations. Each slot's first attachment is always issued,
including null handles; any change of either handle issues that slot again.
The cache is stack-local to **one** invocation. Pipeline switches and external
callbacks occur outside the loop and therefore cannot inherit its remembered state.
Other passes/partitions and subsequent frames always start unknown. No callback,
resource transition, creation/destruction or pipeline switch occurs inside this loop.

This respects the existing RHI contract: pending sampled attachments persist
across draws. Vulkan `bind_texture` resolves texture and sampler handles and stores
pending state; it does not itself issue a Vulkan descriptor bind. The per-draw UBO
attachment still dirties descriptor state, and every draw/descriptor bind remains.
These are **CPU attachment-call reductions**, not fewer GPU draws or a measured
reduction of descriptor-set writes.

## Fresh deterministic controls

The pure CPU recorder compares effective pending state at every draw against an
unconditional-binding reference, including texture/sampler generations, draw UBO
offset, index range and flags. It uses the actual production cache helper. Inputs
are generated fixed workloads, **not a captured current city material stream**.
The 2,206-draw case matches a historical optical draw count only to give scale.

| generated workload | draws | unconditional calls | cached calls | effective state |
|---|---:|---:|---:|---|
| repeated maps | 2,206 | 11,030 | 5 | matching hash |
| alternating all five maps | 2,206 | 11,030 | 11,030 | matching hash |
| only normal map changes | 2,206 | 11,030 | 2,210 | matching hash |
| generations, sampler changes, filtered gaps | 1,470 | 7,350 | 414 | matching hash |
| empty | 0 | 0 | 0 | matching hash |

Separate controls cover a new invocation after arbitrary pending-state disturbance,
null first attachments, and hash sensitivity to a changed texture generation,
draw range and draw order. A false always-skip or slot-only cache would fail.
GCC and Clang ASan/UBSan with leak detection each pass **3 cases / 29 assertions**.
Both changed translation units pass Clang syntax using the real optimized build
flags, plus `-Wno-#warnings` for the inherited doctest 2.4.11/GCC16 `<ciso646>`
compatibility warning. Pinned clang-format 20.1.8 and diff checks pass.
No new build tree was created. Root owns the integrated renderer/Vulkan checks;
this directory does **not** establish pixel equality or native frame improvement.

After the root's ordinary build, reproduce the CPU checks without creating a GPU:

```sh
build/release/bin/rime_render_tests --test-case='material bindings*'
```

## Why this location, and what remains unknown

Historical descriptor-cache-on ADS instrumentation in
[`play-city-profile-2026-10-17/analysis.json`](../play-city-profile-2026-10-17/analysis.json)
attributes CPU p50 2.90158 ms to main forward shading (6,533 draws) and 0.998975 ms
to optical forward shading (2,206 draws). These are instrumented headless pass
measurements, not current baselines and not speedup evidence for this change.
October19's unqualified worst-frame `frame.submit` 12.842 ms includes
`rhi.submit.wait` 10.827 ms: it must not be presented as 12.842 ms of CPU encoding.
There was **no timing run** during parallel editor verification. Actual city
material locality, native time saved and hardware budget qualification remain open.

## Existing physics candidate audited, unchanged

`perf/plain-contact-bounds` (`f9371de`) is already an ancestor of `7be38d2`.
Its complete physics, direct tests and frozen replay source diff is empty.
There is no additional cache optimization to cherry-pick. Fresh execution of the
existing optimized binaries (built October18, **not recompiled for this audit**)
reproduces the immutable 166,374-query corpus: reference/cached both retain 48,879
contacts, hash `6f2d7846c7037783`, zero bit differences. Cached dispatch skips
58,008 exact calls and builds 25,900 bounds. Tight bounds still lose eleven
contacts and refuse verification. The three direct cache tests pass 964 assertions.
An initial unsupported positional filter ran the whole physics executable instead;
that also passed, **193 cases / 137,746 assertions**, and is retained separately.
Binary/source/corpus fingerprints and the negative-control log are retained.

A proposed lazy-padding leaf was rejected before editing: only 1,921 of the
25,900 encountered body bounds are never consumed as the canonical first operand
(7.42%). The retained 720-row instrumented profile attributes 4.98% of summed
contact substage time to the **whole** bounds stage. Multiplying those fractions
(~0.37%) is only a proportional screening estimate, **not a rigorous upper bound**
or measured saving: per-body hull/tight/padding costs differ. Extra cache state and
branches were unjustified. No physics code, tolerance, solver order or assertion changed.

Inherited 21% frozen CPU speed evidence and 900 matching world hashes remain in
[`plain-contact-bounds-2026-10-18`](../plain-contact-bounds-2026-10-18/README.md).
They are not new results of this brick. Collapse/convergence, standard hardware
performance, clock qualification and cross-platform delivery gates remain open.
No baseline replacement, merge or gate relaxation was attempted.
