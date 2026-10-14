# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Bake two static first-person, camera-local, metal-rough GLBs from CC BY sources.

Run from the repository root with:  blender -b -t 4 --python <this file>
Inputs: only the original GLBs in ../source/. Outputs: ../held-scar-h.glb, ../held-m4a1.glb,
plus preview renders in $RIME_PREVIEW_DIR (default: ./viewmodel-previews).

Why textures come from the images EMBEDDED in those GLBs: the textures that Sketchfab's
public media URLs serve for these models are corrupt. Each decodes as a valid PNG at its
advertised size but the pixels are a ~45-degree diagonal shear -- pixel data reinterpreted
at the wrong row stride or channel count -- and the damage is in the served bytes, so it
cannot be undone. An earlier version of this script used those files (and larger, "better"
resolutions), which is how a checkerboarded sleeve and blown-out skin shipped. The embedded
1024x1024 images are intact, and 1024 is their real resolution; nothing here is upscaled.
SCAR-H and the arms use KHR_materials_pbrSpecularGlossiness, so their spec/gloss images are
converted to metal/rough (see viewmodel_textures.py); the M4A1 already ships metal/rough.
Derivative CC BY 4.0 artwork: do not add project code-license headers to the GLBs.
"""
import bpy
import math
import os
import sys
import tempfile
from pathlib import Path
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from viewmodel_textures import extract_glb_images, specgloss_to_metalrough

ROOT = HERE.parent
REPORT = Path(os.environ.get('RIME_PREVIEW_DIR', 'viewmodel-previews')).resolve()
REPORT.mkdir(parents=True, exist_ok=True)
SRC = ROOT / 'source'
WORK = Path(tempfile.mkdtemp(prefix='viewmodel-tex-'))

def image(path, noncolor=False):
    im = bpy.data.images.load(str(path), check_existing=True)
    im.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
    return im

def pbr_material(name, base_path, mr_path, normal_path, rough_factor=1.0,
                 alpha=False, emissive_path=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    links = mat.node_tree.links
    out = nodes.new('ShaderNodeOutputMaterial')
    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.inputs['Roughness'].default_value = rough_factor
    links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    def tex(path, noncolor=False):
        node = nodes.new('ShaderNodeTexImage')
        node.image = image(path, noncolor)
        return node
    base = tex(base_path)
    links.new(base.outputs['Color'], bsdf.inputs['Base Color'])
    if alpha:
        links.new(base.outputs['Alpha'], bsdf.inputs['Alpha'])
        mat.surface_render_method = 'DITHERED'
        mat.use_transparency_overlap = False
    packed = tex(mr_path, True)
    sep = nodes.new('ShaderNodeSeparateColor')
    links.new(packed.outputs['Color'], sep.inputs['Color'])
    links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
    # Preserve M4 glTF roughness factor: the packed texture's G channel is 1.
    if rough_factor == 1.0:
        links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
    normal_tex = tex(normal_path, True)
    norm = nodes.new('ShaderNodeNormalMap')
    links.new(normal_tex.outputs['Color'], norm.inputs['Color'])
    links.new(norm.outputs['Normal'], bsdf.inputs['Normal'])
    if emissive_path:
        em = tex(emissive_path)
        links.new(em.outputs['Color'], bsdf.inputs['Emission Color'])
        bsdf.inputs['Emission Strength'].default_value = 0.15
    return mat

def make_materials(kind):
    # Embedded-image indices below were read from each source GLB's material JSON
    # (diffuse / specGloss-or-metalRough / normal), not guessed.
    arms_img = extract_glb_images(SRC/'fps-arms.glb', WORK, 'arms')
    specgloss_to_metalrough(arms_img[1], WORK/'arms-metalrough.png')
    arms = pbr_material('FPS arms skin and fabric', arms_img[0],
                        WORK/'arms-metalrough.png', arms_img[2])
    if kind == 'scar-h':
        s = extract_glb_images(SRC/'scar-h.glb', WORK, 'scar')
        specgloss_to_metalrough(s[1], WORK/'scar-metalrough.png')  # shared by both materials
        # 'detail' is the opaque body map; 'lowpoly' carries an alpha cutout in its diffuse.
        detail = pbr_material('SCAR-H detail painted metal and polymer', s[0],
                              WORK/'scar-metalrough.png', s[2])
        lowpoly = pbr_material('SCAR-H lowpoly painted metal and polymer', s[4],
                               WORK/'scar-metalrough.png', s[6], alpha=True)
        return {'detailinitialShadingGroup':detail,'lowpolyinitialShadingGroup':lowpoly,
                'material':arms,'arm-material':arms}
    m = extract_glb_images(SRC/'m4a1.glb', WORK, 'm4')
    # source material -> (part, base, metalRough, normal, roughness factor from the GLB)
    parts = {'Body1':('Body',12,13,14,0.4),'lambert4':('Stock',6,7,8,0.5),
             'lambert15':('Scope',9,10,11,0.4),'material':('Mag',0,1,2,0.4),
             'lambert16':('Bullet',3,4,5,0.4)}
    mats={'arm-material':arms}
    for src,(part,b,mr,n,rf) in parts.items():
        # The M4A1 already ships metal/rough: use its embedded packed image as-is.
        mats[src] = pbr_material(f'M4A1 {part} metalrough', m[b], m[mr], m[n], rough_factor=rf)
    return mats

def replace_materials(objects, mapping, arm=False):
    for obj in objects:
        for i, slot in enumerate(obj.material_slots):
            key = 'arm-material' if arm else (slot.material.name if slot.material else '')
            if not arm and key.rsplit('.', 1)[-1].isdigit():
                key = key.rsplit('.', 1)[0]
            if key in mapping:
                obj.data.materials[i] = mapping[key]

def bake_arms(mapping):
    bpy.ops.import_scene.gltf(filepath=str(SRC/'fps-arms.glb'))
    armature = next(o for o in bpy.context.scene.objects if o.type=='ARMATURE')
    mesh = next(o for o in bpy.context.scene.objects if o.type=='MESH' and o.name!='Icosphere')
    for o in list(bpy.context.scene.objects):
        if o.name=='Icosphere': bpy.data.objects.remove(o, do_unlink=True)
    # Source arm coordinates are centimetres: shoulders near y=+18, palms near y=-30.
    # IK shifts the left support palm to the fore-end and the right palm onto the pistol grip.
    for name, xyz in [('L_wrist_03',(0.0,-40.0,-5.0)),
                      ('R_wrist_025',(-5.0,-31.0,2.0))]:
        target=bpy.data.objects.new(name+'-grip-target',None)
        bpy.context.collection.objects.link(target)
        target.location=xyz
        bone=armature.pose.bones[name]
        ik=bone.constraints.new('IK')
        ik.target=target
        ik.chain_count=3
        ik.use_stretch=False
    bpy.context.view_layer.update()
    deps=bpy.context.evaluated_depsgraph_get()
    baked=bpy.data.meshes.new_from_object(mesh.evaluated_get(deps),
                                         preserve_all_data_layers=True,depsgraph=deps)
    baked.transform(mesh.matrix_world)
    obj=bpy.data.objects.new('Baked grip arms',baked)
    bpy.context.collection.objects.link(obj)
    replace_materials([obj],mapping,arm=True)
    # Source shoulders run toward +Y. Lower them out of frame while the wrists
    # stay near the weapon grips; map source -Y forward to camera -Z.
    scale=.01
    transform=Matrix(((-scale,0,0,.14),(0,-.008,scale,-.36),(0,scale,0,-.14),(0,0,0,1)))
    obj.data.transform(transform)
    for o in list(bpy.context.scene.objects):
        if o != obj: bpy.data.objects.remove(o,do_unlink=True)
    return obj

def weapon_transform(kind):
    if kind=='scar-h':
        s=1.0/(24.07-(-18.92))
        # +Y is the muzzle, +Z is up. Back of stock at z=-0.15m.
        return Matrix(((s,0,0,.14),(0,0,s,-.21),(0,-s,0,-.15+s*(-18.92)),(0,0,0,1)))
    s=.85/(-14.49-(-17.84))
    # M4 muzzle is +X; the source is offset by -16.68 in Y.
    return Matrix(((0,-s,0,.14-s*16.68),(0,0,s,-.21),(-s,0,0,-.15-s*17.84),(0,0,0,1)))

def import_weapon(kind,mapping):
    old=set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=str(SRC/(kind+'.glb')))
    imported=set(bpy.context.scene.objects)-old
    transformed=[]
    t=weapon_transform(kind)
    for mesh in [o for o in imported if o.type=='MESH']:
        data=mesh.data.copy()
        data.transform(t@mesh.matrix_world)
        obj=bpy.data.objects.new('Held '+kind+' '+mesh.name,data)
        bpy.context.collection.objects.link(obj)
        transformed.append(obj)
    replace_materials(transformed,mapping)
    for o in imported: bpy.data.objects.remove(o,do_unlink=True)
    return transformed

def bounds(objects):
    points=[obj.matrix_world@Vector(v) for obj in objects for v in obj.bound_box]
    return [[min(p[i] for p in points),max(p[i] for p in points)] for i in range(3)]

def preview(kind, objects):
    camera_data=bpy.data.cameras.new('First-person camera FOV 1.1 rad')
    camera=bpy.data.objects.new('Preview camera',camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location=(0,0,0)
    camera.rotation_euler=(0,0,0)
    camera_data.angle=1.1
    camera_data.clip_start=.01
    bpy.context.scene.camera=camera
    world=bpy.data.worlds.new('Preview neutral environment')
    world.use_nodes=True
    world.node_tree.nodes['Background'].inputs['Color'].default_value=(.1,.12,.15,1)
    world.node_tree.nodes['Background'].inputs['Strength'].default_value=.7
    bpy.context.scene.world=world
    for name,location,power,size in [('key',(-.8,.8,-.5),550,1.2),
                                      ('fill',(.6,.2,-1.0),280,.9)]:
        ld=bpy.data.lights.new(name,'AREA');ld.energy=power;ld.shape='DISK';ld.size=size
        ob=bpy.data.objects.new(name,ld);bpy.context.collection.objects.link(ob)
        ob.location=location;ob.rotation_euler=(Vector((.1,-.25,-.7))-ob.location).to_track_quat('-Z','Y').to_euler()
    scene=bpy.context.scene
    scene.render.engine='CYCLES'
    scene.cycles.samples=24
    scene.render.resolution_x=960;scene.render.resolution_y=540
    scene.render.resolution_percentage=100
    scene.render.image_settings.file_format='PNG'
    scene.render.filepath=str(REPORT/(f'held-{kind}-preview.png'))
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(camera,do_unlink=True)
    for o in list(bpy.context.scene.objects):
        if o.type=='LIGHT': bpy.data.objects.remove(o,do_unlink=True)

def build(kind):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mapping=make_materials(kind)
    arms=bake_arms(mapping)
    gun=import_weapon(kind,mapping)
    objects=[arms]+gun
    print('BOUNDS',kind,'all',bounds(objects),'arms',bounds([arms]),'weapon',bounds(gun),flush=True)
    for o in bpy.context.scene.objects: o.select_set(False)
    for o in objects: o.select_set(True)
    bpy.context.view_layer.objects.active=gun[0]
    output=ROOT/(f'held-{kind}.glb')
    bpy.ops.export_scene.gltf(filepath=str(output),export_format='GLB',use_selection=True,
                              export_apply=True,export_cameras=False,export_lights=False,
                              export_animations=False,export_materials='EXPORT')
    print('EXPORTED',output,output.stat().st_size,flush=True)
    preview(kind,objects)
    for obj in gun: obj.hide_render=True
    preview(kind+'-arms',[arms])
    for obj in gun: obj.hide_render=False

for kind in ('scar-h','m4a1'):
    build(kind)
