# rime::scene — the `.rscene` scene format (M9 / m9.2)

A world saved to a **human-diffable text file** and read back. Reflection-driven: a component
registered once with `RIME_REFLECT` saves and loads with **zero code here**, keyed by its stable
`type_hash` so a file survives a different component-registration order (the same bet as the m9.1
editor host, now on disk). The C++ writer/reader is the **reference** implementation — the Rust
editor reuses it through files, never by reimplementing the byte layout.

## Why text

A scene is authored and version-controlled. Text diffs cleanly in git, a human can read and hand-edit
it, and a merge is a merge. The trade — larger and slower to parse than the m9.1 binary snapshot —
does not matter at authoring time. A cooked *binary* scene (fast load, shipping content) is a future
seam, noted in [docs/design/scene-format.md](../../docs/design/scene-format.md), not v1.

## The format

```
rime_scene 1                          # header: format version

entity 0 {                            # scene-local id (0..N-1)
  rime::ecs::LocalTransform 0x… {     # component: reflected type name + its type_hash
    value {
      translation { x 0 y 1.5 z 0 }   # all-primitive struct → one line
      rotation { x 0 y 0 z 0 w 1 }
      scale { x 1 y 1 z 1 }
    }
  }
  rime::render::Camera 0x… { fov_y 0.87266 z_near 0.1 z_far 1000 active true }
}

entity 1 {
  rime::ecs::Parent 0x… { value @0 }  # @0 = a reference to scene-local entity 0; null = none
}
```

- **Whitespace is insignificant** (indentation is cosmetic); `#` starts a comment to end of line.
- Each component carries its **`type_hash`**, so a schema drift is a clean, explicit error
  (`… schema drift: file hash …, engine … — re-save the scene`), never a silent misread.
- An **entity-reference field** (a `Parent`'s target) is written as a scene-local id (`@0`) and
  remapped to the freshly-spawned entity on load, so a scene never bakes in a volatile runtime handle.
  Detection is reflection-native: an entity ref is a field whose nested type is `ecs::Entity`.
- **Omitted fields keep their defaults** (a load default-constructs each component first), which is
  what lets a hand-authored file set only what it cares about.
- **`WorldTransform` is derived, not stored** — run `ecs::propagate_transforms` after a load.

## API

```cpp
#include "rime/scene/scene_format.hpp"

std::string text = scene::save_scene_to_string(world);       // world → text
scene::LoadReport r = scene::load_scene_from_string(world, text); // text → world (register types first)
scene::save_scene_file(world, "level.rscene");
scene::load_scene_file(world, "level.rscene");
```

Register the component set the file uses **before** loading (e.g.
`ecs::register_transform_components` + `render::register_render_components`) — exactly as the editor
host requires. A failed load reports a clear reason and may have partially populated the world, so
load into a world you are willing to discard on failure.

Proof: `tests/scene` — round-trips every registered type bit-exactly (incl. a reparented hierarchy),
loads a hand-authored file, holds float round-trip stable, and rejects malformed / unknown-type /
stale-hash / dangling-reference input cleanly. The `--scene` flag on the launcher (`engine/app`)
loads one end-to-end, over the fixture at `samples/07-first-light/first_light.rscene`.
