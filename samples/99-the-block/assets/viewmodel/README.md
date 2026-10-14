# Held weapon art for `--play`

`the_block --play` can draw a first-person SCAR-H or M4A1 in the player's hands. The models are
third-party [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/) work by three Sketchfab
creators; **they are not in this repository**, and `.gitignore` here keeps them out. Attribution,
original links and the exact modifications are in [ASSET-LICENSES.md](ASSET-LICENSES.md).

## Without the art

Everything still works. CMake notices the two prepared GLBs are absent, skips the cook target and
does not register `the_block_viewmodel_smoke`; `--play` prints one line and runs without hands;
`--viewmodel-smoke` run by hand skips with status 0. Nothing in the engine, the other sample modes
or CI depends on the art being here.

The trade is that **CI cannot prove the weapon path** — `--viewmodel-smoke` is a local gate only.
A change to `PlayViewmodel` or to the first-person render seam has to be exercised by hand, here,
by someone who has the art.

## Getting the art

1. Download the three source models from the Sketchfab links in `ASSET-LICENSES.md` (this needs a
   Sketchfab account, which is why it cannot be scripted) and put the GLBs in `source/` under the
   names `scar-h.glb`, `m4a1.glb` and `fps-arms.glb`.
2. Run `prepare/prepare-viewmodel.py` with Blender 5.2 or newer. It poses the arms onto each
   weapon, converts the source materials to metallic/roughness, and writes `held-scar-h.glb` and
   `held-m4a1.glb` beside this file.
3. Re-run CMake configure. The `EXISTS` check that gates the cook runs at configure time, so a
   build started before the art arrived will not pick it up.

Do **not** substitute textures pulled from Sketchfab's public media URLs for the ones embedded in
the downloaded GLBs. Those served images decode as valid PNGs at the advertised dimensions but
their pixels are sheared about 45 degrees, which is how an earlier preparation pass ended up with
a checkerboarded sleeve and blown-out skin. `prepare/` sources every texture from the embedded
images and says so.
