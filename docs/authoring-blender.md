# Authoring textured assets for Rime, in Blender

*Written at m16.9. Everything here is what the engine does **today** — where a claim is about
something not yet built, it says so. [ADR-0039](adr/0039-authored-surfaces-m16.md) is the
architecture; [docs/design/assets.md](design/assets.md) is the byte-level format.*

The short version: **Blender → glTF 2.0 → `rime cook` → a `.rscene` that names the asset by content
id.** Everything else on this page is detail about that one path.

---

## 1. Why glTF, and only glTF

The engine loads **only cooked files** — never a `.blend`, `.gltf`, or `.png` at runtime (ADR-0024).
glTF is the interchange format the cook understands, and it is the right one for three reasons that
are not aesthetic:

- It is the **only path that carries materials**. A standalone `rime cook texture.png` produces a
  texture with no material around it, so nothing links it to a mesh.
- Its parameter set **is** the engine's: metallic-roughness, chosen in ADR-0022 precisely because it
  is glTF-native.
- It carries **per-usage colour space**. Base colour and emissive cook as sRGB, normal /
  metallic-roughness / occlusion cook as linear, and the cook works that out from how each texture
  is *used* rather than from a flag you have to remember.

Blender exports it natively. Nothing else in the chain needs to.

---

## 2. The contract

This is the part worth keeping open in another window.

### Texel density and sizes

| Class | Density | Typical set |
|---|---|---|
| First-person weapon | ~1024–2560 px/m on visible faces | one 2048² set, 1024² for attachments |
| Buildings / town kit | **256 px/m** (a 1024² tile covers 4 m) | 1024²–2048² tiling walls, one 2048² trim sheet per kit |
| Props, crates | 256–512 px/m | 512²–1024² unique |
| Vegetation | — | 512² canopy atlas, 256–512² bark tile, 512² grass clump |
| Terrain | near 256 px/m via 2–4 m tiles | 1–2× 2048² tiling ground |

256 px/m is the number to internalise: it is what makes a wall authored for one building look right
next to a wall authored for another, six months apart.

### The five maps

| Map | Channels | Space | Notes |
|---|---|---|---|
| base colour | RGB (+A = mask) | **sRGB** | no lighting or AO baked in |
| ORM | **R = AO, G = roughness, B = metallic** | linear | bind the *same image* to `occlusionTexture` and `metallicRoughnessTexture` in Blender — the cook stores it once and both slots reference one asset |
| normal | RGB tangent-space | linear | **OpenGL convention, +Y / green-up** |
| emissive | RGB | sRGB | |
| *(alpha)* | A of base colour | linear | mask only — see §4 |

**Normal-map handedness is the one that silently ruins lighting.** The shader does `2c−1` with a
bitangent of `w·cross(N,T)`, which is the glTF/MikkTSpace convention: **+Y, green-up**. ambientCG
ships both; take the `GL` variant, or flip green in Krita. A −Y map does not look broken, it looks
*subtly wrong*, which is worse.

### Geometry

- **One UV set.** No lightmap or detail channel exists. Trim sheets, atlases and tiling all live in
  UV0. Overlapping and mirrored islands are fine — nothing bakes lightmaps.
- **No vertex colour.** Variation rides the material's `base_color` factor or decal meshes.
- **Triangles only.** Blender's exporter does this; just do not expect quads to survive.
- **Tangents are optional** — the cook generates MikkTSpace tangents, which is the same basis
  Blender bakes against, so a normal map baked in Blender shades correctly by construction. Do not
  re-unwrap after baking.
- **UV island padding** ≥ 8 px at 1024, 16 px at 2048, and dilate fill colour past island edges
  (Blender's bake margin does this). The mip chain averages across island borders like any other.
- **V origin: no flip.** Row 0 is the image top; Blender's glTF exporter already writes
  glTF-convention UVs. Do nothing.

### Export settings

glTF **Separate** (`.gltf` + textures) or **`.glb`**, apply modifiers, include normals. Skinning
cooks but nothing renders it yet — rigid content only.

---

## 3. The cook

```bash
# one asset, materials and textures and all
cargo run --release -p rime-cli -- cook assets/src/weapons/ak74/ak74.glb --out assets/cooked

# a standalone tileable, where YOU state the colour space
cargo run --release -p rime-cli -- cook sand_bc.png  --srgb   --out assets/cooked
cargo run --release -p rime-cli -- cook sand_orm.png --linear --out assets/cooked

# block-compressed: a quarter the memory and bandwidth (m16.7)
cargo run --release -p rime-cli -- cook sand_bc.png --srgb --bc --out assets/cooked
```

Cooks are content-hash cached and deterministic, so re-cook freely. Since m16.1 the cache keys on
the **cook request** too, so flipping `--srgb`/`--linear`/`--bc` re-cooks instead of silently
serving the previous bytes back.

**Two sources may not share a stem.** `barrel.glb` beside `barrel.gltf`, or `wall.png` beside
`wall.jpg`, both want to write the same cooked filename; the cook refuses rather than letting the
second overwrite the first (m16.x). Rename one.

### Suggested layout

```
assets/
  src/
    weapons/ak74/ak74.blend        # master; never cooked
    weapons/ak74/ak74.glb          # export; the cook input
    weapons/ak74/tex/ak74_bc.png   # _bc  base colour (sRGB)
    weapons/ak74/tex/ak74_orm.png  # _orm AO/rough/metal (linear)
    weapons/ak74/tex/ak74_n.png    # _n   normal, +Y (linear)
    shared/tileables/…             # CC0 library pulls, licences in LICENSES.md
  cooked/                          # rime cook --out; never hand-edited
```

The suffixes are for humans; through glTF the colour space is already automatic.

---

## 4. What works now, and what does not

M16 changed several of these. The column that matters is "today".

| | Today | Notes |
|---|---|---|
| Cooked materials on a scene-placed mesh | **works** (m16.3) | before M16 every placed mesh drew neutral grey |
| Several materials on one mesh | **works** (m16.2) | one draw per glTF primitive; export as one object with several materials |
| Alpha **mask** (foliage, fences, decals) | **works** (m16.4) | including through the depth pre-pass and shadow maps |
| Masked cards at distance | **works** (m16.6) | mip alpha preserves coverage, so cards stop thinning out |
| Double-sided | **works** (m16.5) | glTF `doubleSided` is cooked and honoured |
| Texture wrap / clamp | **works** (m16.5) | set the sampler's wrap mode in Blender; atlases stop bleeding |
| Block compression | **works** (m16.7) | `--bc`, BC7. See the caveat below |
| Alpha **blend** | **NO** | glTF `Blend` draws as **Opaque**. There is no transparency pass. Do not author for it |
| Skinned / animated meshes | **NO** | cooks, does not render |
| Terrain splat / material blending | **NO** | split by material at the object level instead |
| LODs, imposters, instancing | **NO** | the mesh you export is also the far mesh |
| Texture streaming | **NO** | every requested texture is fully resident |

**The BC7 caveat, stated plainly:** the encoder writes **mode 6 only**. That is excellent on smooth
blocks and mediocre where a hard edge crosses one 4×4 block — BC7's partitioned modes exist for
exactly that case and are not implemented. Use `--bc` when memory matters more than the last few
percent of quality, and compare before shipping a hero asset with it. Normal maps take BC7 rather
than BC5 for now, because BC5 needs the shader to reconstruct Z and that change has not landed.

---

## 5. Where art comes from

**CC0 only, for anything committed.** Rime is a public Apache-2.0 repository, so an asset in the
tree has to be redistributable by everyone who clones it. That rules out "free" tiers that are not
open licences.

- **[ambientCG](https://ambientcg.com)** and **[Poly Haven](https://polyhaven.com)** — CC0, and the
  default source for generic surfaces: sand, plaster, brick, concrete, bark, ground. Take the `GL`
  normal variant.
- **Quixel Megascans / Fab free tier** — free is not open. Private experiments only; never anything
  that lands in `assets/`, `samples/` or a PR.

Record every third-party asset and its licence in a `LICENSES.md` beside it. The same policy
`third_party/README.md` applies to code applies to art.

---

## 6. Tools

One recommended path, not a survey:

- **Blender 4.x** — modelling, UV, baking, material assembly, glTF export. The whole 3-D half.
- **Krita** — 2-D painting, masks, touch-ups. Its wrap-around mode (`W`) makes seamless tiles
  directly, and it is 16-bit and linear-aware.
- **Material Maker** — node-based procedural PBR (brick, plaster, sand, bark, trims), exports the
  exact map set above at any resolution.
- **Ucupaint** (Blender add-on) — layered PBR painting *inside* Blender, which for a one-person
  pipeline beats learning a fourth application.
- **ImageMagick** — batch channel packing: `magick ao.png rough.png metal.png -channel RGB -combine
  orm.png`.

All are free and open, and all run natively on Linux and Windows.

---

## 7. Measured end to end, on a real asset

The whole path was run against a CC0 model from Poly Haven (`Barrel_02`, 1k) — chosen because it
ships exactly what §2 asks for: a `_diff` base colour, an `_arm` ORM pack, and the **`_nor_gl`**
(+Y) normal variant.

```
rime cook Barrel_02_1k.gltf --out cooked
  -> Barrel_02_1k.rmesh, .mat0.rmat, img0.lin/img1.srgb/img2.lin.rtex
```

What the engine then read back, and what each line demonstrates:

| Observed | Why it matters |
|---|---|
| colour spaces `img1.srgb`, `img0.lin`, `img2.lin` | the cook picked each from USAGE, not a flag |
| material payload 100 bytes | the m16.5 schema, carrying the two new flags |
| **`double_sided = YES`** | the artist's glTF flag survived cook → engine. Before M16 it was dropped entirely |
| 1 submesh, `material_slot = 0` | the submesh table is real and reaches the runtime |
| `settle took 4 rounds; 1 material resolved` | the four-level chain resolving a genuine cooked material |
| BC7: 5,592,620 → 1,398,344 bytes | **exactly 4.00x**, on a real 1024² texture |

And the authored `.rscene` came out carrying **one `MeshAsset`** (the placed barrel, by content id)
and **two `MeshRef`s** — the hand-authored ones from the default world. That is the m16.8 rule doing
what it is supposed to: the derived index was excluded, the authored ones survived.

**One thing worth knowing about ORM in the wild.** This asset binds its ARM map only to
`metallicRoughnessTexture`, not also to `occlusionTexture`, so its AO channel is cooked but unused
(`occlusion_tex` resolved to 0). If you want AO, bind the same image to BOTH slots in Blender — §2
says so, and this is what it looks like when you do not.

### Three-asset scene proof: .blend → export_gltf.py → rime cook → props.rscene

The workflow was verified end to end across three distinct CC0 `.blend` assets from Poly Haven:
`Barrel_02` (industrial plastic drum), `wooden_crate_02` (shipping crate), and `painted_wooden_bench`
(rustic furniture).

#### 1. Build-time fetch and automated export (`assets/tools/fetch_props.py`)
Rather than committing 11.5 MB of binary GLBs to git history, the assets are downloaded and exported
at build time via `assets/tools/fetch_props.py`:
```bash
python3 assets/tools/fetch_props.py
```
The script queries the Poly Haven API (`https://api.polyhaven.com/files/<id>`), cryptographically
verifies every downloaded `.blend` and texture file against pinned SHA-256 digests, caches downloads
under `build/asset-cache/`, and invokes headless Blender using `assets/tools/export_gltf.py`:
```bash
blender -b <file.blend> --python-exit-code 1 --python assets/tools/export_gltf.py -- <out.glb>
```
The export script enforces the authoring rules from §§ 1–3:
- Strips non-mesh entities (cameras, lights, empties).
- Evaluates and bakes modifier stacks (`bpy.ops.object.convert(target='MESH')`).
- Joins multi-part geometry into a single object per asset (ADR-0039 single-object rule).
- Bakes object transforms into vertex data (`transform_apply`).
- Prunes extraneous UV sets to keep exactly UV0.
- Demotes 32-bit float EXR textures to 8-bit byte buffers (cook v1 requirement).
- Exports standard Y-up GLBs with vertex normals and texcoords.

Source GLB footprints (produced locally in `assets/src/props/`, gitignored):
- `Barrel_02.glb`: 2,884,852 bytes (~2.88 MB)
- `wooden_crate_02.glb`: 4,173,116 bytes (~4.17 MB)
- `painted_wooden_bench.glb`: 4,449,868 bytes (~4.45 MB)
- **Total export size**: 11.5 MB (comfortably within the 25 MB budget; reproducible byte-for-byte).

#### 2. Cooked asset sizes (uncompressed vs `--bc`)
Each prop was cooked with `rime cook assets/src/props --out cooked` and `--bc`:

| Asset | Submesh / Primitive | Cooked Mesh (`.rmesh`) | Material (`.rmat`) | Textures (`.rtex` x3: ORM, base colour, normal) | Textures with `--bc` (BC7) | Total cooked (`--bc`) |
|---|---|---|---|---|---|---|
| `Barrel_02` | 1 submesh | 124,304 B | 124 B | 3 × 5,592,620 B (16.78 MB) | 3 × 1,398,344 B (4.20 MB) | ~4.32 MB |
| `wooden_crate_02` | 1 submesh | 240,560 B | 124 B | 3 × 5,592,620 B (16.78 MB) | 3 × 1,398,344 B (4.20 MB) | ~4.44 MB |
| `painted_wooden_bench` | 1 submesh | 40,136 B | 124 B | 3 × 5,592,620 B (16.78 MB) | 3 × 1,398,344 B (4.20 MB) | ~4.24 MB |

Every 1024² texture compresses from 5,592,620 bytes (uncompressed RGBA8 with complete mip chain) to
exactly 1,398,344 bytes under BC7 — an exact **4.00× reduction**.

#### 3. Scene placement (`samples/m16-blender-props/props.rscene`)
The assets were placed in an authored 7-entity `.rscene` featuring a camera, directional light,
player spawn, ground surface with street material, and three `rime::render::MeshAsset` entities
referencing the cooked mesh content hashes (`bd6bb2b55ec1faf5`, `ef870996039b3b01`, `c3d6b979d3317dda`).

#### 4. Editor viewport verification
In the editor host (`editor-gui` and headless smoke `ed2.png`):
- All three props render with their full cooked PBR textures out of the box.
- Asset bridge settles in 4 rounds (`asset settle took 4 round(s); 3 material(s) resolved, 0 pending`).
- Texturing works immediately because `engine/app/editor_host_app.cpp:510` registers the material sets:
  ```cpp
  renderer.set_material_sets(bridge.material_sets());
  ```

#### 5. Runtime note: registering material sets (`set_material_sets`)
In `samples/99-the-block/main.cpp:2462-2464`, `run_play` settles the asset server but omits
the call to `visuals->renderer.set_material_sets(bridge->material_sets());`. As specified in
`engine/render/include/rime/render/scene_renderer.hpp:583-584`, the renderer defaults to null
material sets (falling back to entity-level `MaterialRef`). Consequently, props render with correct
geometry and transforms but draw untextured white (`game-as-is.png`). Adding that single line
supplies the `MaterialSetRegistry` and restores full PBR texturing in the standalone game
(`game-one-line.png`).

## 8. The first thing to make

A **trim sheet** for one building kit: one 2048² sheet with window frames, sills, cornices, pipes
and vents as horizontal strips, and a couple of 1024² tiling wall surfaces. It is the highest-leverage
texture in this whole document — one sheet textures a town — and it exercises every part of the
contract above: density, ORM packing, +Y normals, one UV set, and clamped sampling on the sheet.

---

## 9. What does not survive the export: mask-driven procedural materials (m16.10)

The contract above assumes a material whose maps are **bound to PBR slots**: an image in
`baseColorTexture`, an image in `metallicRoughnessTexture`, and so on. Blender lets you build a
material that computes its colour instead — a **trim sheet** where a mask selects between tinted
layers inside the node graph, with no single image that *is* the base colour. glTF has no way to
express that, so the exporter picks whichever image is nearest the base-colour slot and the result
is wrong in a way that looks like a renderer bug.

**Measured on Poly Haven's `overhead_crane` (2026-10-23).** Its `overhead_crane_trim` material
exports with `baseColorTexture` → image 1, which is `overhead_crane_trim_mask`. The mask is a
selection map, not an albedo, so the crane renders as saturated mask noise. Nothing in the chain
reports an error: the fetch verifies, Blender exits 0, `rime cook` cooks six textures and two
materials, the mesh resolves, and the renderer draws exactly what it was given. Evidence:
`reports/rime-oct23-big-assets/crane-mask-bug.png`.

**How to spot it before you cook.** Open the `.blend` and look at the material's Base Color input.
If it is a texture node, you are fine. If it is a Mix / ColorRamp / Map Range fed by a `*_mask`
image, the asset needs baking first.

**The fix is to bake, not to patch the exporter.** In Blender, bake the node graph down to a flat
base-colour image (Cycles → Bake → Diffuse, colour only), bind that to Base Color, and export. A
heuristic in `export_gltf.py` that rejected images named `*_mask` would only move the failure: the
asset would then export with *no* base colour at all, which is not better.

The three m16.9 props and the three m16.10 assets that do work (`covered_car`,
`old_military_compressor`, `dutch_ship_large_01`) all use straight texture-to-slot chains. A trim
sheet is the first thing in this repo that does not.
