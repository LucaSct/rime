#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Generate the 08-gltf-zoo source assets: three hand-authored glTF 2.0 models + their PNG textures.

Everything is authored from first principles with the Python standard library only — no external
art, no third-party tools — so the sample carries no third-party license. Geometry lives inside
each `.gltf` as a base64 `data:` URI (the `tests/assets/fixtures` pattern); textures are real PNG
files under `textures/`, referenced by relative URI, so they stay inspectable in any image viewer.
The `textures/` subdirectory also keeps a *directory* cook of this folder from picking the PNGs up
as standalone textures — only the top-level `.gltf` files are cook inputs; each model pulls its own
PNGs in with the correct per-usage colour space (base color = sRGB, normal/MR = linear data).

The three models exercise the M6 asset pipeline end to end:

  cube.gltf    a solid box with a numbered-face sRGB albedo atlas — plain base-color texturing
               (cooks 32-byte position/normal/uv vertices).
  sphere.gltf  the hero: a UV sphere whose material carries a real bumpy normal map (which is what
               makes the cooker run MikkTSpace tangent generation → 48-byte tangented vertices)
               plus a varying metallic-roughness map — all three colour-space usages at once.
  rig.gltf     a skinned three-joint column with one LINEAR "Bend" clip — cooks a skeleton
               (.rskel), a clip (.ranim), and 56-byte skinned vertices (JOINTS_0/WEIGHTS_0).

Conventions (match the engine and cooker): +Y up, right-handed, CCW front faces, each model
roughly unit-sized and centered at the origin; uv (0,0) samples the TOP-left texel (the Vulkan
origin — see `tools/asset-pipeline/src/texture.rs`).

Output is deterministic: the same interpreter and zlib rewrite every file byte-identically.

Regenerate:  python3 samples/08-gltf-zoo/assets/generate.py
"""

import base64
import json
import math
import struct
import zlib
from pathlib import Path

GENERATOR = "rime zoo generator (hand-authored for Rime, stdlib Python, Apache-2.0)"

# glTF wire constants (names per the glTF 2.0 spec).
F32, U16 = 5126, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
_COMP_FMT = {F32: "f", U16: "H"}
_TYPE_LANES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# --------------------------------------------------------------------------------------------------
# A minimal PNG encoder.
#
# The stdlib ships zlib but no image codec, so we write PNG ourselves — a nice excuse to show how
# little a PNG actually is. A PNG file is an 8-byte signature followed by "chunks", each of which
# is: u32 big-endian payload length, a 4-byte ASCII type, the payload, then a CRC-32 over
# type+payload. Three chunks make a complete image:
#
#   IHDR  13 bytes: width, height (u32 BE), bit depth (8), colour type (2 = RGB truecolour),
#         compression method (0 = deflate), filter method (0), interlace (0 = none).
#   IDAT  the pixels: each scanline is prefixed with one *filter* byte, and the whole stream is
#         zlib-compressed. Filters are per-line predictors that help deflate; filter 0 ("None")
#         means "raw bytes, no prediction" — perfectly valid, and it keeps this encoder tiny.
#         Our flat-colour test patterns compress fine without prediction.
#   IEND  empty; marks the end of the stream.
# --------------------------------------------------------------------------------------------------


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def write_png(path: Path, width: int, height: int, pixel) -> None:
    """Write an 8-bit RGB PNG whose texel (x, y) — row 0 at the TOP — is `pixel(x, y) -> (r, g, b)`.

    Top-row-first matters: the cooker does not flip images (uv (0,0) is the top-left texel), so
    painting in this orientation is painting in UV space directly.
    """
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter byte: 0 = None (no per-line prediction)
        for x in range(width):
            raw.extend(pixel(x, y))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"  # signature: high-bit byte + "PNG" + CRLF/EOF/LF transfer tripwires
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _png_chunk(b"IEND", b"")
    )


# --------------------------------------------------------------------------------------------------
# A tiny glTF binary-buffer builder.
#
# glTF separates *storage* (one binary buffer) from *meaning* (accessors: typed views into it).
# This builder appends each attribute array as its own bufferView — 4-byte aligned, so every
# component type is naturally aligned — and returns the accessor index the mesh/skin/animation
# JSON references. One view per accessor is the simplest layout that is unambiguous under the
# spec's stride rules (no byteStride needed, nothing interleaved).
# --------------------------------------------------------------------------------------------------


class GltfBinary:
    def __init__(self):
        self.blob = bytearray()
        self.buffer_views = []
        self.accessors = []

    def accessor(self, flat, component_type, gltf_type, target=None, minmax=False):
        """Pack `flat` (a flat list of numbers) as one bufferView + accessor; return its index."""
        lanes = _TYPE_LANES[gltf_type]
        assert len(flat) % lanes == 0, "flat data must be whole elements"
        fmt = "<%d%s" % (len(flat), _COMP_FMT[component_type])
        data = struct.pack(fmt, *flat)

        while len(self.blob) % 4:  # keep every view 4-byte aligned (spec-friendly, reader-friendly)
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.blob.extend(data)
        self.buffer_views.append(view)

        acc = {
            "bufferView": len(self.buffer_views) - 1,
            "componentType": component_type,
            "count": len(flat) // lanes,
            "type": gltf_type,
        }
        if minmax:
            # min/max must describe the bytes a reader decodes, so compute them from the values
            # *after* the float64 → float32 rounding that struct.pack just performed.
            stored = struct.unpack(fmt, data)
            acc["min"] = [min(stored[i::lanes]) for i in range(lanes)]
            acc["max"] = [max(stored[i::lanes]) for i in range(lanes)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def buffer_json(self):
        uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(self.blob)).decode(
            "ascii"
        )
        return {"byteLength": len(self.blob), "uri": uri}


def _flatten(vectors):
    return [component for vector in vectors for component in vector]


def _write_gltf(path: Path, doc: dict) -> None:
    path.write_text(json.dumps(doc, indent=2) + "\n")


def _doc_skeleton(nodes, meshes, materials, scene_nodes):
    """The shared document shell; callers splice in skins/animations/textures as needed."""
    return {
        "asset": {"version": "2.0", "generator": GENERATOR},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
    }


# --------------------------------------------------------------------------------------------------
# Model 1: the textured cube.
#
# 24 vertices — four per face, not eight shared — because normals and UVs are *per face*: a cube
# corner belongs to three faces that disagree about both, and a glTF vertex is one indivisible
# tuple of all its attributes. Each face is a quad (two CCW triangles) built from an orthonormal
# frame (n, t, b) with t × b = n: walking the corners (-t,-b) → (+t,-b) → (+t,+b) → (-t,+b) is
# counter-clockwise seen from outside (along -n), which is the front-face winding the engine culls
# against. UVs map each face to its own 64×64 tile of a 4×2 atlas, with b as "up" — v grows
# downward in an image, so the +b corners take the tile's smaller v and the painted digits (1–6,
# in face order +X −X +Y −Y +Z −Z) read upright on the surface.
# --------------------------------------------------------------------------------------------------

CUBE_FACES = [
    # (outward normal n, in-plane "right" t, in-plane "up" b) with t × b = n
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
]


def cube_geometry(half=0.5):
    positions, normals, uvs, indices = [], [], [], []
    for face, (n, t, b) in enumerate(CUBE_FACES):
        col, row = face % 4, face // 4  # this face's tile in the 4×2 albedo atlas
        u0, u1 = col / 4.0, (col + 1) / 4.0
        v0, v1 = row / 2.0, (row + 1) / 2.0
        base = len(positions)
        for st, sb, u, v in ((-1, -1, u0, v1), (1, -1, u1, v1), (1, 1, u1, v0), (-1, 1, u0, v0)):
            positions.append([half * (n[i] + st * t[i] + sb * b[i]) for i in range(3)])
            normals.append([float(c) for c in n])
            uvs.append([u, v])
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def build_cube_gltf(path: Path):
    positions, normals, uvs, indices = cube_geometry()
    binary = GltfBinary()
    acc_pos = binary.accessor(_flatten(positions), F32, "VEC3", ARRAY_BUFFER, minmax=True)
    acc_nrm = binary.accessor(_flatten(normals), F32, "VEC3", ARRAY_BUFFER)
    acc_uv = binary.accessor(_flatten(uvs), F32, "VEC2", ARRAY_BUFFER)
    acc_idx = binary.accessor(indices, U16, "SCALAR", ELEMENT_ARRAY_BUFFER)

    doc = _doc_skeleton(
        nodes=[{"name": "cube", "mesh": 0}],
        meshes=[
            {
                "name": "cube",
                "primitives": [
                    {
                        "attributes": {"POSITION": acc_pos, "NORMAL": acc_nrm, "TEXCOORD_0": acc_uv},
                        "indices": acc_idx,
                        "material": 0,
                        "mode": 4,
                    }
                ],
            }
        ],
        materials=[
            {
                "name": "cube-faces",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.6,
                },
            }
        ],
        scene_nodes=[0],
    )
    doc["textures"] = [{"source": 0}]
    doc["images"] = [{"uri": "textures/cube_albedo.png"}]
    doc["bufferViews"] = binary.buffer_views
    doc["accessors"] = binary.accessors
    doc["buffers"] = [binary.buffer_json()]
    _write_gltf(path, doc)


# --------------------------------------------------------------------------------------------------
# Model 2: the normal-mapped metallic-roughness UV sphere (the hero).
#
# A UV sphere is the unit sphere parameterized by two angles, exactly like latitude/longitude:
#
#   θ (polar)   ∈ [0, π]   measured from the +Y pole:  y = cos θ, and the ring radius is sin θ
#   φ (azimuth) ∈ [0, 2π]  around Y:                   x = sin θ · cos φ,  z = sin θ · sin φ
#
# We sample θ over `rings+1` rows and φ over `segments+1` columns. The extra column duplicates the
# φ = 0 ring at φ = 2π: those vertices share a *position* but not a *uv* (u = 0 vs u = 1), and a
# glTF vertex is position+normal+uv as one tuple, so the texture seam needs its own vertices —
# otherwise the last quad would interpolate u from 1 back down to 0, smearing the whole texture
# backwards through one segment. The pole rows collapse to a single position but keep per-column
# u, which is what makes the polar triangle *fan* sample the texture sanely.
#
# Each grid quad becomes two CCW-from-outside triangles, except at the poles where one of the two
# has two corners on the same pole vertex (zero area) and is skipped. Normals are free: on a
# sphere centered at the origin the outward normal IS position/radius. UVs are (φ/2π, θ/π), so
# v = 0 is the north pole — image top, matching the top-left uv origin.
# --------------------------------------------------------------------------------------------------


def sphere_geometry(rings=24, segments=48, radius=0.5):
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        y, ring_radius = math.cos(theta), math.sin(theta)
        for s in range(segments + 1):
            phi = 2.0 * math.pi * s / segments
            unit = (ring_radius * math.cos(phi), y, ring_radius * math.sin(phi))
            positions.append([radius * c for c in unit])
            normals.append(list(unit))
            uvs.append([s / segments, r / rings])

    def grid(r, s):
        return r * (segments + 1) + s

    for r in range(rings):
        for s in range(segments):
            # Quad corners: a—d on ring r, b—c on ring r+1 (one step south), d/c one step east.
            a, d = grid(r, s), grid(r, s + 1)
            b, c = grid(r + 1, s), grid(r + 1, s + 1)
            # With x = sinθ·cosφ, z = sinθ·sinφ, going (south, east) winds CW from outside, so the
            # CCW triangles are (a, c, b) and (a, d, c). At the poles one triangle of each top/bottom
            # quad degenerates (two corners on the pole) and only the fan triangle is emitted.
            if r < rings - 1:
                indices += [a, c, b]
            if r > 0:
                indices += [a, d, c]
    return positions, normals, uvs, indices


def build_sphere_gltf(path: Path):
    positions, normals, uvs, indices = sphere_geometry()
    assert len(positions) < 0xFFFF, "sphere must stay within u16 indices"
    binary = GltfBinary()
    acc_pos = binary.accessor(_flatten(positions), F32, "VEC3", ARRAY_BUFFER, minmax=True)
    acc_nrm = binary.accessor(_flatten(normals), F32, "VEC3", ARRAY_BUFFER)
    acc_uv = binary.accessor(_flatten(uvs), F32, "VEC2", ARRAY_BUFFER)
    acc_idx = binary.accessor(indices, U16, "SCALAR", ELEMENT_ARRAY_BUFFER)

    doc = _doc_skeleton(
        nodes=[{"name": "sphere", "mesh": 0}],
        meshes=[
            {
                "name": "sphere",
                "primitives": [
                    {
                        "attributes": {"POSITION": acc_pos, "NORMAL": acc_nrm, "TEXCOORD_0": acc_uv},
                        "indices": acc_idx,
                        "material": 0,
                        "mode": 4,
                    }
                ],
            }
        ],
        materials=[
            {
                "name": "sphere-hero",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    # Factors stay 1.0 so the metallic-roughness *texture* alone drives the response.
                    "metallicRoughnessTexture": {"index": 1},
                    "metallicFactor": 1.0,
                    "roughnessFactor": 1.0,
                },
                # The presence of normalTexture is what makes the cooker generate MikkTSpace
                # tangents for this mesh (48-byte vertices) — the point of this model.
                "normalTexture": {"index": 2},
            }
        ],
        scene_nodes=[0],
    )
    doc["textures"] = [{"source": 0}, {"source": 1}, {"source": 2}]
    doc["images"] = [
        {"uri": "textures/sphere_albedo.png"},
        {"uri": "textures/sphere_mr.png"},
        {"uri": "textures/sphere_normal.png"},
    ]
    doc["bufferViews"] = binary.buffer_views
    doc["accessors"] = binary.accessors
    doc["buffers"] = [binary.buffer_json()]
    _write_gltf(path, doc)


# --------------------------------------------------------------------------------------------------
# Model 3: the rigged column ("rig") — skinning from first principles.
#
# The mesh is a square column, x/z ∈ ±0.12, y ∈ ±0.5, its four side faces subdivided into 8 rows
# so it can *bend*. It is authored directly in BIND SPACE: the pose the joints are declared in.
# The cooker ignores a skinned mesh node's transform (glTF requires it — the joints place every
# vertex), so bind space is the only honest place to author skinned vertices.
#
# Three joints sit inside the column: root (y = −0.5) → mid (y = 0) → tip (y = +0.5), each child
# translated (0, 0.5, 0) from its parent. Skinning needs two things per joint:
#
#   inverseBindMatrices  Linear-blend skinning moves a vertex by  Σ wᵢ · Wᵢ · Bᵢ⁻¹ · v,  where Wᵢ
#       is joint i's animated world matrix and Bᵢ its *bind* world matrix. Bᵢ⁻¹ (the inverse bind)
#       re-expresses the bind-space vertex in joint i's local frame — "how the vertex rides this
#       bone" — so that Wᵢ can then carry it to the posed position. When nothing is animated,
#       Wᵢ = Bᵢ and the product collapses to identity: the mesh sits exactly as authored. Our
#       joints are pure translations, so each Bᵢ⁻¹ is just a translation back by the joint's bind
#       height (column-major MAT4s below, translation in the last column).
#
#   JOINTS_0 / WEIGHTS_0  Four joint indices (into skin.joints) + four blend weights per vertex.
#       Weights follow triangular "hat" functions of height: weight 1 at the joint's own station,
#       fading linearly to 0 at its neighbours', so every vertex blends at most two joints and the
#       weights sum to 1 by construction (a partition of unity). That linear cross-fade is what
#       turns two rigid bone motions into one smooth bend across the segment between them.
#
# One animation "Bend" (LINEAR, 1 s) rotates mid and tip about +Z: identity → bent (55° + 45°)
# at t = 0.5 → identity. Any sample strictly inside (0, 1) is visibly bent, so a static sampled
# pose proves the whole skeleton/clip path (and it loops cleanly in a viewer).
# --------------------------------------------------------------------------------------------------

RIG_HALF_WIDTH = 0.12
RIG_HALF_HEIGHT = 0.5
RIG_ROWS = 8

RIG_SIDE_FACES = [
    # (outward normal n, horizontal axis t) with the vertical axis b = +Y and t × b = n
    ((1, 0, 0), (0, 0, -1)),
    ((-1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0)),
    ((0, 0, -1), (-1, 0, 0)),
]


def rig_weights(y):
    """Hat-function joint weights for a vertex at height y (see the module comment above)."""
    t = (y + RIG_HALF_HEIGHT) / (2.0 * RIG_HALF_HEIGHT)  # 0 at the base joint, 1 at the tip joint
    w_root = max(0.0, 1.0 - 2.0 * t)
    w_mid = max(0.0, 1.0 - abs(t - 0.5) * 2.0)
    w_tip = max(0.0, 2.0 * t - 1.0)
    return [0, 1, 2, 0], [w_root, w_mid, w_tip, 0.0]


def rig_geometry():
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    hw, hh = RIG_HALF_WIDTH, RIG_HALF_HEIGHT

    def push(position, normal, uv):
        positions.append(list(position))
        normals.append([float(c) for c in normal])
        uvs.append(list(uv))
        j, w = rig_weights(position[1])
        joints.append(j)
        weights.append(w)

    # Four side faces, each a 2-column × (RIG_ROWS+1)-row grid so the skin has rows to bend.
    for face, (n, t) in enumerate(RIG_SIDE_FACES):
        base = len(positions)
        for row in range(RIG_ROWS + 1):
            y = -hh + (2.0 * hh) * row / RIG_ROWS
            for col in (0, 1):
                sideways = (col * 2 - 1) * hw  # -hw or +hw along t
                position = [n[i] * hw + t[i] * sideways for i in range(3)]
                position[1] += y
                # u spans this face's quarter of the texture; v runs top (+Y) → bottom.
                push(position, n, [(face + col) / 4.0, 1.0 - (y + hh) / (2.0 * hh)])
        for row in range(RIG_ROWS):
            v00 = base + row * 2
            v10, v01, v11 = v00 + 1, v00 + 2, v00 + 3
            indices += [v00, v10, v11, v00, v11, v01]  # CCW from outside (t × Y = n)

    # End caps: quads at y = ±hh, wound CCW from outside via the same (n, t, b) frame rule.
    for n, t, b in (((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1))):
        base = len(positions)
        for st, sb, u, v in ((-1, -1, 0.0, 1.0), (1, -1, 1.0, 1.0), (1, 1, 1.0, 0.0), (-1, 1, 0.0, 0.0)):
            position = [hw * (st * t[i] + sb * b[i]) + hh * n[i] for i in range(3)]
            push(position, n, [u, v])
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]

    return positions, normals, uvs, joints, weights, indices


def _translation_inverse_bind(y):
    """Column-major MAT4 undoing a pure translation to (0, y, 0): translate back by (0, −y, 0)."""
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -y, 0.0, 1.0]


def _quat_z(degrees):
    """Quaternion (x, y, z, w) for a rotation of `degrees` about +Z: (0, 0, sin θ/2, cos θ/2)."""
    half = math.radians(degrees) / 2.0
    return [0.0, 0.0, math.sin(half), math.cos(half)]


def build_rig_gltf(path: Path):
    positions, normals, uvs, joints, weights, indices = rig_geometry()
    for w in weights:  # authoring-time honesty check: hat weights must already sum to 1
        assert abs(sum(w) - 1.0) < 1e-6, "rig weights must be a partition of unity"

    binary = GltfBinary()
    acc_pos = binary.accessor(_flatten(positions), F32, "VEC3", ARRAY_BUFFER, minmax=True)
    acc_nrm = binary.accessor(_flatten(normals), F32, "VEC3", ARRAY_BUFFER)
    acc_uv = binary.accessor(_flatten(uvs), F32, "VEC2", ARRAY_BUFFER)
    acc_joints = binary.accessor(_flatten(joints), U16, "VEC4", ARRAY_BUFFER)
    acc_weights = binary.accessor(_flatten(weights), F32, "VEC4", ARRAY_BUFFER)
    acc_idx = binary.accessor(indices, U16, "SCALAR", ELEMENT_ARRAY_BUFFER)

    # Bind heights of root/mid/tip; the inverse binds undo exactly these translations.
    acc_ibm = binary.accessor(
        _translation_inverse_bind(-0.5) + _translation_inverse_bind(0.0) + _translation_inverse_bind(0.5),
        F32,
        "MAT4",
    )
    acc_times = binary.accessor([0.0, 0.5, 1.0], F32, "SCALAR", minmax=True)
    acc_mid_rot = binary.accessor(_quat_z(0) + _quat_z(55) + _quat_z(0), F32, "VEC4")
    acc_tip_rot = binary.accessor(_quat_z(0) + _quat_z(45) + _quat_z(0), F32, "VEC4")

    doc = _doc_skeleton(
        nodes=[
            # The skinned mesh node: no transform on purpose — the mesh is authored in bind space
            # and a skinned node's transform is ignored anyway (the joints place the vertices).
            {"name": "rig", "mesh": 0, "skin": 0},
            {"name": "root", "translation": [0.0, -0.5, 0.0], "children": [2]},
            {"name": "mid", "translation": [0.0, 0.5, 0.0], "children": [3]},
            {"name": "tip", "translation": [0.0, 0.5, 0.0]},
        ],
        meshes=[
            {
                "name": "rig",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": acc_pos,
                            "NORMAL": acc_nrm,
                            "TEXCOORD_0": acc_uv,
                            "JOINTS_0": acc_joints,
                            "WEIGHTS_0": acc_weights,
                        },
                        "indices": acc_idx,
                        "material": 0,
                        "mode": 4,
                    }
                ],
            }
        ],
        materials=[
            {
                "name": "rig-clay",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.78, 0.4, 0.14, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.65,
                },
            }
        ],
        scene_nodes=[0, 1],
    )
    doc["skins"] = [{"joints": [1, 2, 3], "inverseBindMatrices": acc_ibm, "skeleton": 1}]
    doc["animations"] = [
        {
            "name": "Bend",
            "samplers": [
                {"input": acc_times, "output": acc_mid_rot, "interpolation": "LINEAR"},
                {"input": acc_times, "output": acc_tip_rot, "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": 2, "path": "rotation"}},
                {"sampler": 1, "target": {"node": 3, "path": "rotation"}},
            ],
        }
    ]
    doc["bufferViews"] = binary.buffer_views
    doc["accessors"] = binary.accessors
    doc["buffers"] = [binary.buffer_json()]
    _write_gltf(path, doc)


# --------------------------------------------------------------------------------------------------
# Textures. All are painted per-texel in UV orientation (row 0 = v 0 = image top).
#
# Colour space is decided by *usage* at cook time (M6.3/M6.4 rule): we simply author correct
# pixels. The albedo atlases are perceptual colour (they will cook sRGB); the normal and
# metallic-roughness maps are *data* (they will cook linear) — a normal texel encodes a unit
# vector, an MR texel packs roughness into G and metallic into B (the glTF/ORM layout; R carries
# white where occlusion would live, though this material doesn't reference it).
# --------------------------------------------------------------------------------------------------

# The cube's 4×2 atlas of 64 px tiles: digits 1–6 for the six faces plus two spare checker tiles.
CUBE_TILE_COLORS = [
    (198, 62, 54),  # 1 · +X · red
    (72, 158, 74),  # 2 · −X · green
    (60, 110, 198),  # 3 · +Y · blue
    (222, 168, 44),  # 4 · −Y · amber
    (164, 74, 158),  # 5 · +Z · magenta
    (54, 158, 162),  # 6 · −Z · teal
]

# Seven-segment digit shapes: the classic display layout (A top, G middle, D bottom; F/B upper
# left/right, E/C lower left/right), each segment a filled rectangle in 64×64 tile-local pixels.
SEGMENT_RECTS = {
    "A": (20, 12, 44, 18),
    "B": (38, 12, 44, 35),
    "C": (38, 29, 44, 52),
    "D": (20, 46, 44, 52),
    "E": (20, 29, 26, 52),
    "F": (20, 12, 26, 35),
    "G": (20, 29, 44, 35),
}
DIGIT_SEGMENTS = {1: "BC", 2: "ABGED", 3: "ABGCD", 4: "FGBC", 5: "AFGCD", 6: "AFGEDC"}


def cube_albedo_pixel(x, y):
    tile, lx, ly = (y // 64) * 4 + (x // 64), x % 64, y % 64
    if tile >= 6:  # spare tiles: a quiet dark checker, so the whole atlas is intentional
        return (46, 48, 52) if ((lx // 16) + (ly // 16)) % 2 == 0 else (62, 65, 71)
    color = CUBE_TILE_COLORS[tile]
    if lx < 4 or ly < 4 or lx >= 60 or ly >= 60:  # border makes each face edge obvious
        return tuple(c // 2 for c in color)
    for segment in DIGIT_SEGMENTS[tile + 1]:
        x0, y0, x1, y1 = SEGMENT_RECTS[segment]
        if x0 <= lx < x1 and y0 <= ly < y1:
            return (245, 243, 238)
    return color


def sphere_albedo_pixel(x, y):
    # Eight longitude stripes (u) with a darker ring every sixth of latitude (v): the pattern makes
    # the sphere's parameterization — and any UV mistake — immediately visible on screen.
    u, v = x / 256.0, y / 256.0
    color = (232, 224, 199) if int(u * 8.0) % 2 == 0 else (172, 62, 40)
    if (v * 6.0) % 1.0 < 0.08:
        color = tuple(int(c * 0.62) for c in color)
    return color


# The normal map is derived from an analytic height field rather than drawn by hand:
#
#     h(u, v) = H · cos(2π·f·u) · cos(2π·f·v)                       (an "egg-crate" of dimples)
#
# Displacing a surface by h along its normal N tilts the surface tangents, and to first order the
# perturbed normal in tangent space (T along +u, B along +v, N out of the surface) is
#
#     n ∝ ( −∂h/∂u, −∂h/∂v, 1 )
#
# — each slope leans the normal *against* the rise. Differentiating h gives slopes with amplitude
# S = 2π·f·H, so we pick S directly (0.55, a strong visible tilt) instead of H. cos is 1-periodic
# in both axes, so the map tiles seamlessly — in particular across the sphere's u = 0/1 seam. The
# texel encoding maps each unit-vector component from [−1, 1] into [0, 255] ((n+1)/2·255, the
# standard tangent-space encoding); a flat texel would be (128, 128, 255), and this map is very
# deliberately not flat — it is what proves tangent generation + normal mapping end to end.
NORMAL_FREQ = 6.0
NORMAL_SLOPE = 0.55


def sphere_normal_pixel(x, y):
    u, v = (x + 0.5) / 256.0, (y + 0.5) / 256.0  # sample at texel centers
    slope_u = NORMAL_SLOPE * math.sin(2.0 * math.pi * NORMAL_FREQ * u) * math.cos(
        2.0 * math.pi * NORMAL_FREQ * v
    )
    slope_v = NORMAL_SLOPE * math.cos(2.0 * math.pi * NORMAL_FREQ * u) * math.sin(
        2.0 * math.pi * NORMAL_FREQ * v
    )
    # n = normalize(−∂h/∂u, −∂h/∂v, 1); the minus signs are already folded into slope_u/slope_v
    # (d/du of cos is −sin), so these ARE the negated derivatives.
    length = math.sqrt(slope_u * slope_u + slope_v * slope_v + 1.0)
    n = (slope_u / length, slope_v / length, 1.0 / length)
    return tuple(max(0, min(255, round((c + 1.0) * 0.5 * 255.0))) for c in n)


def sphere_mr_pixel(x, y):
    # ORM-style packing: R = 255 (where occlusion would sit), G = roughness sweeping 0.15 → 0.9
    # left to right, B = metallic in four alternating latitude bands. Both channels vary on
    # purpose: a cooked-then-rendered sphere shows a roughness gradient across shiny metal rings.
    roughness = 38 + round((x / 255.0) * 192)
    metallic = 255 if (y // 64) % 2 == 0 else 0
    return (255, roughness, metallic)


# --------------------------------------------------------------------------------------------------


def main():
    here = Path(__file__).resolve().parent
    textures = here / "textures"
    textures.mkdir(exist_ok=True)

    write_png(textures / "cube_albedo.png", 256, 128, cube_albedo_pixel)
    write_png(textures / "sphere_albedo.png", 256, 256, sphere_albedo_pixel)
    write_png(textures / "sphere_normal.png", 256, 256, sphere_normal_pixel)
    write_png(textures / "sphere_mr.png", 256, 256, sphere_mr_pixel)

    build_cube_gltf(here / "cube.gltf")
    build_sphere_gltf(here / "sphere.gltf")
    build_rig_gltf(here / "rig.gltf")

    for name in (
        "cube.gltf",
        "sphere.gltf",
        "rig.gltf",
        "textures/cube_albedo.png",
        "textures/sphere_albedo.png",
        "textures/sphere_normal.png",
        "textures/sphere_mr.png",
    ):
        print(f"wrote {name} ({(here / name).stat().st_size} bytes)")


if __name__ == "__main__":
    main()
