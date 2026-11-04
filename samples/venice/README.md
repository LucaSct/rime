# Venice Map (`samples/venice`)

A Venetian rio between two banks, assembled from Luca's 14 photo-sourced assets: a north row (an
8-bay, two-storey intonaco and Istrian-stone façade on a masegni fondamenta, `z = 0..8`) and a south
row (`z = -14..-6`, bays 0-3 three storeys, bays 4-7 two, openings shifted one bay), both with return
walls 8 m deep and quay end faces at `x = ±16`. The rio (`z = -6..0`) holds two moored boats and floats
on one 1960 × 1960 m lagoon water plane that runs to the horizon; a rose sits on the second barred-window
sill of the north row, and a small Lido sand patch (`x = 16..24`) has driftwood, marram grass, and
shells. `python3 samples/venice/make_scene.py --camera opposite --out PATH` writes a review copy
viewed from the south fondamenta.

## Private Sources (Not in Git)

The 14 source `.glb` files are private photo-derived assets and are **never committed to git**
(repository policy forbids binary assets and non-CC0 art in git history).

By default, `cook_venice.py` reads from:

```text
/mnt/tessera/venice-assets/venice_photo_assets/assets/src
```

Override this path with `RIME_VENICE_SRC` or `--src`.

## Cooking and Scene Generation

After building the engine and CLI (`scripts/build.sh --no-tests`):

```bash
# Verify all 14 GLB SHA-256 pins, cook with --bc into build/asset-cache/venice,
# and verify every MeshAsset / MaterialAsset ID in venice.rscene against manifest.txt:
python3 assets/tools/cook_venice.py

# Deterministically regenerate samples/venice/venice.rscene from manifest.txt:
python3 samples/venice/make_scene.py
```

`cook_venice.py` exits with status `2` if the source directory or any GLB is missing or fails
SHA-256 verification, and exits non-zero if any scene asset ID is absent from the cooked manifest.

## Opening in the Editor and the Game

### Editor

Build the GUI shell if needed (`(cd tools && cargo build -p editor --features gui)`), then launch:

```bash
./tools/target/debug/editor \
  --engine ./build/dev/bin/rime-engine \
  --scene samples/venice/venice.rscene \
  --assets build/asset-cache/venice/manifest.txt
```

Headless viewport smoke capture:

```bash
xvfb-run -a ./tools/target/debug/editor --smoke --frames 8 \
  --engine ./build/dev/bin/rime-engine \
  --scene samples/venice/venice.rscene \
  --assets build/asset-cache/venice/manifest.txt \
  --dump-frame build/venice-shots/editor.ppm
```

### Standalone Game (`99-the-block`)

`the_block` loads its fractured `.rdest` templates (`wall_bg.rdest`, etc.) from the same `--cooked`
directory as `manifest.txt`. Populate `build/dev/samples/99-the-block/cooked` once with
`block_demo_cook` and cook the Venice assets into that directory:

```bash
ctest --test-dir build/dev -R block_demo_cook
python3 assets/tools/cook_venice.py --out build/dev/samples/99-the-block/cooked
./build/dev/bin/the_block --play --scene samples/venice/venice.rscene
```

Headless frame capture:

```bash
xvfb-run -a ./build/dev/bin/the_block --play \
  --scene samples/venice/venice.rscene \
  --frames 8 --ppm build/venice-shots/game.ppm --mute
```

## Stubs, Limitations, and Assumed Dimensions

- **Water is a stub:** There is no water simulation or buoyancy. The lagoon (one plane at
  `y = -0.6`, scaled `1960 × 1960 m`, just inside the camera's 1000 m far plane) uses the cooked `canal_water` quad mesh and its factor-only material. Because `MaterialRef` serializes only a runtime
  table index in `.rscene` (not inline PBR base colour, roughness, or metallic fields), the water
  cannot carry an authored `(0.04, 0.07, 0.06)` green-teal PBR material in `.rscene`.
- **No collision on structures or props:** Only the Fondamenta `GroundSurface` (`x = -16..16`,
  `z = 0..8` at `y = 0`) provides walkable collision. Façades, canal walls, boats, and Lido tiles
  have no physics colliders.
- **Stretched plaster texels on infill slabs:** Each 4 m façade bay scales the `4 × 3 m`
  `venice_intonaco_wall` mesh to form piers and lintel/spandrel infills around openings, which
  stretches/compresses the plaster texels rather than world-space tiling UV0.
- **Assumed asset scales and draft:** Asset dimensions come from the source handoff assumptions:
  boat LOA **7.0 m** and beam **2.0 m**, clear door and window widths **1.10 m** (outer surrounds
  `1.59 m`, `1.63 m`, and `1.31 m`). Both boats place their local `y = 0` plane on the `y = -0.6 m`
  water surface, making their `0.24 m` submerged hull depth an assumed draft.
