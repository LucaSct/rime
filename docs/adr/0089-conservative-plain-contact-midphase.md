# ADR-0089: Conservative bounds before plain exact contact generation

- Status: Proposed — implemented and locally verified; delivery gates remain open
- Date: 2026-10-18

## Context

The city collapse attributes most contact-generation work to plain exact convex
dispatch. A previous tight-AABB shortcut changed 209/720 world hashes and was
reverted. Two captured separated-hull contacts were subsequently traced to
numerically flat GJK tetrahedra and repaired. Even after that repair, the frozen
166,374-query oracle finds eleven reference contacts that tight bounds discard.
The engine intentionally has finite near-touch tolerances; exact geometric
separation alone does not prove the current narrowphase emits no contact.

The existing compound midphase already uses a 1 mm contact margin. Reusing that
policy with one outward representable float step preserves all frozen contacts.
An external candidate also preserves all 900 world hashes of three building
charges, avoiding 1,159,765 exact calls. Per-pair bounds improve controlled
frozen-corpus time by about 6%, but repeatedly posing shared bodies wastes work.

## Decision

Apply that policy to plain convex pairs before the exact call. Compute tight and
padded bounds lazily once per body slot **per contact build**. Reuse allocated
storage, but clear validity on every call, including `compute_contacts`.
Integration precedes this interval and solving follows it, so body poses are
immutable while cached bounds are used. Teleports, generation reuse and dense
compaction cannot retain a prior build's pose. Cache storage is bounded by the
world's existing slot storage.

Use the existing 1 mm compound margin, with one outward ULP to keep the float
expansion conservative. Unsupported or unresolved shapes retain the original
dispatch. A rejected current overlap still runs the original speculative CCD
fallback when eligible. Keep canonical pair/manifold ordering, warm-start
features, solver inputs, sleeping and physics authority unchanged.

Expose eligible bound tests, rejections, posed bodies and disjoint bound time
through the existing opt-in `ContactProfile`. A compile-only reference macro
supports the external oracle executable; ordinary runtime builds use the
optimized path. There is no public runtime setting to alter collision semantics.

## Consequences and verification

This adds bounded per-world scratch memory and a validity reset, while replacing
repeated GJK/EPA work for fat-broadphase near misses. Cache costs must be included
in the fixed-workload measurement; skipped calls alone do not prove speed.
Preserve both the frozen contact oracle and full per-step world-hash proof on the
final source. Public-seam tests must exercise teleport, slot reuse, compaction,
empty contact builds and approaching CCD. Shape/rotation/gap sweeps retain real
reference contacts. Sanitizers cover the new cache and related tests.

The existing failed M18 budget, city replication divergence and cross-platform
delivery gates remain. No baseline is replaced, threshold retuned or old rejected
timing reinterpreted. Evidence is recorded in
[`docs/perf/plain-contact-bounds-2026-10-18`](../perf/plain-contact-bounds-2026-10-18/README.md).

## Alternatives considered

- Tight bounds: rejected by the sensitivity control's eleven lost contacts.
- Increase a margin until the scene passes: rejected; the margin predates the
  experiment and comes from the existing compound policy.
- Compute bounds per pair: verified and useful as a cost control, but repeatedly
  poses shared bodies. Keep it in the benchmark for comparison.
- Reuse poses across ticks: rejected; it needs a broader invalidation contract
  and can silently miss teleports, topology changes or reused handles.
- GPU candidates: a later shadow experiment with CPU authority and fallback;
  this CPU optimization introduces no readback or synchronization dependency.
