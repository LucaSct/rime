# ADR-0031: Editor v1 architecture — a Rust shell that is a client of a live engine

- Status: Accepted
- Date: 2026-07-17

## Context

Milestone 9 builds Rime's first editor. [ADR-0016](0016-editor-is-a-client-of-the-engine.md) already
set the *strategy* — **the editor is a client of a live engine process**, not a monolith with the
engine linked in — and the roadmap's bet is that by M9 this is **assembly, not invention**, because
the pieces exist:

- **The wire.** Track **S1 just landed** (this session): `ProtocolConnection` is transport-generic
  over a `platform::ByteStream`, and s1.4's **local fast path** (`LocalSocket`, UDS, LZ4-lossless) is
  exactly the low-latency, pixel-exact viewport transport an editor wants.
- **Reflection + type hashes** (M6): a component registered once serializes to bytes and carries a
  schema hash — the inspector's field data and the compatibility gate.
- **ECS change detection** (ADR-0018 §4, landed at M7.6): `chunk.hpp`'s `column_version()` /
  `mark_changed()` — the header comment already names *"the editor's live inspector sync (M9)"*.
- **The reserved channel** (M6.9): `MessageType::EditorReservedBegin = 0x0200 … EditorReservedEnd =
  0x02FF` in `protocol.hpp`, with a documented "bump `kProtocolVersion` when the editor lands" rule.
- **Cooked-asset manifest** (M6), the **fixed tick** (M5.7/ADR-0023) for deterministic play, and the
  **viewport toolkit patterns** (camera, picking, UI) graduated from the ICEM viewer.

This ADR settles the concrete shape the M9 bricks (m9.1–m9.8) all cite: the process model, the editor
channel, the Rust UI toolkit, the edit/play world model, undo/redo, and what the viewport *is*.

Like the other `.0` decision bricks (ADR-0024/0026/0029/0030), this is a **decision brick: proof is
the ADR, no engine code**. One scoping note on the toolkit "spike" the plan imagined (§3): the fps
comparison it wanted (a fake viewport + a 10k-row tree) is a *UI* measurement, and this development
box is **headless** — it has no display and its numbers would not reflect a real editor machine
anyway. Consistent with the plan's own honest split (*the editor UI is Mac-eyeballed; the command
layer is what CI proves*) and the house rule *don't fake a proof*, the toolkit is **decided here on
analysis** and its perf is **validated in m9.3**, where the toolkit is integrated for real on a
machine with a display — not in a throwaway spike that would duplicate m9.3's shell.

## Decision

### 1. Process model: the editor launches the engine as a child

The editor **launches the engine as a child process** (`rime-engine --editor-host <session>`) by
default, and **attach-to-a-running-engine** is kept as a mode — it is the *same* handshake, inherited
from the dev-streaming heritage (a client connecting to a server). This gives **crash isolation** for
free: an engine crash is a dead child the editor detects (EOF on the channel, child exit code), not a
dead editor. The editor shows a "engine stopped — relaunch?" state and can respawn, preserving the
open `.rscene` (m9.2) so a crash costs a relaunch, not the work. The child is spawned with the local
transport (§6): a per-session socket path the editor picks and passes to the child.

### 2. The editor channel: message families on the reserved 0x02xx range

The editor's traffic is new `MessageType` values inside the reserved `[0x0200, 0x02FF]` band, carried
by the *same* `ProtocolConnection` as frames/input. `kProtocolVersion` bumps (4) so an editor only
speaks 0x02xx to a new-enough engine — the reservation's forward-compat rule. The families, each with
its **consuming brick named** (the plan's discipline — no message without a consumer):

| Family | Purpose | Lands in |
|---|---|---|
| **Hello / capabilities** | schema-hash exchange — the engine sends its registered component **type hashes**; a mismatch is reported, not mis-decoded | m9.1 |
| **Entity-tree sync** | initial world **snapshot** (entities + reflection-serialized components) then per-tick **deltas** driven by change-detection versions | m9.1 |
| **Component get/set** | a component as **reflection bytes + type hash**; set is queued, applied at the ECS `CommandBuffer` boundary | m9.1, m9.4 |
| **Spawn / despawn** | structural edits, likewise queued to the command-buffer boundary | m9.1, m9.4 |
| **Asset manifest** | list the cook manifest (kind, name, `AssetId`) for the browser | m9.5 |
| **Scene ops** | save / load a `.rscene` path | m9.2 |
| **Selection / pick** | a viewport pick request → entity id (engine-side ID-buffer pass) | m9.6 |
| **Gizmo transaction** | begin / delta / end of a drag → one undo command; engine applies to `LocalTransform` | m9.6 |
| **Play control** | edit / play / pause / step state machine | m9.7 |
| **Log / stats feed** | engine log ring + `WorldStats` → a status panel | m9.3+ |

Payloads are reflection's existing little-endian byte round-trip + a type hash, matching the S-protocol
field-by-field discipline (never memcpy a struct). Same-machine v1; cross-machine editing (endianness,
versioning polish) is a documented later seam.

### 3. The Rust UI toolkit is **egui + egui_dock**

Decided on analysis; the criteria and why egui wins:

- **Immediate-mode fits live data.** The outliner and inspectors re-render from the world *mirror*
  every frame; an immediate-mode toolkit (rebuild the UI from current state each frame) matches that
  with no retained widget-tree to keep in sync with a mutating world — the single biggest ergonomic
  win for an editor whose data changes under it.
- **Docking exists** (`egui_dock`) — the multi-panel layout M9 wants, without building a docking
  system (the yak-shave §Risks warns against).
- **License is clean:** egui/egui_dock are **MIT OR Apache-2.0** — ship-safe for an open project,
  unlike a copyleft-defaulted toolkit.
- **Viewport integration is a solved path:** egui draws a registered GPU texture in a panel
  (`egui::TextureId` via the backend), so a decoded stream frame (LZ4 → RGBA, s1.4) becomes a
  texture in the viewport dock; `eframe` owns the window + a wgpu/glow backend.
- **Dependency surface is acceptable** because the editor is a *tool*, not the shipped engine: egui
  pulling wgpu/winit never touches the C++ engine or its RHI. Actively maintained, widely used for
  exactly this class of tool.

Perf validation (a fake viewport at editor-panel size + a virtualized 10k-row tree, fps recorded)
is **m9.3**'s, on a display — see §Context. If egui there proves unsuitable, the shell is thin enough
to swap; the *engine-side* host (§below) is toolkit-agnostic and unaffected.

### 4. Edit vs play: snapshot → play → restore, bit-exact

**Edit mode:** the simulation schedule is **paused**; the editor host applies edits (set/spawn/despawn)
directly at tick boundaries. **Play** snapshots the whole world — reflection-serialization of every
component, m9.2's machinery, to *memory* not disk — then runs the fixed-tick sim (ADR-0023) live;
**Stop restores the snapshot bit-exactly** (rebuilt via `CommandBuffer`, asset handles kept warm — no
reload storm). Determinism (ADR-0023's fixed tick) makes **step** honest: N single-steps equal one
N-tick run. The real engineering (m9.7) is that engine **side-tables** (physics bodies, the M8
destruction SoA) must be **reconstructible from components** on restore — a rule this ADR sets and
m9.7 enforces.

### 5. Undo/redo: a command over the channel's mutating messages

Every mutation is a **command** (set-component, spawn, despawn, a gizmo drag = one begin/end
transaction) with its inverse; the editor keeps a v1 **linear history** (undo/redo stack). This lives
**editor-side**, over the channel — the engine host stays a stateless applier of queued edits, which
keeps the headless command-layer test (below) the source of truth. Undo beyond
component-edit/spawn/despawn is a non-goal (ADR-0016 scope).

### 6. The viewport is the s1.4 local session

The viewport panel is a **local session** (§ADR-0016) over s1.4's `LocalSocket` — **LZ4-lossless**
(pixel-exact; an editor must not show codec artifacts), at **editor-panel resolution**. A panel
**resize renegotiates** the stream geometry (StreamConfig, s1.2's message); input focus follows the
panel (key/mouse routed to the engine only while the viewport holds focus). Same-host, so latency is
near-zero (s1.4's whole point).

### Engine-side host + the design invariants

A small engine module, **`engine/editorhost`** (m9.1), serves the channel against a live `World`:
snapshot + change-detection deltas out, queued edits in at the `CommandBuffer` boundary, schema
handshake, log/stats. The invariants every M9 brick holds:

- **The editor never links engine internals** — protocol + files + (optionally) the M6.9 C-ABI only.
- **Engine crash ≠ editor crash** (§1 child isolation; reconnect/relaunch).
- **Inspectors are *generated* from reflection** — a newly registered component is editable with
  **zero editor code**.
- **A headless command layer.** Every editor-host behaviour is provable by a **protocol-driven test
  with no UI** (the UI calls the same internal command layer the tests drive). The Rust shell's UI
  itself is Mac-eyeballed — the honest CI-vs-display split, stated per brick.

## Consequences

- **+** M9 is assembly: the wire (S1), reflection/change-detection (M4/M6), the manifest (M6), and the
  fixed tick (M5.7) are all consumed end-to-end by a thin Rust shell — no new engine subsystems.
- **+** Crash isolation and "a new component is editable for free" fall out of the architecture, not
  bespoke code.
- **+** The command-layer/UI split keeps M9 **CI-provable headless** despite being a GUI milestone.
- **+** One protocol now carries dev-streaming, remote play, *and* the editor — the S-track bet paid.
- **−** A Rust UI dependency (egui/eframe → wgpu/winit) enters `tools/` — contained there, never in
  the shipped engine, but real build surface for the editor.
- **−** The toolkit fps is **not** CI-measured (headless box); it is validated on a display in m9.3,
  an honest deferral, not a skipped proof.
- **−** `kProtocolVersion` bumps again (→4): an editor and engine must match — intended, gated at the
  handshake.
- **−** Play-restore fidelity for side-tables (physics/destruction) is real engineering pushed to m9.7
  (the "reconstructible from components" rule this ADR sets).

## Alternatives considered

- **iced** (Elm-style, retained) — a fine toolkit, but the retained/message-update model is more
  ceremony for data that changes *under* the UI every tick, and its docking story is less mature than
  egui_dock. Immediate-mode is the better fit for a live editor.
- **Slint** — excellent for polished product UIs, but it is markup-declarative (heavier for a dev
  tool that is mostly generated-from-reflection panels) and its licensing is dual **GPL/commercial** by
  default — a poor fit for a permissively-licensed open project. egui's MIT/Apache is the clean choice.
- **Custom UI on wgpu** — the yak-shave the plan explicitly warns against. The shell is a *tool*, not
  the product; egui-class maturity wins, and building a UI framework would dwarf the actual M9 work.
- **Editor links the engine directly (monolith)** — rejected long ago by ADR-0016: it forfeits crash
  isolation, forces the editor onto C++, and couples the tool to engine internals. The client model is
  the whole bet, and S1 just made its wire real.
- **A full toolkit spike in m9.0** — deferred (not cancelled): a real fps number needs a display this
  box lacks and would duplicate m9.3's shell. Deciding on analysis now and validating in m9.3 is the
  honest, non-throwaway path.
