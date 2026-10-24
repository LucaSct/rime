# Asset attribution

The 3D model assets in `assets/src/props/` are sourced from **Poly Haven** (https://polyhaven.com/) and released under the **Creative Commons CC0 1.0 Universal (CC0 1.0) Public Domain Dedication**. CC0 dedicates the work to the public domain by waiving all copyright and related rights worldwide.

The `.glb` files are not committed to git; they are fetched at build time and exported via `assets/tools/fetch_props.py` (`python3 assets/tools/fetch_props.py`). The fetcher script queries the Poly Haven API, verifies SHA-256 hashes against pinned digests, caches source assets in `build/asset-cache/`, and executes the headless Blender export script `assets/tools/export_gltf.py` (`blender -b <file.blend> --python-exit-code 1 --python assets/tools/export_gltf.py -- <out.glb>`).

> [!NOTE]
> **Blender authoring vs texture provenance**: The `.blend` project files and mesh geometry were authored and assembled in Blender by Poly Haven's 3D artists. As noted by Poly Haven, source PBR texture maps may derive from photogrammetry, scan data, or physical material captures before being processed, tiled, and packed for Blender.

---

## Assets

| Asset ID | File path | Source URL | Author(s) | Licence |
|---|---|---|---|---|
| `Barrel_02` | `assets/src/props/Barrel_02.glb` | https://polyhaven.com/a/Barrel_02 | Jorge Camacho | CC0 1.0 Universal |
| `wooden_crate_02` | `assets/src/props/wooden_crate_02.glb` | https://polyhaven.com/a/wooden_crate_02 | James Ray Cock (modeling), Jurita Burger (graphic design) | CC0 1.0 Universal |
| `painted_wooden_bench` | `assets/src/props/painted_wooden_bench.glb` | https://polyhaven.com/a/painted_wooden_bench | Kirill Sannikov | CC0 1.0 Universal |

### Asset details

1. **Barrel 02 (`Barrel_02`)**
   - **Path**: `assets/src/props/Barrel_02.glb`
   - **Source**: https://polyhaven.com/a/Barrel_02
   - **Author**: Jorge Camacho
   - **Licence**: CC0 1.0 Universal
   - **Description**: Blue industrial plastic drum container with wear and lid. Single UV set, baked modifiers, transforms applied to vertices. Float textures demoted to 8-bit during export.

2. **Wooden Crate 02 (`wooden_crate_02`)**
   - **Path**: `assets/src/props/wooden_crate_02.glb`
   - **Source**: https://polyhaven.com/a/wooden_crate_02
   - **Authors**: James Ray Cock (modeling), Jurita Burger (graphic design)
   - **Licence**: CC0 1.0 Universal
   - **Description**: Vintage wooden crate with worn planks, dovetail joints, brass corner plates, and rope handles. Joined into a single mesh object with one UV set.

3. **Painted Wooden Bench (`painted_wooden_bench`)**
   - **Path**: `assets/src/props/painted_wooden_bench.glb`
   - **Source**: https://polyhaven.com/a/painted_wooden_bench
   - **Author**: Kirill Sannikov
   - **Licence**: CC0 1.0 Universal
   - **Description**: Antique wooden bench with worn red finish and lower shelf. Joined into a single mesh object with one UV set.
