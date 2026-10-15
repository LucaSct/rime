# Viewmodel artwork and attribution

The two `held-*.glb` files are derivative artwork licensed under [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/). The original creators, titles, and links are:

| Source | Creator | Original |
| --- | --- | --- |
| FN SCAR-H | Alex_Z | https://sketchfab.com/3d-models/fn-scar-h-ba1c338e87fa45c99be0baef5aa8ccd7 |
| fps arms | DJMaesen | https://sketchfab.com/3d-models/fps-arms-08ec4403a47645d8ad80633abf13d39d |
| M4A1 Carbine | MrM0lten | https://sketchfab.com/3d-models/m4a1-carbine-9bb8f18ad94d4b468e447a2d43bd08b9 |

The `source/` directory contains the original GLBs and their provenance JSON, including download URLs and SHA-256 hashes. Every texture in the final GLBs comes from the images embedded in those source GLBs. The `source-textures/` directory holds textures downloaded from Sketchfab's public media URLs (`source-textures/MANIFEST.json` records their URLs, sizes and hashes), but they are corrupt: valid PNGs whose pixels are a diagonal shear. They were NOT used in the final GLBs and are kept only as evidence of the defect. No private viewer data or protected geometry endpoint was used. The M4A1 model's verified download metadata credits MrM0lten, despite an earlier task note naming Jean Isserstedt; attribution here follows the model actually included.

Modifications: the weapon meshes were scaled and rotated to camera-local metres (+X right, +Y up, -Z forward); the arms' wrist IK was posed toward the fore-end and grip, baked to a static mesh, and combined with each weapon. The source helper Icosphere was removed. Original specular/glossiness materials (SCAR-H, arms) were converted approximately to metallic/roughness from the embedded specular/glossiness image (RGB specular F0 for metalness, alpha glossiness for roughness); the M4A1's embedded metallic/roughness images and roughness factors were retained. The embedded base-colour, normal and metal/rough images are 1024x1024 (the M4A1 bullet maps are 512x512), which is their native resolution. The SCAR-H lowpoly base colour keeps its embedded alpha cutout. Occlusion and the SCAR-H emissive map are not carried over. The converted maps exist only inside the GLBs; there is no separate `prepared-textures` output any more. Neither geometry nor textures were artificially upscaled. The exported GLBs contain no rig or animation.

Reproduce from the repository root with Blender 5.2 and Python/Pillow/numpy: `blender -b -t 4 --python samples/99-the-block/assets/viewmodel/prepare/prepare-viewmodel.py` (helper: `prepare/viewmodel_textures.py`). It reads only the GLBs in `source/` and writes the two `held-*.glb` files plus FOV 1.1 rad preview renders into `$RIME_PREVIEW_DIR`. The `prepared-textures/` directory is a leftover of the superseded corrupt-texture pipeline and is unused.
