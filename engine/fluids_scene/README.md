# Thermal scene authoring and preview

`rime::fluids_scene` is an optional adapter above ECS and the core-only numerical
`fluids` module. It owns no renderer, editor transport, physics or network state.
Worldkit registers its three reflected components; the viewport editor host owns
one disposable `ThermalPreview`. All component defaults are disabled.

| Component | Authored description |
| --- | --- |
| `ThermalCell` | Fixed mass in kg, initial temperature in kelvin, initial liquid **mass** fraction, complete material coefficients/domain, enabled |
| `ThermalContact` | Two reflected entity references, nonnegative conductance in W/K, enabled |
| `ThermalSource` | Target cell reference, signed constant power in watts, enabled |

At the melting temperature, both ice and liquid are possible: provide the initial
fraction explicitly. Below it the fraction must be zero; above it, one. Negative
power removes heat. The default fresh-water material admits 253.15–313.15 K;
limits refuse an update rather than clip energy. Source heat is explicit; existing
cosmetic fire does not automatically heat these cells.

## Author and inspect

Add these components through the ordinary inspector. Enable cells first, point
contacts/sources at their entity IDs through the reflected reference field, then
enable those descriptions. References use existing session EditorIds on the wire
and scene-local IDs on disk. Do not copy runtime entity index/generation values
into scene files. The generic inspector supports typed fields and edit history;
this brick does not add a dedicated graph canvas or an entity-picker widget.

Press Play or Step. Select a cell to read temperature, liquid mass percentage and
energy in the **Thermal preview** inspector panel. Pause stops time. A relevant
settings edit, including disabling an enabled link/source, rebuilds **every cell** from its
authored initial state, even while paused. Unrelated transform/selection edits
and changes to already disabled descriptions preserve live state. Stop discards
the preview; the next Play reconstructs it. Save contains descriptions only.

Malformed typed settings remain editable/undoable; the preview clears its old
rows and reports an invalid graph. Missing, disabled, self or stale-generation
cell references, duplicate undirected contacts and multiple sources targeting
one cell refuse the graph. At most 16 enabled cells, 32 contacts and 16 sources
are admitted. Exceeding any cap refuses the **whole** graph. Collection uses
bounded storage but scans the matching world components; it is not a constant
work guarantee for arbitrarily large authored worlds.

Numerical failures differ from invalid descriptions: they retain the last
committed cells and field tick, visibly marked refused. Host ticks continue;
accepted-host tick identifies the last thermal commit. There are no hidden
retries. The adapter accepts positive finite dt up to .25 seconds and increasing
accepted host ticks. Solver substep/work budgets can still refuse an admitted
graph. Initialization requires the numerical module's supported floating-point
mode; a failed initialization is retried after an edit/reset.

Description comparisons use all explicit double bits and generation-safe
references, never struct padding/hash equality. The same invalid NaN payload
stays stable; changing a NaN payload or signed zero rebuilds. Ordering follows
local entity handles, not a cross-machine identity contract. This adapter and its
world are owned by one thread, as in the current editor host.

## Reproduce the live proof

From the configured checkout (use an absolute persistent save path):

```sh
cmake --build --preset release -j 3
(cd tools && cargo build -p editor)
tools/target/debug/editor --smoke --thermal-sources --engine build/release/bin/rime-engine --save /path/to/thermal.rscene
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 tools/target/debug/editor --smoke --frames 3 --thermal-reload --scene /path/to/thermal.rscene --engine build/release/bin/rime-engine
```

The smoke creates two fixed cells, a contact and an explicit source; checks inert
defaults and typed edit/inverse/redo snapshots; then verifies 12 melting and 12
freezing steps, Pause, disabled-source sensitivity, numerical refusal, invalid
settings clearing, Stop and fresh Play. Reload is a separate host process.
`--thermal-sources` can also run with `--frames` to author in the viewport host.
The [retained verification](../../docs/verification/thermal-authoring-2026-10-18/README.md)
records what actually ran. GUI tests prove the panel is read-only; the smoke
proves real engine semantics. A clicked thermal graph creation/save/reopen proof
is still separate work.

This is a fixed-cell numerical/readout preview. There is no ice surface,
thickness, collision, expansion, advection, moving-basin freezing, combustion
spread or replicated authority. The next visual consumer must derive its
presentation from these validated states without interpreting liquid **mass**
fraction as a geometric thickness. See [ADR-0094](../../docs/adr/0094-thermal-editor-preview.md).
