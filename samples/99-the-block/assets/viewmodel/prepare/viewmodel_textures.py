# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Texture helpers for prepare-viewmodel.py: pull images out of a GLB and convert spec/gloss.

Why the images come from inside the GLBs: an earlier version of this pipeline used
"original" textures downloaded from Sketchfab's public media URLs. Every one of those
files is corrupt -- a valid PNG at the advertised size whose pixels are a ~45-degree
diagonal shear, the signature of pixel data read back at the wrong row stride or channel
count. The damage is in the served bytes, not in our download, so those files cannot be
repaired or trusted. The textures embedded in the downloaded GLBs under ../source/ are
intact (1024x1024, readable UV islands), so they are the only texture source. We never
upscale them: 1024 is the true resolution of the clean art.
"""
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def extract_glb_images(glb_path, out_dir, stem):
    """Write each embedded image of `glb_path` to out_dir/<stem>_<index>.png; return {index: path}."""
    data = Path(glb_path).read_bytes()
    assert data[:4] == b'glTF', glb_path
    json_len, = struct.unpack_from('<I', data, 12)
    doc = json.loads(data[20:20 + json_len])
    bin_len, = struct.unpack_from('<I', data, 20 + json_len)
    blob = data[28 + json_len:28 + json_len + bin_len]
    paths = {}
    for i, img in enumerate(doc['images']):
        view = doc['bufferViews'][img['bufferView']]
        start = view.get('byteOffset', 0)
        path = Path(out_dir) / f'{stem}_{i}.png'
        path.write_bytes(blob[start:start + view['byteLength']])
        paths[i] = path
    return paths


def _srgb_to_linear(v):
    v = v / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def specgloss_to_metalrough(specgloss_path, out_path):
    """KHR_materials_pbrSpecularGlossiness image -> glTF metal/rough (G = roughness, B = metal).

    RGB is specular colour (sRGB), A is glossiness. Metalness is inferred from specular F0
    against a 0.04 dielectric baseline; roughness is 1 - gloss. Spec/gloss and metal/rough
    are not exactly interchangeable, so this is an approximation.
    """
    rgba = np.asarray(Image.open(specgloss_path).convert('RGBA'), dtype=np.float32)
    f0 = _srgb_to_linear(rgba[..., :3]).max(axis=2)
    metal = np.clip((f0 - 0.04) / 0.96, 0.0, 1.0)
    rough = 1.0 - rgba[..., 3] / 255.0
    packed = np.empty((*metal.shape, 3), dtype=np.uint8)
    packed[..., 0] = 255
    packed[..., 1] = np.rint(rough * 255).astype(np.uint8)
    packed[..., 2] = np.rint(metal * 255).astype(np.uint8)
    Image.fromarray(packed, 'RGB').save(out_path)
