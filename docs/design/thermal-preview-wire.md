# Optional editor thermal diagnostics v1

Engine → editor `ThermalPreview` (`0x020A`) follows the matching viewport frame's
`PlayState`. Unknown message codes remain ignorable; this does not change the
streaming handshake version. The host emits while an enabled graph exists and
one clear message when it disappears/Stop occurs. Headless authoring still uses
ordinary schema, snapshot and SetComponent; it has no numerical preview ticks.

All integers and IEEE binary64 doubles are little endian, without padding.

| Offset | Field | Type |
| --- | --- | --- |
| 0 | Version (1) | u8 |
| 1 | Preview status | u8 |
| 2 | Play phase: Edit=0, Playing=1, Paused=2 | u8 |
| 3 / 4 / 5 | Field / heat / contact status | three u8 |
| 6 / 14 | Session ID / description revision | two u64 |
| 22 / 30 / 38 | Host tick / accepted host tick / field tick | three u64 |
| 46 | Problem entity EditorId, zero if unavailable | u64 |
| 54 / 58 | Failed substep / descriptor | two u32 |
| 62 | Cell count, at most 16 | u8 |
| 63 onward | EditorId, temperature K, liquid **mass** fraction, energy J | count × (u64, f64, f64, f64) |

Total payload is exactly `63 + 32 * count`, at most 575 bytes. Count/length are
checked before allocating rows. IDs must be nonzero and unique; temperatures
finite/positive, fractions finite in [0,1], energies finite. Unknown version,
status/phase ranges, truncated/extra payload and malformed rows refuse. C++ parses
into a candidate before replacing output. The 95-byte shared fixture under
`tools/rime-protocol/tests/fixtures/thermal_preview.bin` is checked by both
languages; fixture generation is not part of the existing stream fixture writer.

Preview statuses: Empty=0, Ready=1, InvalidDescription=2, CapacityExceeded=3,
StepRefused=4. Empty/invalid/capped states have no rows; Ready requires rows. Edit
requires Empty and all three ticks zero. Accepted host tick cannot exceed host
tick. Rebuild resets field/accepted ticks; it does not rewind host tick. Stop/new
Play resets runtime; each new Play/Step session advances session ID. Revision
advances on reset or changed collected descriptions. These are local correlation
stamps, not persistent/network identities.

Field status values: Accepted=0, InvalidInput=1, UnsupportedEnvironment=2,
SubstepBudgetExceeded=3, ReceiptBudgetExceeded=4, WorkBudgetExceeded=5,
SourceRefused=6, ContactRefused=7, TickLimit=8. Heat statuses range 0–4 and contact
statuses 0–6, matching the numerical public enums. Underlying statuses and failure
indices describe attempted work only, interpreted when their parent refusal is
present. They do not imply that partial energy was committed. Successful rebuilds
reset result diagnostics. Rows after StepRefused describe the retained field;
accepted host tick and field tick disambiguate them from a successful current tick.

The editor clears its previous diagnostic on every PlayState, then accepts a
valid diagnostic only if phase/host tick match and session/revision are current
(session increasing, or equal session with nondecreasing revision). A new session
may have a smaller revision. This uses ordered delivery on the existing local
connection. It is not a replay-safe replicated protocol. The read-only panel never
writes values to authored component blobs, history or scene saves.
