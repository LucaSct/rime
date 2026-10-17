# Held weapon art for `--play`

`the_block --play` draws a SCAR-H (`1`), M4A1 (`2`), or Remington MSR with a Schmidt & Bender
5–25×56 PM II (`3`). Hold `Q` or right mouse to aim (also during drag-look fallback). The aimed ocular sits 50 mm from the eye (about 85% of frame height). The optical view is
5×; only the physical scope lens magnifies, and the surrounding view keeps its normal FOV.
The models are third-party [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/) work by four Sketchfab
creators; **they are not in this repository**, and `.gitignore` here keeps them out. Attribution,
original links and the exact modifications are in [ASSET-LICENSES.md](ASSET-LICENSES.md).

All three weapons have separate aim poses and authored opposed thumbs/finger curls. The optical
camera is calibrated through the physical ocular for an apparent 5× image; a fivefold feed focal
ratio alone would only appear about 2.36× through this aperture. Rifles retain normal magnification.

## Without the art

Everything still works. CMake notices the six prepared GLBs are absent, skips the cook target and
does not register `the_block_viewmodel_smoke`; `--play` prints one line and runs without hands;
`--viewmodel-smoke` run by hand skips with status 0. Nothing in the engine, the other sample modes
or CI depends on the art being here.

The trade is that **CI cannot prove the weapon path** — `--viewmodel-smoke` is a local gate only.
A change to `PlayViewmodel` or to the first-person render seam has to be exercised by hand, here,
by someone who has the art.

## Getting the art

1. Obtain the four source models linked in `ASSET-LICENSES.md`, respecting their CC-BY attribution,
   and put them in `source/` as `scar-h.glb`, `m4a1.glb`, `msr.glb`, and `fps-arms.glb`.
   Existing verified public Objaverse copies are also usable; provenance records in `source/`
   record each local download's URL and SHA-256.
2. Run `blender -b -t 4 --python-exit-code 1 --python
   samples/99-the-block/assets/viewmodel/prepare/prepare-viewmodel.py` from the repo root.
   It writes three `held-*.glb` weapons, three weighted `arms-*.glb` rigs, editable `grip-*.blend`
   files, grip-frame sidecars and previews. The GLBs and editable artwork remain outside git.
   Uniform metre-space skeletons retain finger poses; runtime two-bone IK solves elbows and wrist
   frames after weapon bob, recoil and aiming, then skins actual weighted vertices every frame.
3. Re-run CMake configure. The `EXISTS` check that gates the cook runs at configure time, so a
   build started before the art arrived will not pick it up.

Do **not** substitute textures pulled from Sketchfab's public media URLs for the ones embedded in
the downloaded GLBs. Those served images decode as valid PNGs at the advertised dimensions but
their pixels are sheared about 45 degrees, which is how an earlier preparation pass ended up with
a checkerboarded sleeve and blown-out skin. `prepare/` sources every texture from the embedded
images and says so.

The MSR source has solid materials and no texture maps; the preparation keeps those materials.
Its original opaque glass is replaced by a physical lens with the separate camera's HDR image,
eye-box blackout, reticle and Fresnel-weighted sky reflections. This is an optical approximation,
not a simulation of every glass element. Scope rendering adds a second scene view only when its
exit pupil can transmit to the eye. At hip fire the same glass/reflection pass uses a black input.
Hold right mouse to aim while drag-look remains available; `Q` is another held aim control.

The camera represents the right eye. Each measured stock seats on a fixed right shoulder with
a bladed arm frame; wrist constraints still follow bob, recoil and the full aim blend. Both M4
sights define its calibrated sight line. The SCAR's opaque panes are separated from their shared
housing material; its original near glass masks a transparent 1× parent-world image and collimated
red ring. This path needs no second world camera. See
[ADR-0086](../../../../docs/adr/0086-play-sight-calibration-and-render-graph-scale.md).
