# Editor inspectors — reflection-driven property editing (M9.4)

*How the editor turns an entity's opaque component bytes into editable, typed fields — and edits
back into the live world — without a line of per-component code.*

This is the cash-in of a bet made all the way back in ADR-0018 §4 ("register once ⇒ inspectable at
M9"): a component registered with `RIME_REFLECT` is snapshot, streamed, **and edited** for free. It
builds directly on [reflection.md](reflection.md) (the `TypeInfo`/`Field` model and the packed
serialization) and the editor channel from [graphics-streaming.md](graphics-streaming.md) /
[scene-format.md](scene-format.md).

## The problem

The snapshot the engine sends the editor (ADR-0016/0031) carries each component as a **type_hash +
opaque blob** — the reflection-serialized bytes. That is enough to *label* a component (m9.3 showed
its name and size), but not to *edit* it: to draw a drag-number for `translation.x` and send the
change back, the editor must know the component's **field layout** — the names, primitive kinds, and
order of its members, recursively through nested value types (`LocalTransform` → `Transform` →
`Vec3`/`Quat`). The editor is a separate Rust process with no access to the C++ `TypeInfo`, so the
layout has to travel over the wire.

## The schema, grown up (RSM2)

The `Schema` message stops being a name table and becomes a **reflected-type dictionary**. The engine
walks every registered component's `TypeInfo` and, transitively, every nested `Struct` field's type,
collecting a deduplicated, `type_hash`-keyed set — the components **and** the value types they
contain. Each entry carries its full field layout:

```text
Schema : [magic 'RSM2':u32][type_count:u32] then per type
         [type_hash:u64][name_len:u16][name...][is_component:u8][field_count:u16] then per field
         [name_len:u16][name...][kind:u8][nested_hash:u64]
```

- `kind` is the engine's `core::FieldType` tag (`Bool=0, Int32=1, UInt32=2, Int64=3, UInt64=4,
  Float=5, Double=6, Struct=7`) — declared order is the wire contract.
- `nested_hash` is 0 for a primitive, or the `type_hash` of the type a `Struct` field recurses into.
  Nesting is expressed by **reference**, not inlining, so a `Vec3` shared by `translation` and `scale`
  is described once.
- `is_component` marks a top-level registered component (vs. a nested-only value type). The editor's
  "add component" menu lists only these — you never "add a bare `Vec3`".

Because everything is keyed by the stable `type_hash`, a snapshot's component blob is decoded by
looking its type up in the dictionary and walking the fields.

## The value codec (`rime-protocol`)

The Rust side pairs a blob with its type descriptor and decodes it into a typed tree — the exact
inverse of the C++ packed serialization (little-endian, fields in declared order, structs inlined
recursively, no padding):

```
decode_value(&schema, type_hash, blob) -> Value      // Value::{Bool,I32,U32,I64,U64,F32,F64,Struct}
encode_value(&Value)                   -> Vec<u8>     // re-encode after an edit; no schema needed
```

The cross-language conformance test is what keeps the two honest: the C++ engine emits golden
component blobs; the Rust codec must decode each to a typed tree **and re-encode the identical bytes**.
Drift on either side is a red test, not a silent field-misread. The inspector renders one widget per
scalar (drag-number for ints/floats, checkbox for bool) and a collapsing section per nested struct;
on change it re-encodes the tree and issues a `SetComponent`.

## Edits are commands (undo, and the honest proof)

Every mutation the UI makes is a typed `Command` (`SetComponent`, `AddComponent`, `RemoveComponent`,
`Spawn`, `Despawn`, `RequestSnapshot`), not a scattered `send_editor` call. Routing everything through
one place buys three things:

1. **Undo/redo.** A component edit has an exact inverse — set-to-new undoes to set-to-old; add undoes
   to remove; remove undoes to set-the-old-bytes-back — so the undoable commands go on a stack as
   `{forward, inverse}` pairs. A whole drag is **one** undo step: the gesture start captures the
   pre-edit bytes, the intermediate frames stream live edits, and the release pushes a single record.
   (Entity spawn/despawn are deliberately *not* undoable in v1 — the engine assigns a fresh handle on
   spawn, so a naive undo can't reproduce the identities other components reference. A spawn/despawn
   history is a later brick.)
2. **A headless proof.** The same commands the inspector issues are what a scripted client issues, so
   `editor --smoke` decodes a real component through the schema, edits a scalar, sends the
   `SetComponent`, asks for a fresh snapshot, and confirms the field changed on the **live engine** —
   the entire reflection-driven loop, GPU-free, on every CI OS. The UI itself stays Mac-eyeballed
   (ADR-0031); the wire underneath it is CI-proven.
3. **Two hosts, one behaviour.** The world-mutating helpers (`apply_set_component`,
   `add_default_component`, `remove_component`) live in `editorhost` and are shared by the GPU-free
   channel host and the viewport render thread, so an edit behaves identically whether or not a
   viewport is streaming.

`AddComponent` is its own message, not a `SetComponent` with a zeroed blob, precisely so the engine
**default-constructs** the component — a zeroed `Transform` would have scale 0. The editor learns the
real defaults from the next snapshot.

## The mirror, and why there is no echo storm

While editing, nothing but the editor mutates the world (that changes at Play — m9.7), so the editor
keeps an **optimistic mirror**: a value edit patches the local snapshot immediately, so the inspector
shows the new value the same frame without a round-trip. Structural changes (add/remove/spawn/despawn)
re-sync by asking the engine for a fresh snapshot (`RequestSnapshot` → `Snapshot`) — the authoritative
truth, including an added component's real defaults. Because snapshots are only ever sent **on
request**, an edit never races an unsolicited snapshot that would rewrite the field mid-drag — the
classic inspector echo-loop is avoided by construction rather than by sequence-number suppression.

## Deliberately deferred

- **Semantic annotations** (color pickers, ranges, units) need a small reflection-macro extension and
  get their own mini-ADR if adopted; v1 covers the reflected primitive + math-type set.
- **Hierarchy in the outliner** (a `Parent` tree + reparent-drag) and **row virtualization** — the
  outliner is a flat list this brick.
- **Multi-select** — single selection only (the m9.0 recommendation).
- **A live delta channel** — unnecessary until something other than the editor moves the world.
