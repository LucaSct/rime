# The `.rscene` scene format — design note (M9 / m9.2)

Companion to `engine/scene/`. A scene is a world you can save to a file and load back: which
entities exist, which components they carry, and how they are placed and parented. `.rscene` is the
**v1, human-diffable text** form of that. ADR-0031 (editor v1) is the umbrella; this note pins the
format.

## Why a text format (and why reflection-driven)

Two forces shape the choice:

- **A scene is authored content.** It lives in git next to the code. So it should **diff cleanly**,
  be **readable and hand-editable**, and **merge** like source. Text wins that; a packed binary blob
  (the m9.1 editor snapshot) does not. The cost — larger files, slower parse — does not matter at
  authoring time. A *cooked* binary scene for shipping is a deliberate future seam (below), not v1.
- **New components must not need new save/load code.** The engine gains components constantly. If
  each needed a bespoke serializer the format would rot. So `.rscene` is **reflection-driven**: it
  walks a component's `TypeInfo` (the M1.7 reflection core), never a concrete struct. Register a
  component once with `RIME_REFLECT` and it saves and loads for free — the same "described once ⇒
  serializable now, inspectable at M9" bet the render components were written for (ADR-0016/0018).

The C++ writer/reader in `engine/scene` is the **reference** implementation of the format. The Rust
editor consumes scenes through files (it never reimplements the byte layout) — the discipline that
keeps the two languages honest across the whole engine.

## The grammar

```
scene   := "rime_scene" <version> entity*
entity  := "entity" <local-id> "{" component* "}"
component := <type-name> <type-hash> "{" field* "}"
field   := <name> value
value   := number | "true" | "false" | struct | entity-ref
struct  := "{" field* "}"
entity-ref := "@" <local-id> | "null"      # only where the field's reflected type is ecs::Entity
```

- **Whitespace is insignificant** — indentation is cosmetic, so the writer's layout and a hand
  author's are equally valid. `#` starts a comment to end of line.
- **`<type-hash>`** is the component's `compute_type_hash` fingerprint, written as `0x` + 16 hex
  digits. It is the versioning key (see below).
- **Numbers** are written by fmt's shortest round-tripping decimal and read back by
  `std::from_chars`, so a `float`/`double` recovers **the exact same bits** — locale-independent, no
  `1.0000001` drift. (A dedicated test asserts this on deliberately awkward values.)
- **All-primitive structs inline** (`translation { x 0 y 1 z 0 }`); a struct that nests another goes
  multi-line. Cosmetic only — the reader does not care.

Full example: `samples/07-first-light/first_light.rscene` (a camera, a sun, a ground mesh, and a prop
parented to the ground).

## Two things the format knows beyond "a struct of fields"

Everything else is generic reflection. Two concepts are load-bearing enough to name:

### Versioning by `type_hash` — schema drift is a clean error

Every component carries its `type_hash`. On load the reader resolves it against the world's
registry. If **no** registered type has that hash but one has the **same name**, that is *schema
drift* — the component gained/lost/reordered a field since the file was written — and it is reported
as `component 'X' schema drift: file hash …, engine … — re-save the scene`. If no type matches by
name either, it is an *unknown type*. Neither is ever a silent skip: a scene that cannot be loaded
faithfully fails loudly, so stale content is caught, not half-applied.

### Entity references remap through scene-local ids

An entity handle (`{index, generation}`) is a *runtime* value — meaningless in another process or
after a reload. But a `Parent` component holds one. So the format assigns every saved entity a
**scene-local id** (its ordinal, `0..N-1`) and writes any entity-reference field as that id (`@2`),
or `null`. On load the reader **spawns every entity first**, building `local-id → fresh handle`, then
fills components — so a reference (forward or backward in the file) resolves to the freshly-spawned
entity. A reference to an id the file never defines is a clean *dangling reference* error.

Detection is reflection-native and needs no hard-coded component list: an entity-reference field is
simply a `Struct` field whose nested type **is** `ecs::Entity` (pointer-identity on its `TypeInfo`).
Reflecting `Entity` and the transform-hierarchy components (`engine/ecs/reflect.hpp`) is what turns
"save a posed, parented world" into zero per-component code.

## What a load does *not* restore

- **`WorldTransform` is derived, not stored.** A scene saves the authored `LocalTransform` and the
  `Parent` edges; the absolute placement is recomputed by `ecs::propagate_transforms` after the load
  (the launcher's `--scene` path does exactly this). Persisting it would just bake a stale duplicate.
- **Unreflected components are skipped.** A component with nothing reflected has no inspectable state
  to author — a documented property, not data loss.
- **Fresh entity handles.** The reconstructed world's entities are new; only the *relationships*
  (via scene-local ids) are preserved, never the raw handles. A load into a world you are willing to
  discard on failure, since a mid-parse error may leave it partially populated.

## Using it

```cpp
#include "rime/scene/scene_format.hpp"

std::string text = scene::save_scene_to_string(world);
scene::LoadReport r = scene::load_scene_from_string(world, text); // register the component set first
scene::save_scene_file(world, "level.rscene");
scene::load_scene_file(world, "level.rscene");
```

Register the components a file names **before** loading — e.g. `ecs::register_transform_components`
+ `render::register_render_components` — exactly as the editor host requires. `rime_hello --scene
<file>` does the whole dance (register → load → propagate → report).

## Future seams (noted, not built)

- **Cooked binary scene.** For shipping, a `.rscene` cooks to a packed binary (fast load, no parse)
  through the M6 asset pipeline — the text stays the authoring source, the binary the runtime form.
- **Asset source-path comments.** A writer with the asset manifest in hand could annotate an
  `AssetId`/registry-id field with `# source: meshes/wall.gltf` for human context. The format already
  permits arbitrary `#` comments; auto-emitting them awaits the manifest coupling.
- **Prefabs / nested scenes.** An entity that instantiates another `.rscene` with an override list —
  the natural next step once the flat format is proven.
