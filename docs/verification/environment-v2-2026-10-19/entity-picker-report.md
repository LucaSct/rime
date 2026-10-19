# Thermal entity reference picker

Commit: `d47f9b8` (`feat/editor): pick thermal references by readable entity labels`)
Branch/worktree: `feat/entity-reference-picker`, `/home/next/projects/rime-wt/entity-reference-picker`.
Base: exact `b5bf53b`. No main-checkout changes, C++/protocol/schema changes, scene edits, push, or merge.

Implemented the bounded next editor brick: ThermalSource.cell and ThermalContact.first/second now offer readable Outliner labels plus exact stable EditorIds. Choices contain ThermalCell entities and explicit None (wire id 0). The current missing/external or incompatible target remains visible without automatic retargeting. The ordinary exact u64 text editor remains available below each picker. A strict component/field allowlist and reflected Entity/U64 shape guard keep other integers and unknown reference types on their original editor.

Selection uses the existing component encoder, SetComponent command and exact inverse undo path. Engine handle generations and reference resolution remain engine-owned. Standard egui keyboard focus, Enter and Escape work; the combo accessibility label includes its field and current target.

Measured verification:

- `CARGO_TARGET_DIR=/home/next/projects/rime/tools/target CARGO_INCREMENTAL=0 cargo test -p editor --features gui entity_picker::tests -- --nocapture`: **7 passed**, 0 failed, 134 filtered out. Latest compile 1.32s; tests 0.01s; exit 0.
- Focused rustfmt check on gui.rs and gui/entity_picker.rs: exit 0.
- `git diff --check`: exit 0.

Tests cover actual dropdown selection, unchanged companion field bytes, source wire bytes, one undo/redo step, independent contact endpoints, keyboard open/cancel, missing target preservation/clear, exact >2^53 numeric entry, incompatible candidate filtering, and strict schema/allowlist fallback.

Limits: no real engine/GPU/window proof or complete GUI suite was run, per parallel targeted-only instructions. Labels are existing derived Outliner names; persistent authored naming remains future work. Candidate compatibility checks the ThermalCell component, while the engine validates live references and thermal simulation configuration. Numeric missing/external ids can still be refused by the engine, as before; the UI neither fabricates nor resolves handles. No prefab library.
