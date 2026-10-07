# ADR-0075 (E1): The editor names entities by EditorId, and Stop restores the world in place

- Status: Accepted (number provisional — renumbered at merge if it clashes)
- Date: 2026-10-06
- Supersedes: the restore half of ADR-0031 §4 (m9.7's "despawn everything, rebuild from the
  snapshot"); amends the editor wire of ADR-0031 and the m9.4 command layer

## Context

The editor click tests and the Xvfb smoke (`scripts/editor-click-smoke.sh`) measured three defects
that share one cause:

1. **Play→Stop corrupted the scene.** `PlaySession::stop` despawned every entity and rebuilt the
   world from a snapshot. Every entity came back under a new `(index, generation)` handle, and no
   handle stored *inside* a component was rewritten, so every `ecs::Parent` pointed at a dead
   entity and the next save wrote it as `null`. Playing once and saving deleted the hierarchy.
2. **Undo died after any Play/Stop.** The undo history named entities by handle, and (1) replaced
   all of them.
3. **Spawn, despawn, place, add-component and remove-component were not undoable.** For spawn and
   despawn there was no way to express the inverse: the engine assigns a fresh handle on every
   spawn, so "bring back the entity I despawned" had no name to bring it back under.

The cause is that the editor had no stable name for an entity. `ecs::Entity` is the right handle for
the *engine*: a despawn bumps the slot's generation, so every stale copy of the handle is
detectably dead forever. That is exactly what makes it the wrong name for an *editor*, whose
history and selection must outlive the operations that kill handles.

## Decision

### 1. A stable identity, owned by the editor host

`editorhost::EditorId { u64 value }` is a reflected component the host stamps on every entity the
editor can see — on scene load, on spawn, on place — from a per-session counter that only goes up.
An id is never reused, not even after a despawn; `0` is never issued (it is the wire's null) and
neither is `~0` (the wire's "dangling"). `EditorIds` is the registry: the counter, plus a cache from
id to the handle that holds it which is verified against the world on every hit and rebuilt on a
miss, so it cannot answer with a wrong handle after a Stop or a game-side despawn.

**On load, ids are assigned in file order**, by ascending slot index: the scene loader spawns one
entity per record into an empty directory, so slot order *is* record order. The registry that
serves the editor later moves its counter past any id already present.

**EditorId is not written into `.rscene` (the orchestrator's default, kept).** It is a session
identity: the host reassigns it deterministically from the file's own order on every load, so
persisting it would only bake one session's numbering into a document that already carries its
order — and would need a collision story for merged or hand-edited files that the reassignment does
not. The type is marked with a new ECS trait, `ecs::kSessionComponent<T>`, which the scene writer
honours on every entity. Unlike `kDerivedComponent` it is unconditional: a derived component can
also be authored (a hand-written `MeshRef`), a session component never is. The editor does not
depend on this choice — it only ever sees ids on the wire.

### 2. Everything on the editor wire names entities by id; the host resolves

Every editor→engine message that names an entity carries its EditorId: `SetComponent`,
`AddComponent`, `RemoveComponent`, `Despawn`, `Spawn`, `SpawnEntity` and `GizmoState`. **So does
every entity reference inside a component**: on the editor wire an `ecs::Entity` field holds the
referenced entity's EditorId in the same eight bytes (low half in `index`, high half in
`generation`), and the schema describes `Entity` as one `editor_id: u64` field. The host translates
at the boundary, both ways, through reflection — exactly what the scene format does with its `@N`
scene-local ids. Snapshots are `RSN2`: each entity record carries its id after its handle, in
creation (id) order; the handle stays only because `PickResult` speaks handles.

`apply_editor_edit` is the one dispatcher both hosts (GPU-free channel, viewport) use. An id or a
reference it cannot resolve is **refused**: nothing applied, a counter bumped
(`IdentityCounters::unresolved_ids`, `unresolved_refs`, `refused_adoptions`), a named error logged.
A non-null reference is never silently turned into null. A respawn under an explicit id is accepted
only for an id this session issued that no live entity holds.

The structural edits (`Spawn`, `SpawnEntity`, `Despawn`, `AddComponent`, `RemoveComponent`) are
answered with a minimal `EditResult { ok: u8, editor_id: u64 }` (0x0208), in arrival order. The
snapshot round trip alone could not do this job: it cannot say that an edit was *refused*, and a
spawn's history step needs the id the engine assigned. `SetComponent` is not answered — it is the
high-rate message (every frame of a drag), and its history step is the gesture.

The handshake's `PROTOCOL_VERSION` is not bumped: the editor launches the engine it talks to, the
snapshot magic bump (`RSN1`→`RSN2`) makes a mismatched pair fail loudly at the first snapshot, and
the browser client that shares the handshake speaks none of these messages.

### 3. Stop reconciles in place

`PlaySession` keeps an in-memory baseline per entity: its handle, its EditorId, its full component
signature (reflected and not), and its reflected component bytes. `stop()`:

- despawns every entity the baseline does not name (spawned during play);
- keeps every baseline entity that is still alive **under its handle**, removes what play added
  (a `RigidBodyHandle`), re-adds what play removed, and restores every reflected value;
- respawns every baseline entity play destroyed **under a fresh handle** with its old components,
  EditorId included;
- allocates every missing entity **before** filling any component, then rewrites every Entity
  field through *old handle → EditorId → new handle* (for an entity no host stamped, its old handle
  stands in as the identity, in a separate map), so forward, backward and self references resolve;
- never rewinds a generation and never forces an old handle back into existence;
- counts survivors, newcomers removed, casualties respawned, references remapped, and references
  that were already dangling before play (kept exactly, logged, never nulled) in a `StopReport`.

Derived state is rebuilt by the caller as before: `derive_world_transforms`, then a fresh
`PhysicsWorld`/`PhysicsSync` on the next Play.

Two consequences had to follow so that "Stop restores the scene" holds byte for byte:

- **Saves are written in editor order.** The writer's default order follows archetype layout, and
  an entity that gained and lost a component during play returns to a different row, which would
  renumber the file. `save_scene_to_string` gained an optional entity order; the host passes
  `entities_in_editor_order` (by id). Since ids follow file order on load, an untouched
  open→save is still byte-identical, and a placed entity is still numbered last.
- **`world_content_hash` is layout- and identity-independent.** Per-entity digests (with Entity
  fields hashed as the referent's EditorId) are sorted before folding, so a survivor that moved
  rows, or a casualty with a new handle, hashes the same.

### 4. Undo stays in the editor; every structural edit records its own inverse

There is no host-side undo stack. The editor's `CommandStack` holds `Edit { forward: Vec<Command>,
inverse: Vec<Command> }`, all by id:

| edit | inverse |
|---|---|
| spawn / place | despawn the id; redo respawns **under the same id** |
| despawn | an exact `SpawnEntity` under the old id with its full component set, then a `SetComponent` per inbound reference |
| add component | remove it |
| remove component | set its captured bytes back (which re-adds it) |
| set component | set the previous bytes |

**Despawn nulls inbound references (the orchestrator's default, kept).** A dangling handle left in
a `Parent` is invisible state: it saves as `null` anyway, snapshots as "dangling", and the next
system to read it does something arbitrary. Nulling makes the world honest at the moment of the
edit. The editor captures every referring component from its mirror *before* sending the despawn —
found through the schema (`rime_protocol::entity_refs`), never by scanning bytes — and the undo puts
each one back after the exact respawn, so the restore is exact.

A structural step is committed only when the engine's `EditResult` says ok; a refused edit leaves no
step and its reason is shown on the status line. **The history is frozen while the simulation
runs**: nothing is recorded and undo/redo are refused, because Stop discards whatever play did. The
steps already recorded are untouched by Play→Stop — they name entities by id, and Stop gives every
entity its id back — and the pre-play selection (also an id now, not a row) is restored on Stop.

## The proof

- **C++** (`tests/editorhost/editor_identity_test.cpp`): (1) Stop restores values and membership,
  keeps survivors' handles, removes newcomers, recreates casualties under fresh handles with their
  old id; (2) a `Parent` survives Stop through a respawned parent, a forward reference and a nested
  self-reference, appears on the editor wire as the parent's id, and survives a save + reload with
  no `null`; (3) unresolvable ids, references and adoptions are refused and counted, and a
  pre-play dangling reference is kept and counted; (4) a dead handle stays dead after Stop. Plus the
  despawn → nulled reference → exact undo round trip, and the editor-order save.
- **Falsified**: with the Entity-field rewrite in `stop()` skipped, test (2) fails in both subcases
  (the Parent, the self-reference, the content hash, the wire id, `value null` in the save).
- **Rust click tests**: each former `*_is_not_undoable` pin is a positive undo/redo test (spawn,
  despawn with a child, place, add component, remove component with exact values); undo after
  Play/Stop with a redo branch, while play destroys the edited entity; the freeze; despawn → undo
  → undo the edit before it → redo across slot reuse. The fake engine recycles slots LIFO and
  respawns casualties under new handles, as the real one does. **Falsified**: never freezing the
  history fails the during-play test.
- **The Xvfb smoke**: both pins flipped. Undo and redo work after Play→Stop, and the save after
  Stop is byte-identical (`cmp`) to the save before Play, with its `value @N` Parent reference.

## Consequences

- The scene format's private entity-field detector moved to `ecs/entity_refs.hpp`
  (`is_entity_field`, `for_each_entity_ref`), and the binary paths reuse it. The loader's
  text-token remap itself is not shared: it resolves `@N` tokens during the parse, before any
  component exists in memory, while every editor path rewrites a deserialized object in place —
  the shared piece is the rule for *which* fields are references and the walk that reaches them.
- `deserialize_world` now allocates every record before filling and remaps references to the
  entities it spawned; a reference outside the blob makes it return false instead of quietly
  surviving as a dead handle.
- A component with an Entity field crosses the editor wire through a scratch object (decode,
  rewrite, encode) — negligible at editor rates, and only for types that have such a field.
- **Destruction side-tables** are still not rebuilt by Stop (ADR-0031 §4's open item); nothing here
  changed that.
- An old editor against a new engine fails at the first snapshot (`RSN2`); there is no
  compatibility shim.
