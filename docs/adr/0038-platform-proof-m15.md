# ADR-0038: M15 — "The Platform Proof", and where M13's frame-rate debt goes

- Status: Proposed
- Date: 2026-08-30

## Context

M13 and M14 both shipped, and three independent review passes over the tree found that both were
reported complete one wire too early. The pattern was identical each time — **a seam built and wired
to a test rather than to the product**:

- **m13.5** shipped the vision demo with `clustered_enabled` and nothing else, so it ran with no
  shadows, no GI and no SSR. M13's "done when" names M8+**M10**+M11+M12. Fixed at m13.L; the claims
  now assert *work done* (`local_shadow_stats`, `sdf.stamps`, `ddgi.probes_updated`) rather than
  settings flags, because a flag can be true while the pass never runs.
- **m14.3** shipped `SaveScene` end to end and left `tools/editor/src/gui.rs:361` a dead
  `ui.label("File")`. The wire message is sent only from `smoke.rs`. **A person editing in the GUI
  loses their work on close.**

The same review found one gap that is not last-mile:

**There is no engine-level answer to "how does a scene file name a mesh."** `render::MeshRef` is a
dense index into a runtime registry. `render::MeshAsset{u64}` is registered, reflected, and resolved
by nothing. `GpuAssetBridge` is textures-only ("v1 scope: textures. Mesh upload and the material
pipeline … are the next bricks"). The consequences compound:

- The Assets panel's `place` button spawns an entity with a `MeshAsset` and no `LocalTransform` — it
  does not draw and the gizmo will not touch it. A dead end presented as a feature.
- Every game must reinvent `blockkit`'s workaround: a game-specific `SlabRole` component plus an
  `apply_palette` function that derives `MeshRef`/`MaterialRef` at load. `role.hpp` concedes that
  even this has the same defect one level up ("reordering that array silently re-tints every
  building … and no counter sees it").
- `engine/app/editor_host_main.cpp` registers `blockkit` — **one game's content module — into the
  engine's editor host.** A second game cannot be inspected without editing engine C++.

VISION §5's second success criterion is still unmet: *"a small team can build a Battlefield-style
multiplayer combat sandbox on Rime without forking the engine."* Everything above is a reason they
cannot.

## Decision

**M15 is "The Platform Proof."**

> **Done when:** a small game that is **not the block** is authored through the editor and runs on
> the engine, with **no engine or editor source changed to support it** — provable by the proof
> brick's own diff touching only `samples/` and `docs/`.

That is the only proof on offer here that can fail honestly: anything you must fork the engine to do
becomes a brick. A milestone that instead promised "the editor gets better" could be declared done at
any point.

**M16 is "The Visual Bar"** — the second half of the same request, split because the two have
different proof regimes. M15's is CI-gateable and GPU-light; M16's can only be judged on hardware.
Bundling them would give one milestone whose "done when" is two sentences joined by "and", which is
the exact shape [ADR-0036](0036-milestone-split-player-and-block.md) cut apart when it split M12.

### M13's frame-rate clause is carried to M16, deliberately and with the number written down

m13.p measured the block for the first time: `frame` p99 **35.60 ms** against ADR-0035's ratified
**16.6 ms**. The budget does not move — it was ratified at m12.0 against measurement, and relaxing it
because the first measurement missed is what a ratified number exists to prevent. So **M13 stands at
⚠️, not ✅**, and its milestone row says so.

It goes to M16 rather than being fixed now because the measurement says where the time is, and it is
not where M15 works:

| | p50 | p99 |
|---|---|---|
| `render` — the entire M10 stack at 1080p | **5.23** | 12.69 |
| `sim.client` | 7.48 | 16.59 |
| `sim.server` | 5.83 | 8.97 |

The renderer is comfortable. What remains is **physics at 667 debris bodies**, which is precisely the
item ADR-0035 §6 listed as *in-M12-only-if-measured*: "the every-tick narrowphase cache (M7's named
first hot spot, likely at 400+ debris)". It now has its measurement. It belongs beside LOD and
occlusion culling in M16, and **M15's proof does not depend on it** — a target range does not level a
city block.

### The rulings

**1. Asset references get an engine-level answer.** `MeshAsset{u64}` resolves to a `MeshRef` with a
real GPU upload, and `assets::MaterialAsset` resolves to a `render::PbrMaterialDesc`. A scene names
content by **asset id**, which is stable across loaders, and not by a dense registry index, which is
correct only while every loader builds its registries in identical order. `blockkit::apply_palette`
stays — a game is still free to derive its look from a role — but it stops being the *only* way.

**2. A game supplies its own editor host.** `engine/editorhost` is already a library and the editor
already accepts `--engine <path>`. What breaks the promise is one game's components compiled into the
engine's host binary. The demo gets its own host; `rime-engine` stops knowing what a `SlabRole` is.
This is the O3DE-shaped answer VISION §4.4 asks for, reached without a plugin system: the composition
point is a binary, not a registry.

**3. A field the inspector shows and the engine ignores is a defect.** `physics::Collider::sensor` is
reflected and read by nothing — it appears in the editor as a checkbox that does nothing.
`assets::AlphaMode`/`alpha_cutoff` are cooked, validated, checksummed and thrown away; there is no
`discard` in any forward shader, so **`Mask` is silently broken too** and all alpha-tested glTF
renders as opaque quads. Each is implemented or deleted in M15. Dead reflected fields are worse than
absent ones, because absent ones cannot be authored against.

**4. The on-ramp is part of the product.** `README.md` contains no build command. There is no
`docs/getting-started.md` and no minimal sample — the smallest engine-framework sample is 763 lines.
VISION §5's third criterion ("someone with no engine experience … lands their first contribution")
cannot be met by a repository you cannot build from its front page.

## The bricks

- **m15.0** — this ADR + the ladder. Decision brick, no engine code.
- **m15.1** — **asset references that survive.** *The architectural core; everything visual depends
  on it.* `MeshAsset → MeshRef` + mesh upload + `MaterialAsset → PbrMaterialDesc` in
  `GpuAssetBridge`; an `AssetServer` material path. *Proof: a scene naming a cooked `.rmesh` by asset
  id loads and draws into a world whose `MeshRegistry` was never touched by hand.*
- **m15.2** — **a game supplies its own editor host.** Move `blockkit` registration out of
  `rime-engine` into the demo's own host. *Proof: a second host serves its own components to an
  unmodified editor.*
- **m15.3** — **the editor can author.** `File → Save/Save As` on the existing wire message; an
  engine-level `Name` component (the outliner is 213 rows of `entity 137` today); `+ spawn` giving a
  `LocalTransform` so the gizmo appears; `place` producing something visible and movable; a colour
  swatch for the three-`DragValue` light colours.
- **m15.4** — **the editor draws real content.** The viewport asset bridge ADR-0037 cut as m14.5.
  *Proof: the streamed-viewport smoke runs against `block.rscene` — the run
  `scripts/editor-smoke.sh` currently refuses because the frame is black.*
- **m15.5** — **pointer capture.** `Window::set_cursor_mode()` across X11/Wayland/Win32/Cocoa,
  honouring the `CursorMode` declared since M2.3 and implemented by nothing; `look_requires_drag`
  goes false. Also honour or delete `WindowDesc::fullscreen` and `high_dpi`, read by no backend.
  **Without this you cannot ship a first-person game.**
- **m15.6** — **dead reflected fields: implement or delete.** `Collider::sensor` → real trigger
  events (which m15.8 needs anyway); `alpha_cutoff` → a `discard` in the forward shaders.
- **m15.7** — **the on-ramp.** README build instructions, `docs/getting-started.md`,
  `samples/hello-game`. Fix `samples/README.md`, which lists `99-the-block` under "Still to come" and
  omits `13-networked-player`.
- **m15.8** — **the proof.** A target range: crates placed in the editor, shot, counted. Authored as
  a `.rscene` through the GUI, run by its own host, gated in CI. **Its diff must not touch `engine/`
  or `tools/`.**

**Cut order if it runs long:** m15.7's `hello-game` → m15.6's alpha mask → m15.3's colour swatch.
**Never cut:** m15.1, m15.3's Save, m15.8 — without any of them there is no proof.

## Every deferred item, ruled

ADR-0035 §6's discipline: an unruled item silently becomes scope. Against its "After M12" list plus
what this session's review added:

- **In M15:** the asset-id → mesh/material resolver · the viewport asset bridge (ADR-0037's cut
  m14.5) · pointer capture · triggers/sensors · alpha `Mask` · entity names · the editor's Save ·
  the on-ramp.
- **In M16:** M13's frame-rate clause and the narrowphase cache · m10.i virtualized geometry ·
  virtual shadow maps · cube shadows · the `kMaxLocalShadows = 8` priority atlas (36 lights compete
  for 8 slots, arbitrated by ECS iteration order) · GI colour bleed · pre-filtered specular probe ·
  LOD · occlusion culling · a sky (the block's frame is pure black above the roofline).
- **Deferred past M16, and named so they are not absent by accident:** `an1` GPU skinning — there is
  no `engine/anim`, and `render::MeshVertex` is a fixed 48-byte layout with no joint indices or
  weights, so the runtime vertex format cannot express a skinned mesh at all · world/level streaming
  · asset eviction and hot reload · a cooked audio format (`bank.hpp`: "no `.rsnd`, no decoder, no
  importer") · gamepad · a game UI system · scripting · static trimesh colliders · lag compensation ·
  late-join baselines.
- **Closed:** the AI track stays closed.

## Alternatives considered

**Fix the frame rate first.** Arguable — a milestone that leaves its predecessor's headline clause
unmet is uncomfortable. Rejected because the measurement is unambiguous about where the cost is
(physics at debris scale, not rendering), that work has no dependency on M15, and M15's proof does
not exercise it. Doing it now would be sequencing by discomfort rather than by dependency.

**A plugin/module system so the editor loads a game's components at runtime.** The O3DE-shaped
answer, and `core` does have a runtime module loader (M1.8). Rejected for M15 as a much larger brick
with a reflection-registration problem at its centre: component registration is compile-time in this
engine. "A game builds its own host binary" reaches the same place for one CMake target, and leaves
the plugin route open.

**Make `MeshRef` content-addressed everywhere and delete the dense index.** Rejected as too wide for
one milestone. `MeshRef` is what the renderer consumes every frame and a dense index is the right
thing there; the fix is that *scene files* stop carrying it, not that the runtime stops using it.
