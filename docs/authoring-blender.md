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

## 8. The first thing to make

A **trim sheet** for one building kit: one 2048² sheet with window frames, sills, cornices, pipes
and vents as horizontal strips, and a couple of 1024² tiling wall surfaces. It is the highest-leverage
texture in this whole document — one sheet textures a town — and it exercises every part of the
contract above: density, ORM packing, +Y normals, one UV set, and clamped sampling on the sheet.
