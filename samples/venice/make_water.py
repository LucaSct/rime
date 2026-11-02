# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Write `samples/venice/canal_water.gltf`, the canal's stand-in water surface.

Why a generated glTF and not a `MaterialRef`: a `.rscene` `MaterialRef` is only an index into the
host's runtime material table (slot 0 is the default white), so a scene file cannot author a
colour or a roughness through it. Every authored PBR value in Rime enters through the cook, as a
glTF material. The smallest honest water surface is therefore a one-quad glTF with no textures:
its colour and roughness live in the material's *factors* (`baseColorFactor`,
`roughnessFactor`), which `gltf_material` cooks exactly like a textured material whose maps are
all white.

It is a STUB, not water: no waves, no refraction, no simulation. It is dark and very smooth
(roughness 0.05) so that the sky and the facade reflect in it through the renderer's specular
and SSR paths, which is most of what makes a canal read as a canal at a distance.

The quad is 1 x 1 m in XZ at y = 0, centred on the origin, facing +Y; the scene scales it.
Output is deterministic (fixed float formatting, fixed key order), so the cooked content id is
stable and the file can be committed: it is code-generated geometry, not a photo asset.
"""

from __future__ import annotations

import base64
import json
import struct
from pathlib import Path

# Linear-space base colour of a Venetian rio in shade: a very dark green-teal.
BASE_COLOR = [0.04, 0.07, 0.06, 1.0]
ROUGHNESS = 0.05


def build() -> dict:
    positions = [(-0.5, 0.0, -0.5), (0.5, 0.0, -0.5), (0.5, 0.0, 0.5), (-0.5, 0.0, 0.5)]
    normals = [(0.0, 1.0, 0.0)] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    # Counter-clockwise seen from +Y (glTF front faces are CCW).
    indices = [0, 2, 1, 0, 3, 2]

    blob = b"".join(struct.pack("<3f", *p) for p in positions)
    blob += b"".join(struct.pack("<3f", *n) for n in normals)
    blob += b"".join(struct.pack("<2f", *t) for t in uvs)
    blob += b"".join(struct.pack("<H", i) for i in indices)
    uri = "data:application/octet-stream;base64," + base64.b64encode(blob).decode("ascii")

    return {
        "asset": {"version": "2.0", "generator": "rime samples/venice/make_water.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "canal_water"}],
        "meshes": [
            {
                "name": "canal_water",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                    }
                ],
            }
        ],
        "materials": [
            {
                "name": "canal_water",
                "pbrMetallicRoughness": {
                    "baseColorFactor": BASE_COLOR,
                    "metallicFactor": 0.0,
                    "roughnessFactor": ROUGHNESS,
                },
            }
        ],
        "buffers": [{"byteLength": len(blob), "uri": uri}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962},
            {"buffer": 0, "byteOffset": 128, "byteLength": 12, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-0.5, 0.0, -0.5], "max": [0.5, 0.0, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
    }


def main() -> None:
    out = Path(__file__).resolve().parent / "canal_water.gltf"
    out.write_text(json.dumps(build(), indent=1, sort_keys=False) + "\n", encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
