# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Author weapon-local grip frames and retain weighted, editable arms for runtime IK.

The imported bone tails are display helpers, often unrelated to the next joint. Solve
from joint HEADS instead. All exported rest bones and vertices share camera-local metres;
there is no nonuniform shoulder-lowering shear for the runtime solver to compensate for.
"""
import json
import math
import bpy
from mathutils import Matrix, Vector

CHAINS = [('L_arm_01', 'L_elbow_02', 'L_wrist_03'),
          ('R_arm_023', 'R_elbow_024', 'R_wrist_025')]
# Wrist centres are BELOW the knuckle row, at each actual grip. The former targets
# put the right knuckles in the receiver and left the unposed thumb sticking out.
WRIST_POINTS = {
    'scar-h': ((.112, -.365, -.705), (.168, -.343, -.363)),
    'm4a1': ((.117, -.295, -.680), (.163, -.333, -.366)),
    'msr': ((.112, -.300, -.465), (.166, -.385, -.030)),
}


def place_bone(rig, bone, matrix):
    bone.matrix = rig.matrix_world.inverted() @ matrix
    bpy.context.view_layer.update()


def orient_segment(rig, bone, next_bone, destination):
    world = rig.matrix_world @ bone.matrix
    head = world.translation
    current = (rig.matrix_world @ next_bone.matrix).translation - head
    rotation = current.rotation_difference(destination - head).to_matrix().to_4x4()
    place_bone(rig, bone, Matrix.Translation(head) @ rotation @
               Matrix.Translation(-head) @ world)


def solve_chain(rig, names, wrist_frame, pole):
    upper, lower, wrist = [rig.pose.bones[n] for n in names]
    shoulder = (rig.matrix_world @ upper.matrix).translation
    elbow = (rig.matrix_world @ lower.matrix).translation
    hand = (rig.matrix_world @ wrist.matrix).translation
    a, b = (elbow - shoulder).length, (hand - elbow).length
    delta = wrist_frame.translation - shoulder
    distance = delta.length
    if not abs(a-b)+1e-5 < distance < a+b-1e-5:
        raise RuntimeError(f'Authored grip unreachable: {names}, {distance} vs {a+b}')
    direction = delta.normalized()
    bend = pole - shoulder
    bend = (bend - direction * bend.dot(direction)).normalized()
    along = (a*a-b*b+distance*distance)/(2*distance)
    elbow = shoulder + direction*along + bend*math.sqrt(max(0, a*a-along*along))
    orient_segment(rig, upper, lower, elbow)
    orient_segment(rig, lower, wrist, wrist_frame.translation)
    # The authoring rig is still in centimetres under a .01 object transform. A
    # rigid metre-space wrist target must keep that bone scale until export.
    place_bone(rig, wrist, wrist_frame @ Matrix.Diagonal((.01,.01,.01,1)))


def prepare_arms(p, kind, mapping):
    old = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=str(p.SRC/'fps-arms.glb'))
    imported = set(bpy.context.scene.objects)-old
    rig = next(o for o in imported if o.type == 'ARMATURE')
    mesh = next(o for o in imported if o.type == 'MESH' and o.name != 'Icosphere')
    # Uniform centimetres->metres and a proper rotation. Move the shoulders below the
    # camera, while their depth leaves room for a forward support arm without stretching.
    transform = Matrix(((-.01,0,0,.14), (0,0,.01,-.64),
                        (0,.01,0,-.65), (0,0,0,1)))
    rig.matrix_world = transform
    mesh.matrix_world = transform
    bpy.context.view_layer.update()
    grips = []
    for side, names in enumerate(CHAINS):
        # The source's default pose has displaced shoulder roots. Author them below
        # the camera with enough reach for both hip and ADS, rather than stretching.
        upper = rig.pose.bones[names[0]]
        root_frame = rig.matrix_world @ upper.matrix
        root_frame.translation = Vector((-.12 if side == 0 else .36, -.42, -.36 if side == 0 else -.28))
        place_bone(rig, upper, root_frame)
        wrist = rig.pose.bones[names[2]]
        # Retain the imported hand's roll initially, then rotate the palm's long axis
        # down the pistol/vertical grip. A full frame, rather than a wrist point, locks roll.
        prefix = 'L_' if side == 0 else 'R_'
        middle = next(b for b in rig.pose.bones if b.name.startswith(prefix+'middle1_'))
        source = (rig.matrix_world @ middle.matrix).translation-(rig.matrix_world @ wrist.matrix).translation
        desired = Vector((0,.85,-.52)).normalized()
        rotation = source.rotation_difference(desired).to_matrix().to_4x4()
        frame = rotation @ rig.matrix_world @ wrist.matrix
        # Remove the source centimetre scale: a grip frame is a rigid transform.
        frame = frame.normalized()
        frame.translation = Vector(WRIST_POINTS[kind][side])
        solve_chain(rig, names, frame,
                    Vector((-.4 if side == 0 else .5, -.65, -.25)))
        # Curl the finger chains around the grip rather than leaving the source's
        # relaxed fingers extending away from it. The runtime retains these local
        # finger poses while IK controls the wrist's complete contact frame.
        inward = Vector((1 if side == 0 else -1, 0, 0))
        axis = desired.cross(inward).normalized()
        for finger in ('point', 'middle', 'ring', 'pink'):
            if side == 1 and finger == 'point':
                continue  # authored independently against the trigger guard below
            for phalanx, degrees in ((1,15), (2,55), (3,40)):
                bone = next(b for b in rig.pose.bones
                            if b.name.startswith(prefix+finger+str(phalanx)+'_'))
                world = rig.matrix_world @ bone.matrix
                head = world.translation
                rotation = Matrix.Rotation(math.radians(degrees), 4, axis)
                place_bone(rig, bone, Matrix.Translation(head) @ rotation @
                           Matrix.Translation(-head) @ world)
        # The imported thumbs were previously left in their open-hand pose. Solve each
        # phalanx from joint heads so the thumb opposes the curled fingers on the grip;
        # the source bone tails are unrelated display helpers, not anatomical endpoints.
        for phalanx, direction in enumerate(((0,.65,-.76),
                (-.5 if side == 0 else .5,.2,-.85),
                (-.25,-.3,-.92) if side == 0 else (.8,-.3,-.5)), start=1):
            bone = next(b for b in rig.pose.bones
                        if b.name.startswith(prefix+'thumb'+str(phalanx)+'_'))
            child = next(b for b in rig.pose.bones
                        if b.name.startswith(prefix+'thumb'+str(phalanx+1)+'_'))
            origin = (rig.matrix_world @ bone.matrix).translation
            orient_segment(rig, bone, child, origin + Vector(direction))
        if side == 1:
            # The index follows the trigger guard, not the shared fist curl. A mostly
            # straight source index extended left of the receiver instead of into its guard.
            # Use actual head-to-head segment lengths and keep its bend in the guard.
            directions = ((0,.25,-.968), (-.3,0,-.954), (-.3,-.15,-.942))
            if kind == 'm4a1':
                directions = ((0,.70,-.714), (-.3,.2,-.933), (-.3,-.1,-.949))
            if kind == 'msr':
                directions = ((0,.05,-1), (-.75,.15,-.65), (.5,-.3,.81))
            for phalanx, direction in enumerate(directions, start=1):
                bone = next(b for b in rig.pose.bones
                            if b.name.startswith(prefix+'point'+str(phalanx)+'_'))
                child = next(b for b in rig.pose.bones
                            if b.name.startswith(prefix+'point'+str(phalanx+1)+'_'))
                origin = (rig.matrix_world @ bone.matrix).translation
                orient_segment(rig, bone, child, origin + Vector(direction))
        grips.append(frame.copy())
    # Preserve this solved pose as the weighted REST mesh. Runtime IK starts here and
    # changes shoulder/elbow/wrist matrices; finger children retain their authored pose.
    deps = bpy.context.evaluated_depsgraph_get()
    baked = bpy.data.meshes.new_from_object(mesh.evaluated_get(deps),
        preserve_all_data_layers=True, depsgraph=deps)
    baked.transform(mesh.matrix_world)
    posed = {b.name: (rig.matrix_world @ b.matrix, b.length*.01) for b in rig.pose.bones}
    new_data = bpy.data.armatures.new('Runtime grip rig '+kind)
    new_rig = bpy.data.objects.new('Arms '+kind, new_data)
    bpy.context.collection.objects.link(new_rig)
    bpy.ops.object.select_all(action='DESELECT')
    new_rig.select_set(True)
    bpy.context.view_layer.objects.active = new_rig
    bpy.ops.object.mode_set(mode='EDIT')
    for b in rig.data.bones:
        e = new_data.edit_bones.new(b.name)
        matrix, length = posed[b.name]
        e.matrix = matrix.normalized()
        e.length = max(length, .001)
        if b.parent:
            e.parent = new_data.edit_bones[b.parent.name]
    bpy.ops.object.mode_set(mode='OBJECT')
    arms = bpy.data.objects.new('Weighted arms '+kind, baked)
    bpy.context.collection.objects.link(arms)
    # new_from_object keeps deform vertex weights, but vertex-group names live on the
    # object; recreate the same indexed table before binding it to the retained skeleton.
    for group in mesh.vertex_groups:
        arms.vertex_groups.new(name=group.name)
    modifier = arms.modifiers.new('Runtime armature', 'ARMATURE')
    modifier.object = new_rig
    p.replace_materials([arms], mapping, arm=True)
    for o in imported:
        bpy.data.objects.remove(o, do_unlink=True)
    return arms, new_rig, grips


def export_selected(p, path, objects):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB', use_selection=True,
        export_apply=False, export_cameras=False, export_lights=False,
        export_animations=False, export_materials='EXPORT', export_yup=False)


def build(p, kind):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mapping = p.make_materials('scar-h' if kind == 'msr' else kind)
    arms, rig, grips = prepare_arms(p, kind, mapping)
    if kind != 'msr':
        gun = p.import_weapon(kind, mapping)
    else:
        old = set(bpy.context.scene.objects)
        bpy.ops.import_scene.gltf(filepath=str(p.SRC/'msr.glb'))
        imported = set(bpy.context.scene.objects)-old
        # The source optic is explicitly a Schmidt & Bender 5-25x56. Keep its
        # recognisable housing; its opaque source glass is replaced by a runtime lens.
        s = 1.25/(11.878555-(-13.479499))
        t = Matrix(((0,-s,0,.14),(0,0,s,-.22),
                    (-s,0,0,.22+s*(-13.479499)),(0,0,0,1)))
        gun = []
        for obj in imported:
            if obj.type != 'MESH':
                continue
            # Material.012 is the opaque source ocular/objective glass.
            if all(slot.material and slot.material.name.split('.')[0]=='Material'
                   and '.012' in slot.material.name for slot in obj.material_slots):
                continue
            data = obj.data.copy()
            data.transform(t @ obj.matrix_world)
            output = bpy.data.objects.new('Held MSR '+obj.name, data)
            bpy.context.collection.objects.link(output)
            gun.append(output)
        for obj in imported:
            bpy.data.objects.remove(obj, do_unlink=True)
    export_selected(p, p.ROOT/(f'held-{kind}.glb'), gun)
    export_selected(p, p.ROOT/(f'arms-{kind}.glb'), [arms, rig])
    # A small text sidecar preserves the authored wrist FRAME (column-major, metres).
    # Runtime movement transforms these frames before solving the arms each render.
    with (p.ROOT/(f'{kind}.grip')).open('w') as out:
        out.write('RIME_GRIP_V1\n')
        for frame in grips:
            out.write(' '.join(str(frame[r][c]) for c in range(4) for r in range(4))+'\n')
    # Editable art must remain usable after the temporary conversion directory disappears.
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(p.ROOT/(f'grip-{kind}.blend')))
    p.preview(kind, [arms]+gun)
    report = {'weapon': kind, 'wrist_frames': [[[float(v) for v in row] for row in m]
        for m in grips], 'arm_vertices': len(arms.data.vertices),
        'rig_bones': len(rig.data.bones), 'uniform_metres': True}
    (p.REPORT/(kind+'-rig.json')).write_text(json.dumps(report, indent=2))
