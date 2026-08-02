import bpy, math, os
from mathutils import Vector
OUT="/tmp/obshots"
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath="/Users/mike/Downloads/onewheel-pint/prepared/OneWheelPint_prepared.fbx")
sc=bpy.context.scene
sc.render.engine='BLENDER_EEVEE'
sc.render.resolution_x=1600; sc.render.resolution_y=900
sc.render.film_transparent=False
sc.eevee.taa_render_samples=64
try: sc.eevee.use_raytracing=True
except Exception: pass

# ground
bpy.ops.mesh.primitive_plane_add(size=40, location=(0,0,-0.1454))
gp=bpy.context.active_object
gm=bpy.data.materials.new("Ground"); gm.use_nodes=True
bsdf=gm.node_tree.nodes["Principled BSDF"]
bsdf.inputs["Base Color"].default_value=(0.20,0.21,0.23,1)
bsdf.inputs["Roughness"].default_value=0.65
gp.data.materials.append(gm)

# world
w=bpy.data.worlds.new("W"); sc.world=w; w.use_nodes=True
w.node_tree.nodes["Background"].inputs[0].default_value=(0.28,0.34,0.42,1)
w.node_tree.nodes["Background"].inputs[1].default_value=1.2

# key + fill
bpy.ops.object.light_add(type='SUN', location=(3,-4,6))
sun=bpy.context.active_object; sun.data.energy=4.0; sun.data.angle=0.08
sun.rotation_euler=(math.radians(50), 0, math.radians(35))
bpy.ops.object.light_add(type='AREA', location=(-3,3,3))
fl=bpy.context.active_object; fl.data.energy=220; fl.data.size=5
fl.rotation_euler=(math.radians(-50),0,math.radians(-140))

cam_data=bpy.data.cameras.new("C"); cam=bpy.data.objects.new("C",cam_data)
sc.collection.objects.link(cam); sc.camera=cam
TARGET=Vector((0,0,-0.02))
def shoot(name, pos, lens=60):
    cam_data.lens=lens
    cam.location=Vector(pos)
    d=TARGET-cam.location
    cam.rotation_euler=d.to_track_quat('-Z','Y').to_euler()
    sc.render.filepath=os.path.join(OUT,name)
    bpy.ops.render.render(write_still=True)
    print("RENDERED",name)

shoot("board_01_three_quarter", (0.95,-1.05,0.42), 62)
shoot("board_02_side",          (0.02,-1.35,0.16), 55)
shoot("board_03_front",         (1.30,-0.10,0.28), 58)
shoot("board_04_low_hero",      (0.80,-0.85,0.05), 48)
