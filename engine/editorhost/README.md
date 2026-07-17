# rime_editorhost — the engine-side editor host (M9 / m9.1)

The engine half of Rime's **editor-as-a-client** architecture ([ADR-0016](../../docs/adr/0016-editor-is-a-client-of-the-engine.md),
[ADR-0031](../../docs/adr/0031-editor-v1.md)). The editor is a separate process (a Rust shell, M9.3);
this module serves the **editor channel** against a live `ecs::World` over the S1
`stream::ProtocolConnection` — so the editor sees and edits the world without ever linking engine
internals.

Everything here is **reflection-driven**: it walks the world through the `ComponentRegistry`'s
`TypeInfo`, so a component registered once (`RIME_REFLECT`) is snapshot, streamed, and edited with
**zero code in this module** — the "register once ⇒ inspectable at M9" bet of
[ADR-0018 §4](../../docs/adr/0018-ecs-storage-model.md), finally cashed.

## What it does

Two layers:

1. **The reusable reflection core** (no wire) — the machinery m9.2's scene format and m9.7's play
   snapshot/restore also build on:
   - `serialize_world(world)` / `deserialize_world(dst, bytes)` — the whole world ↔ a self-describing
     blob. Each component is keyed by its stable **`type_hash`** (not its registration-order
     `ComponentId`), so a blob survives a different registration order between two worlds.
   - `serialize_schema(world)` — the component registry (`type_hash` + name) the editor uses to label
     inspectors and gate compatibility.
   - `apply_set_component(world, entity, type_hash, blob)` — deserialize one component onto an entity
     (adding it if absent), stamping it changed. Only registered, reflected types are editable.
2. **`EditorHost`** — that core over a `ProtocolConnection`: `send_hello()` (schema + snapshot),
   then `poll_one()` drains and applies the client's edits (set-component / spawn / despawn) **at a
   tick boundary** (the ECS structural-change rule). Message types live in the reserved `0x02xx`
   protocol band M6.9 carved out for exactly this.

## Boundaries

- **Removable** (guardrail 2): nothing in the runtime engine depends on `editorhost`; the engine
  builds with it gone. Depends only on `ecs`, `stream`, and `core`.
- **Headless-provable.** Every behaviour is exercised by `tests/editorhost` with no UI — including a
  full loopback over the s1.4 local socket (the editor's real wire). The Rust shell's UI is
  Mac-eyeballed (M9.3), the honest CI-vs-display split.
- **Same-machine v1.** Cross-machine editing (endianness/versioning polish) and change-detection
  **deltas** (streaming only what changed each tick, over ADR-0018 §4's per-column versions) are the
  next increments; v1 sends a full snapshot and applies edits.
