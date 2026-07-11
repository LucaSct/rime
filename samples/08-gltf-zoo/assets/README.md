# 08-gltf-zoo source assets

Three small, real 3D glTF 2.0 models — one per major M6 asset-pipeline feature — plus the PNG
textures they reference. Each model is single-material / single-primitive, roughly unit-sized,
centered at the origin, +Y up, CCW front faces.

- `cube.gltf` + `textures/cube_albedo.png` — a solid box whose faces map to the numbered tiles
  (1–6, bordered) of a 4×2 sRGB albedo atlas. Exercises **base-color texturing**: cooks a 32-byte
  position/normal/uv mesh, one material, one sRGB texture.
- `sphere.gltf` + `textures/sphere_albedo.png` / `sphere_normal.png` / `sphere_mr.png` — the hero:
  a 24-ring × 48-segment UV sphere whose material carries a genuinely bumpy normal map (an
  analytic egg-crate height field — not a flat `(128,128,255)` sheet) and a varying
  metallic-roughness map (roughness gradient in G, metallic latitude bands in B). Exercises
  **MikkTSpace tangent generation** (the normal map is what gates it → 48-byte tangented
  vertices), **normal mapping**, and **per-usage colour space** — the albedo cooks sRGB while the
  normal/MR maps cook linear, all from one file.
- `rig.gltf` — a skinned three-joint column (root → mid → tip), its sides subdivided so
  hat-function `JOINTS_0`/`WEIGHTS_0` blends bend it smoothly, with translation-only
  `inverseBindMatrices` and one LINEAR animation `Bend` that folds it over and back. Exercises
  **skinning (AN0)**: cooks a 3-joint `.rskel`, a `.ranim` clip, and a 56-byte skinned mesh. The
  geometry is authored in bind space (a skinned node's transform is ignored by the cooker).

These models are **hand-authored for Rime** by [`generate.py`](generate.py), a stdlib-only Python 3
script (it even writes the PNGs with its own minimal encoder): geometry buffers are embedded as
base64 data URIs, textures are generated files under `textures/`, referenced by relative URI so
they stay inspectable. Nothing is vendored from any external asset collection, so none of it
carries a third-party license; it is all covered by the repository's Apache-2.0.

The PNGs live in the `textures/` subdirectory on purpose: a *directory* cook of this folder
(`rime cook samples/08-gltf-zoo/assets --out <dir>`, the CI path) picks up only the top-level
`.gltf` sources — each material pulls its images in with the correct per-usage colour space —
rather than also cooking the loose PNGs as standalone textures.

To regenerate everything (deterministic; models and textures alike):

```
python3 samples/08-gltf-zoo/assets/generate.py
```
