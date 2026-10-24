# Handoff — thermal editor authoring — 2026-10-18

User requested continued water/smoke/fire/ice and the editor pipeline, prioritizing
freezing/melting. During delivery they explicitly requested a working city/new
scene demo and a good day-end exit point. **Continue to that demo after this
checkpoint; this authoring brick alone does not satisfy the latest request.**

Branch `feat/thermal-editor-authoring`; implementation `74cd474`.
Parent `feat/bounded-thermal-field` c1ca901f2210a55f09097753738d1f66b6e1b1fe,
draft #339. Nothing merged.

## Implemented and verified

New removable `fluids_scene` adapter: disabled-by-default reflected ThermalCell,
ThermalContact, ThermalSource; existing reflected entity references/remapping;
whole graph caps 16/32/16. Relevant enabled-description changes rebuild all cells
from initial values, including Pause. Unchanged descriptions retain state. Invalid
descriptions clear stale values; numerical refusal retains last committed values.
Stop clears; new Play reconstructs. Defaults create no diagnostic stream.

Optional bounded read-only 0x020A diagnostics with session/revision/host/accepted
host/field ticks, problem EditorId and numerical refusal statuses. C++ and Rust
exact-length/finite/unique/count validation share a 95-byte fixture. Inspector
shows selected-cell K / liquid MASS percentage / J and human status; no authoring
commands or solver buffers in save/undo. Typed invalid values stay editable.

SDK config now discovers AV1 imported targets missed by the previous render-only
consumer. Repository SDK smoke actually runs thermal API + codec from install.
ADR0094, module README and protocol docs describe exact semantics/limits.

Boundary results in `docs/verification/thermal-authoring-2026-10-18/README.md`:
- Full build pass; adapter/codec 9 cases395assertions pass, same ASan/UBSan.
- Actual-flag Clang preview/codec/test/host/worldkit pass. Remaining engine static
  dependencies in the sanitizer run are normal release libraries; no whole-engine claim.
- Full Rust workspace pass (4 pre-existing ignored); GUI122 pass; exact default
  and GUI Clippy pass, format/license/whitespace pass. GUI lint placement fixed.
- Real headless author/inverse/redo/save; fresh viewport reload8entities19components
  zero skipped; 12 heating vs12 source-disabled steps: fractions .300448/.000598.
  12 cooling steps give .700748 from liquid1, all at273.15K. Pause/invalid edit,
  numerical out-of-range refusal, Stop and fresh Play proven. Host37accepted1refused.
- Combined smoke/fire+thermal authoring/save pass.
- Full lavapipe CTest **89/91**, exit8,287.76s. Inherited three block collapse/locality
  claims plus ambient-display window input key-up failure. Isolated unchanged
  binary `DISPLAY=:99` recheck1/1pass. Exact display-sensitive cause not established;
  do not call full suite green. Actual parent #339 Linux and ASan logs only fail
  same block claims; parent SDK/TSan/lint pass, hosted checks skipped.
- SDK smoke + separate installed thermal/codec consumer pass. Final hashes retained.

One new narrow tool-free Astra design consultation, response and exact gateway
usage delta $0.04958. Adopted whole graph validation/correlation; rejected float
canonicalization for explicit serialized-bit comparison; bounded lowest-key scan
instead of early cap stop. Prior Fable research reused, no new call. Root reviewed
all hunks. Private account/credentials excluded. Live shared gateway cap is lower
than user $7 ceiling; check before another premium call.

## Continue for latest user request

Build a small city courtyard/thermal demonstration scene with visible validated
phase, explicit heater/cooler and separate smoke/fire plume. Prefer existing
renderer/material and host seams, no false ice collision/flow/combustion claims.
Need an actual launch command, saved scene, screenshot/frame proof, Play/Pause/
Stop and reload, clear explanation/labels. Make it usable at day end, not merely
a numerical report. New API/presentation design gets narrow Astra consultation
under existing authorization and local verification, not implementation delegation.

M18 perf (parent sim.block p998.074>6), city convergence, water displacement and
transport, ice thickness/collision, authoritative fire, export/two-client/late join
and clicked asset proofs remain open. No benchmarks beside CI/builds/tests.

Scratch `/home/next/reports/rime-thermal-authoring-2026-10-18/`. Actual global rules
`/home/next/.claude-switch/profiles/default/CLAUDE.md`, project CLAUDE/VISION/roadmaps,
brick-delivery skill. Shared runner reserve18GB free, minimum15GB. Old untracked
notes belong to others; never add-all/delete/reset. Explicit staging, own branch
per brick, pinned format exact find set, self-review every hunk, REST draft PR.
