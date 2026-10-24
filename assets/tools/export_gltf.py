# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Export a .blend as the single-object .glb that docs/authoring-blender.md section 2 asks for.

    blender -b <file.blend> --python-exit-code 1 --python assets/tools/export_gltf.py -- <out.glb>

Why a script instead of File > Export: the contract is a list of things that are easy to forget
and that fail QUIETLY. A mesh with a non-identity transform draws in the wrong place, a second UV
set is silently dropped by the cook, and an unapplied modifier exports the un-modified mesh. This
script applies the fix for each one, so every asset on the way through is held to the same rules,
and the rules live in a file that git can diff.

`--python-exit-code 1` on the command line is NOT optional: without it Blender 5.x exits 0 when a
Python exception escapes, and a successful process status then leaves a stale .glb in place.

What it does, in order:
  1. keeps only mesh objects (Poly Haven files carry lights, cameras and sculpting helpers),
  2. bakes every modifier (geometry nodes, weighted normals) by converting to plain meshes,
  3. joins them into ONE object, because the cook's submesh table is per glTF primitive and one
     object with several materials is the shape ADR-0039 ruling 1 expects,
  4. applies location / rotation / scale so the vertices carry the transform (the engine places the
     asset with its own LocalTransform; a transform baked into the node would be applied twice),
  5. leaves exactly one UV set (the engine has UV0 only; extras would only waste bytes),
  6. demotes float (EXR) images to 8-bit, because the cook v1 only reads 8-bit textures,
  7. exports GLB with Y-up (glTF convention) and the normals the cook needs.
"""

import sys

import bpy


def _out_path() -> str:
    if "--" not in sys.argv or len(sys.argv) <= sys.argv.index("--") + 1:
        raise SystemExit("export_gltf.py: usage: ... --python export_gltf.py -- <out.glb>")
    return sys.argv[sys.argv.index("--") + 1]


def _select_only(objs):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def _demote_float_images() -> None:
    """Re-point every float-buffer (EXR) image at an 8-bit copy.

    Poly Haven ships normal, roughness and metallic maps as 32-bit float EXRs. Blender's glTF
    exporter writes those out as 16-bit PNG, and `rime cook` refuses them ("v1 cooks only 8-bit
    textures", measured 2026-10-21). The fix belongs here, in the export, rather than in a GUI:
    copy the pixels into a byte-buffer image of the same name and colour space, and swap it into
    every node that used the original. The values are already 0..1 (they are display-referred
    texture data, not HDR), so quantising to 8 bits is the only loss and it is the contract's own
    precision.
    """
    import numpy as np

    for img in list(bpy.data.images):
        if not img.is_float or img.size[0] == 0:
            continue
        w, h = img.size
        px = np.empty(w * h * 4, dtype=np.float32)
        img.pixels.foreach_get(px)
        px = np.clip(px, 0.0, 1.0)
        new = bpy.data.images.new(img.name + "_8bit", w, h, alpha=False, float_buffer=False)
        # Set the colour space FIRST so the byte values are stored verbatim, not re-encoded.
        new.colorspace_settings.name = img.colorspace_settings.name
        new.pixels.foreach_set(px)
        new.update()
        new.pack()
        for mat in bpy.data.materials:
            if mat.node_tree:
                for node in mat.node_tree.nodes:
                    if node.type == "TEX_IMAGE" and node.image == img:
                        node.image = new


def main() -> None:
    out = _out_path()

    # 1. Anything that is not geometry cannot be in the file (the cook ignores it at best).
    for o in list(bpy.data.objects):
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("export_gltf.py: the file has no mesh objects")

    # 2. Bake modifiers. `convert` evaluates the modifier stack, which is exactly "export_apply"
    #    but done up front so the join in step 3 sees the final geometry.
    _select_only(meshes)
    bpy.ops.object.convert(target="MESH")

    # 3. One object.
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    _select_only(meshes)
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active

    # 4. The transform goes into the vertices.
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # 5. One UV set. Keep the first (Poly Haven's is the one the textures were authored against).
    uvs = obj.data.uv_layers
    while len(uvs) > 1:
        uvs.remove(uvs[len(uvs) - 1])
    if len(uvs) != 1:
        raise SystemExit("export_gltf.py: the mesh has no UV set, so nothing could be textured")

    _demote_float_images()

    _select_only([obj])
    print(
        f"export_gltf: {obj.name}: {len(obj.data.vertices)} verts, "
        f"{sum(len(p.vertices) - 2 for p in obj.data.polygons)} tris, "
        f"{len(obj.data.materials)} material(s), uv='{uvs[0].name}'"
    )

    # 7. Export. Defaults are kept wherever the contract has no opinion; the lines below are the
    #    ones it does (docs/authoring-blender.md, "Export settings").
    bpy.ops.export_scene.gltf(
        filepath=out,
        export_format="GLB",
        use_selection=True,
        export_apply=True,
        export_yup=True,
        export_normals=True,
        export_texcoords=True,
        export_tangents=False,  # the cook generates MikkTSpace tangents itself
        export_materials="EXPORT",
        export_image_format="AUTO",
        export_cameras=False,
        export_lights=False,
        export_animations=False,
        export_skins=False,
    )


main()
