# Split the X7 exterior GLB (overboard-viz-kit) into the frame and the motor; see build_x7_skin.sh.
# Split the X7 exterior GLB into a static frame and the spinning motor, both with the AXLE as origin.
import bpy, sys
src, out_dir = sys.argv[sys.argv.index("--") + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
AXLE_Z = 0.146   # Blender Z up after import: the axle is 0.146 m above the GLB origin (ground contact)
objs = [o for o in bpy.context.scene.objects]
def under(o, name):
    while o:
        if o.name.split(".")[0] == name:
            return True
        o = o.parent
    return False
motor = [o for o in objs if o.type == "MESH" and under(o, "motor")]
frame = [o for o in objs if o.type == "MESH" and not under(o, "motor")]
print("motor meshes", len(motor), "frame meshes", len(frame))
for o in bpy.context.scene.objects:
    o.location.z -= AXLE_Z if o.parent is None else 0.0
bpy.context.view_layer.update()
def export(sel, path):
    # one joined mesh per file, so Unreal makes one static mesh with one slot per material
    bpy.ops.object.select_all(action="DESELECT")
    for o in sel:
        o.select_set(True)
        m = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = m
    bpy.context.view_layer.objects.active = sel[0]
    bpy.ops.object.join()
    j = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bpy.ops.object.select_all(action="DESELECT")
    j.select_set(True)
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True)
# pad tops, for the rider's deck height
import mathutils
for o in frame:
    if o.name.startswith("pad"):
        zs = [(o.matrix_world @ mathutils.Vector(c)).z for c in o.bound_box]
        xs = [(o.matrix_world @ mathutils.Vector(c)).x for c in o.bound_box]
        print("PAD %s top %.4f m above axle, x %.3f..%.3f" % (o.name, max(zs), min(xs), max(xs)))
export(frame, out_dir + "/x7_frame.glb")
export(motor, out_dir + "/x7_motor.glb")
