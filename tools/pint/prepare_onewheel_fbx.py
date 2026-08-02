import bpy, math, os
from mathutils import Vector, Matrix

SRC="/Users/mike/Downloads/onewheel-pint/source/OneWheelPint.fbx"
TEXDIR="/Users/mike/Downloads/onewheel-pint/textures"
OUT="/Users/mike/Downloads/onewheel-pint/prepared/OneWheelPint_prepared.fbx"

AXLE=Vector((0.0,0.0,-0.0953)); FBX_TIRE_R=1.0314; MUJOCO_TIRE_R=0.1454
TILT_DEG=23.54; SCALE=MUJOCO_TIRE_R/FBX_TIRE_R

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=SRC)

if 'Plane' in bpy.data.objects:
    bpy.data.objects.remove(bpy.data.objects['Plane'], do_unlink=True)
for old,new in {'Plateau.Central.001':'OW_Frame','Cylinder.010':'OW_Wheel_Tire',
                'Cylinder.011':'OW_Wheel_Hub'}.items():
    if old in bpy.data.objects: bpy.data.objects[old].name=new

X=(Matrix.Rotation(math.radians(180.0),4,'Z') @ Matrix.Scale(SCALE,4)
   @ Matrix.Rotation(math.radians(TILT_DEG),4,'Y') @ Matrix.Translation(-AXLE))
for o in bpy.data.objects:
    if o.type=='MESH': o.matrix_world = X @ o.matrix_world

for img in bpy.data.images:
    c=os.path.join(TEXDIR, os.path.basename(img.filepath))
    if os.path.exists(c): img.filepath=c

# --- THE FIX -------------------------------------------------------------
# Material.001 (tyre) and Material.002 (hub) shipped with Base Color UNLINKED, sitting on the
# Principled default of 0.8 white -- which is why both render white. Their albedo maps exist in
# the pack (Tire-a.png is a solid pure black, WheelPlate-a.png near-black) and were simply never
# wired. Bind them. Nothing here invents a colour: it is the artist's own texture, connected.
def bind_albedo(mat_name, tex_file):
    m=bpy.data.materials.get(mat_name)
    if not m or not m.node_tree: return "no material"
    nt=m.node_tree
    bsdf=next((n for n in nt.nodes if n.type=='BSDF_PRINCIPLED'), None)
    if not bsdf: return "no bsdf"
    if bsdf.inputs["Base Color"].is_linked: return "already linked"
    p=os.path.join(TEXDIR, tex_file)
    if not os.path.exists(p): return "missing "+tex_file
    img=bpy.data.images.load(p, check_existing=True)
    img.colorspace_settings.name='sRGB'
    tex=nt.nodes.new('ShaderNodeTexImage'); tex.image=img
    tex.location=(bsdf.location.x-500, bsdf.location.y+250)
    nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    return "bound "+tex_file

print("FIX tyre:", bind_albedo("Material.001","Tire-a.png"))
print("FIX hub :", bind_albedo("Material.002","WheelPlate-a.png"))

# data maps must not be read as sRGB
for m in bpy.data.materials:
    if not m.node_tree: continue
    for n in m.node_tree.nodes:
        if n.type!='TEX_IMAGE' or not n.image: continue
        nm=n.image.name.lower()
        if any(k in nm for k in ("-n.","_n.","-r.","_r.","-m.","_m.","-ao","_ao","-s.","_s.")):
            try: n.image.colorspace_settings.name='Non-Color'
            except Exception: pass

for o in bpy.data.objects: o.select_set(o.type=='MESH')
bpy.ops.export_scene.fbx(filepath=OUT, use_selection=True, path_mode='COPY',
                         embed_textures=False, apply_scale_options='FBX_SCALE_NONE',
                         axis_forward='-Z', axis_up='Y', object_types={'MESH'},
                         mesh_smooth_type='FACE', use_tspace=True)
print("EXPORTED", OUT)
for m in bpy.data.materials:
    b=next((n for n in m.node_tree.nodes if n.type=='BSDF_PRINCIPLED'), None) if m.node_tree else None
    if b: print(f"  {m.name:20s} BaseColor_linked={b.inputs['Base Color'].is_linked}")
