## Proposed boundary

Add removable `fluids_scene` above ECS/fluids. It owns reflected authoring registrations, scene validation, preview lifetime, and diagnostic production—not solver changes, transport authority, or rendering. Runtime samples and journals live only in the preview side table.

### Smallest useful contract

- All three authored components default to `enabled=false`.
- `ThermalCell`: fixed mass, initial temperature, phase fraction, material description.
- `ThermalContact`: two cell references and finite, nonnegative conductance.
- `ThermalSource`: one cell reference and finite signed watts.
- Define how temperature/fraction map to the material curve; reject incompatible combinations rather than silently normalizing them.
- Limits apply to **enabled** descriptions: 16 cells, 32 contacts, 16 sources. Disabled descriptions neither participate nor reserve capacity.
- Contacts are undirected: reject self-links and duplicate unordered endpoint pairs. Reject multiple enabled sources targeting one cell.
- Every enabled reference must resolve to a live, enabled cell in the same scene. Any invalid enabled description invalidates the entire preview.

## Hard integrity traps

**References:** use the existing reflected entity-reference type, not integer lookalikes. Scene loading remaps scene-local IDs; wire edits remap session EditorIds; Play snapshot/restore must cover these fields automatically. Resolve references generation-safely: recycled ECS indices must not retarget old links. Diagnose missing references without dereferencing them. Use deterministic graph/source ordering independent of registry iteration.

**Change detection:** compare canonical authored descriptions, never evolving runtime state. Include enabled state, references, initial conditions, material contents, and integration-relevant configuration. Exclude names, selection, and unrelated components. Canonicalize signed zero and NaN payloads for comparison; still reject all nonfinite physical values. Thus an unchanged invalid NaN description remains invalid without rebuilding every frame. Do not rely solely on hashes.

**Allocation:** perform a bounded scan first, stopping at the first cap violation; do not collect an unbounded ECS graph and then check its size. Bound nested material data too. Validate into fixed-capacity staging storage and publish only a complete graph.

## Lifetime and diagnostics

At each host boundary, process accepted edits before validation/stepping:

- Semantic changes rebuild **everything** from initial descriptions, even while paused.
- Unchanged descriptions preserve evolving state; Pause freezes stepping, not validation.
- Invalid edits immediately discard the preview and clear diagnostic rows.
- Stop clears runtime state; a subsequent Play always reconstructs it.
- A refused numerical tick retains the last committed whole-field values, explicitly labeled **refused**, never “current successful.”

Emit one dedicated, versioned, bounded readonly message after `PlayState`. Include host tick, Play-session ID, description revision, status/reason code, last successful commit tick, and up to 16 cell rows keyed by EditorId. Distinguish `stopped`, `invalid`, `ready/paused`, `advanced`, and `refused`; invalid/stopped carry no rows. Rows contain only finite doubles with documented units.

The inspector displays a selected-cell row only for the matching session/revision; otherwise “unavailable/stale.” Never write diagnostics into authored components.

Decoder: validate version, exact payload length, row count before allocation, unique/nonzero IDs, status/row consistency, and finite doubles. Unknown message types remain safely skippable by old peers. Old scenes lacking these components remain unaffected; removal must preserve existing unknown-component loading policy.

## Decisive end-to-end tests

1. Save/load and Play restore preserve remapped references; successful stepping changes diagnostics but not authoring blobs.
2. Pause preserves values; unrelated edits preserve state; semantic edits reset all cells immediately.
3. Duplicate links/targets, stale/disabled references, invalid phase combinations, NaN, and each cap-plus-one clear rows atomically; repeated unchanged NaN does not rebuild.
4. Inject step refusal: every cell retains committed values, labeled refused.
5. Smoke test acknowledges a step, then consumes diagnostics matching its host tick/session—not the next available packet.
6. Reject truncated, oversized, duplicate-ID, and nonfinite packets; test old scenes and unknown-message peers.
