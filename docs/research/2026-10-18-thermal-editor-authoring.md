# Thermal editor authoring: contract and consultation — 2026-10-18

This brick consumes the existing local numerical field; it changes no material
coefficients or heat solver. See [ADR-0094](../adr/0094-thermal-editor-preview.md)
and the [wire contract](../design/thermal-preview-wire.md).

Before the load-bearing editor/data contract, one tool-free raw devpass Astra
consultation requested its own narrow proposal. Live availability and pricing
were checked first ($10/M input, $50/M output); it ran in the background. Returned
448 prompt + 902 completion tokens, cost **$0.04958**. Shared gateway's exact
usage delta was also $0.04958. The user ceiling is $7; shared weekly cap is lower
and must be checked live before another call. Private credentials/account files
are excluded. Public prompt, answer and attributable usage are retained with
[verification](../verification/thermal-authoring-2026-10-18/README.md).

Adopted: existing generation-safe reflected entity references; whole-graph
validation and cap refusal; separate read-only diagnostics with session, revision
and accepted-commit ticks; exact-length/count/finite/duplicate wire guards; stale
session/revision tests; real host invalidation, Stop and repeated Play proofs.
No consultation output is treated as implementation authority.

Disagreements: Astra suggested canonicalizing signed zero and NaN payloads before
comparing descriptions. The implementation compares explicit serialized double
bits: unchanged invalid NaNs do not churn, but differing authored bits trigger a
rebuild. That rule is simple, exact and testable without hidden normalization.
Astra also suggested stopping collection at the first cap violation. We retain
bounded lowest-key arrays while scanning all matching components so a changed
archetype traversal does not select a different retained diagnostic description.
Overflow still refuses the entire graph; no truncated simulation is admitted.
This bounds storage, not scan work for arbitrarily large authored worlds.

The main reset choice remains deliberate: changing any active setting restarts
all cells from authored initial conditions, including on Pause. Preserving heat
while editing mass/material would require a separate physical policy and could
silently create/destroy energy. Numerical failures retain the last committed
field; invalid authored graphs remove it. The panel communicates both outcomes.

The consultation was an independent design proposal, not a final diff review.
Root owns edits, hunk-by-hunk adversarial review and local proofs. Prior Fable
thermal advice remains in the preceding numerical research; no new Fable call was
needed for this adapter. No solver, rendering or networking claim follows from
model agreement.

An installed codec consumer exposed an existing SDK export defect: stream's
static AV1 imported targets were not re-found. Added `libsvtav1` and `dav1d` to
package dependency discovery, and made the actual SDK smoke run a thermal update
and codec roundtrip. Its previous render-only consumer never exercised that
closure. This is verified by an installed build, not an in-tree link.
