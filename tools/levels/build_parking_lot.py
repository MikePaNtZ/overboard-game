# build_parking_lot.py -- UE editor python. Builds OB_ParkingLot (Level 1, a parking-lot training
# circuit) from the files tools/levels/gen_parking_lot.py wrote: the asphalt lot, the ramp concrete
# and the grass verge (exact course heights), every obstacle as a box or a cone, the lane and stall
# markings, the pole_NE street light and the bin props, daylight, and the dressing outside the lot.
# Run headless (see tools/levels/build_parking_lot.sh):
#
#   OB_PL_DATA=/tmp/ob-levels-map/data tools/levels/ue.sh tools/levels/build_parking_lot.py
#
# HARD RULE: Unreal computes no board physics. Every mesh is the exact geometry MuJoCo owns, drawn
# from the exported OBM files, so an exporter bug shows on screen. Log: /tmp/ob-levels-map/build.txt.

import json
import math
import os

import unreal

DATA = os.environ.get("OB_PL_DATA", "/tmp/ob-levels-map/data")
LOG_PATH = os.environ.get("OB_BUILD_LOG", "/tmp/ob-levels-map/build.txt")

MAP = "/Game/Maps/OB_ParkingLot"
ROOT = "/Game/ParkingLot"
MATS = ROOT + "/Materials"
MESHES = ROOT + "/Meshes"

os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
_log = open(LOG_PATH, "w")


def log(*msg):
    _log.write(" ".join(str(m) for m in msg) + "\n")
    _log.flush()


def fail(msg):
    log("FAIL: " + msg)
    raise RuntimeError(msg)


def setp(obj, name, value):
    try:
        obj.set_editor_property(name, value)
    except Exception as e:
        log("  could not set %s.%s: %s" % (obj.__class__.__name__, name, e))


eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.TrailBuildLibrary
meta = json.load(open(os.path.join(DATA, "meta.json")))
log("data", DATA)

USAGES = [unreal.MaterialUsage.MATUSAGE_NANITE, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
          unreal.MaterialUsage.MATUSAGE_STATIC_LIGHTING]
MP = unreal.MaterialProperty


def new_material(name):
    full = MATS + "/" + name
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    return tools.create_asset(name, MATS, unreal.Material, unreal.MaterialFactoryNew())


def node(mat, cls, n, **props):
    e = mel.create_material_expression(mat, cls, -350 * (1 + n // 20), 130 * (n % 20))
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def flat_material(name, color, rough=0.85, spec=0.3, noise_amt=0.0, emissive=None, metallic=0.0):
    """A plain PBR material: a base colour, a roughness, and an optional faint world-space noise so
    a big flat surface is not a dead sheet. Procedural, so the build needs no texture content."""
    mat = new_material(name)
    base = node(mat, unreal.MaterialExpressionConstant3Vector, 0,
                constant=unreal.LinearColor(color[0], color[1], color[2], 1))
    if noise_amt > 0:
        wp = node(mat, unreal.MaterialExpressionWorldPosition, 1)
        ns = node(mat, unreal.MaterialExpressionNoise, 2, scale=0.01, levels=3,
                  output_min=1.0 - noise_amt, output_max=1.0 + noise_amt, quality=1, turbulence=False)
        mel.connect_material_expressions(wp, "", ns, "Position")
        mul = node(mat, unreal.MaterialExpressionMultiply, 3)
        mel.connect_material_expressions(base, "", mul, "A")
        mel.connect_material_expressions(ns, "", mul, "B")
        base = mul
    mel.connect_material_property(base, "", MP.MP_BASE_COLOR)
    r = node(mat, unreal.MaterialExpressionConstant, 10, r=float(rough))
    mel.connect_material_property(r, "", MP.MP_ROUGHNESS)
    sp = node(mat, unreal.MaterialExpressionConstant, 11, r=float(spec))
    mel.connect_material_property(sp, "", MP.MP_SPECULAR)
    if metallic > 0:
        mt = node(mat, unreal.MaterialExpressionConstant, 12, r=float(metallic))
        mel.connect_material_property(mt, "", MP.MP_METALLIC)
    if emissive is not None:
        em = node(mat, unreal.MaterialExpressionConstant3Vector, 13,
                  constant=unreal.LinearColor(emissive[0], emissive[1], emissive[2], 1))
        mel.connect_material_property(em, "", MP.MP_EMISSIVE_COLOR)
    for u in USAGES:
        mel.set_material_usage(mat, u)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    errs = lib.get_material_compile_errors(mat)
    eal.save_asset(mat.get_path_name())
    log("material %s%s" % (name, "" if not errs else "  COMPILE ERRORS: " + " | ".join(errs)))
    return mat


def spawn(cls, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0, label=None):
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw))
    if not a:
        fail("spawn %s failed" % cls)
    if label:
        a.set_actor_label(label)
    return a


# --- daylight look -------------------------------------------------------------------------------
def build_look():
    sun = spawn(unreal.DirectionalLight, (0, 0, 3000), pitch=-42.0, yaw=35.0, label="OB_Sun")
    sc = sun.light_component
    setp(sc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(sc, "intensity", 75000.0)
    setp(sc, "use_temperature", True)
    setp(sc, "temperature", 5600.0)
    setp(sc, "atmosphere_sun_light", True)
    setp(sc, "cast_shadows", True)
    setp(sc, "dynamic_shadow_distance_movable_light", 40000.0)

    spawn(unreal.SkyAtmosphere, (0, 0, 0), label="OB_SkyAtmosphere")

    sky = spawn(unreal.SkyLight, (0, 0, 2000), label="OB_SkyLight")
    kc = sky.light_component
    setp(kc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(kc, "real_time_capture", True)
    setp(kc, "intensity", 1.0)

    clouds = spawn(unreal.VolumetricCloud, (0, 0, 0), label="OB_Clouds")
    ccomp = clouds.get_component_by_class(unreal.VolumetricCloudComponent)
    setp(ccomp, "layer_bottom_altitude", 3.0)
    setp(ccomp, "layer_height", 8.0)

    ppv = spawn(unreal.PostProcessVolume, (0, 0, 0), label="OB_Look")
    setp(ppv, "unbound", True)
    setp(ppv, "priority", 100.0)
    s = ppv.settings
    PP = dict(
        dynamic_global_illumination_method=unreal.DynamicGlobalIlluminationMethod.LUMEN,
        reflection_method=unreal.ReflectionMethod.LUMEN,
        auto_exposure_method=unreal.AutoExposureMethod.AEM_MANUAL,
        auto_exposure_apply_physical_camera_exposure=False,
        auto_exposure_bias=-14.0,
        bloom_intensity=0.4, vignette_intensity=0.3,
    )
    for k, v in PP.items():
        try:
            s.set_editor_property(k, v)
            s.set_editor_property("override_" + k, True)
        except Exception as e:
            log("  pp %s: %s" % (k, e))
    ppv.set_editor_property("settings", s)


# --- build ---------------------------------------------------------------------------------------
les.load_level("/Game/Maps/OB_Main")
if eal.does_asset_exist(MAP):
    fail("%s exists; build_parking_lot.sh deletes it before the editor starts" % MAP)
for d in (MESHES, MATS):
    if eal.does_directory_exist(d):
        eal.delete_directory(d)
log("cleaned")

mats = dict(
    Asphalt=flat_material("M_PL_Asphalt", (0.055, 0.055, 0.058), rough=0.9, spec=0.2, noise_amt=0.12),
    Concrete=flat_material("M_PL_Concrete", (0.34, 0.33, 0.31), rough=0.85, spec=0.25, noise_amt=0.08),
    Grass=flat_material("M_PL_Grass", (0.045, 0.11, 0.035), rough=0.95, spec=0.1, noise_amt=0.25),
    Wood=flat_material("M_PL_Wood", (0.19, 0.12, 0.06), rough=0.7, spec=0.3),
    Metal=flat_material("M_PL_Metal", (0.30, 0.31, 0.33), rough=0.4, spec=0.6, metallic=0.9),
    Cone=flat_material("M_PL_Cone", (0.85, 0.22, 0.03), rough=0.6, spec=0.3, emissive=(0.25, 0.05, 0.0)),
    White=flat_material("M_PL_White", (0.75, 0.75, 0.72), rough=0.6, spec=0.3),
    Yellow=flat_material("M_PL_Yellow", (0.75, 0.62, 0.05), rough=0.6, spec=0.3),
    Magenta=flat_material("M_PL_Magenta", (0.75, 0.05, 0.55), rough=0.6, spec=0.3, emissive=(0.3, 0.0, 0.22)),
    Massing=flat_material("M_PL_Massing", (0.22, 0.21, 0.20), rough=0.8, spec=0.3, noise_amt=0.1),
)


def make_mesh(obm_name, mesh_name, nanite=True):
    path = MESHES + "/" + mesh_name
    m = lib.create_static_mesh_from_obm(os.path.join(DATA, obm_name), path + "." + mesh_name, nanite, 0, False)
    if not m:
        fail("mesh %s from %s" % (mesh_name, obm_name))
    for i, sm in enumerate(m.get_editor_property("static_materials")):
        slot = str(sm.material_slot_name)
        if slot in mats:
            m.set_material(i, mats[slot])
        else:
            log("  mesh %s: unmapped slot '%s'" % (mesh_name, slot))
    eal.save_asset(path)
    return m


def sm_actor(mesh, label, collide=True):
    a = spawn(unreal.StaticMeshActor, label=label)
    smc = a.static_mesh_component
    smc.set_static_mesh(mesh)
    smc.set_mobility(unreal.ComponentMobility.STATIC)
    if collide:
        smc.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
        smc.set_collision_profile_name("BlockAll")
    return a


# create the mesh ASSETS while OB_Main is loaded (shaders compile in the full editor); the actors
# are spawned only AFTER the fresh map exists, because new_blank_map clears the current level.
ntile = 0
while os.path.exists(os.path.join(DATA, "ground_lot_%d.obm" % ntile)):
    make_mesh("ground_lot_%d.obm" % ntile, "SM_PL_Lot_%d" % ntile)
    ntile += 1
log("built %d lot tile meshes" % ntile)
for obm_name, mesh_name in (("ground_ramp.obm", "SM_PL_Ramp"), ("ground_verge.obm", "SM_PL_Verge"),
                            ("boxes.obm", "SM_PL_Boxes"), ("cones.obm", "SM_PL_Cones"),
                            ("marks.obm", "SM_PL_Marks")):
    make_mesh(obm_name, mesh_name)
log("ground, boxes, cones and marks meshes built")

# a fresh map, the PlayerStart at the world origin
world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
if not world:
    fail("new_blank_map failed")
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("first save_map(%s) failed" % MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def place_all_meshes():
    ntile = 0
    while eal.does_asset_exist(MESHES + "/SM_PL_Lot_%d" % ntile):
        sm_actor(unreal.load_asset(MESHES + "/SM_PL_Lot_%d" % ntile), "OB_PL_Lot_%d" % ntile)
        ntile += 1
    for mesh_name, label, collide in (("SM_PL_Ramp", "OB_PL_Ramp", True), ("SM_PL_Verge", "OB_PL_Verge", True),
                                      ("SM_PL_Boxes", "OB_PL_Boxes", True), ("SM_PL_Cones", "OB_PL_Cones", True),
                                      ("SM_PL_Marks", "OB_PL_Marks", False)):
        sm_actor(unreal.load_asset(MESHES + "/" + mesh_name), label, collide)
    return ntile


ntile = place_all_meshes()
log("re-placed %d lot tiles + ground/boxes/cones/marks into %s" % (ntile, MAP))

ps = spawn(unreal.PlayerStart, tuple(meta["origin_cm"]), yaw=meta["origin_yaw_deg"], label="OB_ParkingLotOrigin")
log("PlayerStart at %s yaw %s" % (ps.get_actor_location(), ps.get_actor_rotation().yaw))


# --- props: the pole_NE street light and the bin ------------------------------------------------
PROP_MESH = {
    "street_lamp": "/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B",
    "trash_can": "/Game/Prop/Kit_Trashcan_A/Mesh/SM_Trashcan_A_01",
}


def ue_xy(x, y):
    return 100.0 * x, -100.0 * y


props = json.load(open(os.path.join(DATA, "props.json")))
for p in props:
    ux, uy = ue_xy(p["x"], p["y"])
    uz = 100.0 * p["z"]
    path = PROP_MESH.get(p["kind"], "")
    mesh = unreal.load_asset(path)
    if not mesh:
        log("  prop %s MESH MISSING (%s)" % (p["kind"], path))
        if p["kind"] == "street_lamp":        # the axis marker must stand, so fall back to a box
            a = sm_actor(unreal.load_asset("/Engine/BasicShapes/Cube"), "OB_PL_PoleNE")
            a.set_actor_location(unreal.Vector(ux, uy, uz + 300.0), False, False)
            a.set_actor_scale3d(unreal.Vector(0.3, 0.3, 6.0))
        continue
    a = sm_actor(mesh, "OB_PL_%s" % p["kind"], collide=False)
    a.set_actor_location(unreal.Vector(ux, uy, uz), False, False)
    a.set_actor_rotation(unreal.Rotator(0.0, 0.0, -p["yaw"]), False)
    if p.get("light"):
        lt = spawn(unreal.PointLight, (ux, uy, uz + 600.0), label="OB_PL_PoleNE_Light")
        lc = lt.light_component
        setp(lc, "mobility", unreal.ComponentMobility.MOVABLE)
        setp(lc, "intensity", 20000.0)
        setp(lc, "attenuation_radius", 2500.0)
        setp(lc, "use_temperature", True)
        setp(lc, "temperature", 3200.0)
log("placed %d props" % len(props))


# --- dressing OUTSIDE the lot only (never inside x[-60,60] y[-40,40]) ----------------------------
def dressing():
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    holder_n = 0
    # a ring of massing blocks beyond the grass verge, so the horizon reads as a built edge
    ring = []
    for x in range(-90, 91, 18):
        ring.append((x, 58.0)); ring.append((x, -58.0))
    for y in range(-54, 55, 18):
        ring.append((82.0, y)); ring.append((-82.0, y))
    for (x, y) in ring:
        hgt = 6.0 + 3.0 * ((x * 7 + y) % 5)
        ux, uy = ue_xy(float(x), float(y))
        a = spawn(unreal.StaticMeshActor, (ux, uy, hgt * 100.0 / 2.0), label="OB_PL_Massing_%d" % holder_n)
        a.static_mesh_component.set_static_mesh(cube)
        a.static_mesh_component.set_material(0, mats["Massing"])
        a.set_actor_scale3d(unreal.Vector(0.12, 0.12, hgt))
        a.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
        holder_n += 1
    # trees and street lamps on the verge band, clear of the lot
    tree = unreal.load_asset("/Game/Prop/Kit_Tree_Maple_Red/Mesh/Tree_Maple_Red_A")
    lamp = unreal.load_asset("/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B")
    verge = []
    for x in range(-60, 61, 15):
        verge.append((float(x), 43.5, tree)); verge.append((float(x), -43.5, tree))
    for y in range(-38, 39, 19):
        verge.append((63.5, float(y), lamp)); verge.append((-63.5, float(y), lamp))
    for (x, y, mesh) in verge:
        if mesh is None:
            continue
        ux, uy = ue_xy(x, y)
        a = sm_actor(mesh, "OB_PL_Dress_%d" % holder_n, collide=False)
        a.set_actor_location(unreal.Vector(ux, uy, 0.0), False, False)
        holder_n += 1
    log("dressing: %d actors" % holder_n)


dressing()
build_look()

if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map failed")
log("saved %s" % MAP)
log("DONE")
_log.close()
