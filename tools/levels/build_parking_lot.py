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


# --- Megascans textured materials (world-space UVs, so they tile on any scaled mesh) -------------
MS = "/Game/Megascans/Surfaces"
TEX = dict(
    asphalt=(MS + "/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Albedo",
             MS + "/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Normal",
             MS + "/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Roughness"),
    concrete=(MS + "/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Albedo",
              MS + "/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Normal",
              MS + "/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Roughness"),
)


def sampler_for(tex):
    """Pick the sampler type that matches the texture, including the Virtual variants (the Megascans
    8K surfaces are streamed virtual textures, so a plain Color/Normal sampler fails to compile)."""
    cs = tex.get_editor_property("compression_settings")
    srgb = tex.get_editor_property("srgb")
    vt = tex.get_editor_property("virtual_texture_streaming")
    T = unreal.MaterialSamplerType
    if cs == unreal.TextureCompressionSettings.TC_NORMALMAP:
        return T.SAMPLERTYPE_VIRTUAL_NORMAL if vt else T.SAMPLERTYPE_NORMAL
    if cs == unreal.TextureCompressionSettings.TC_MASKS:
        return T.SAMPLERTYPE_VIRTUAL_MASKS if vt else T.SAMPLERTYPE_MASKS
    if cs == unreal.TextureCompressionSettings.TC_ALPHA:
        return T.SAMPLERTYPE_VIRTUAL_ALPHA if vt else T.SAMPLERTYPE_ALPHA
    if cs == unreal.TextureCompressionSettings.TC_GRAYSCALE:
        if vt:
            return T.SAMPLERTYPE_VIRTUAL_GRAYSCALE if srgb else T.SAMPLERTYPE_VIRTUAL_LINEAR_GRAYSCALE
        return T.SAMPLERTYPE_GRAYSCALE if srgb else T.SAMPLERTYPE_LINEAR_GRAYSCALE
    if vt:
        return T.SAMPLERTYPE_VIRTUAL_COLOR if srgb else T.SAMPLERTYPE_VIRTUAL_LINEAR_COLOR
    return T.SAMPLERTYPE_COLOR if srgb else T.SAMPLERTYPE_LINEAR_COLOR


def textured_material(name, key, tile_cm, tint=(1, 1, 1), spec=0.25):
    """A Megascans surface in world space: albedo tinted, normal, roughness. If a texture is
    missing the build falls back to a flat colour, so a missing pack never fails the build."""
    a_p, n_p, r_p = TEX[key]
    ta, tn, tr = unreal.load_asset(a_p), unreal.load_asset(n_p), unreal.load_asset(r_p)
    if not (ta and tn and tr):
        log("  textured %s MISSING (%s); using a flat colour" % (name, a_p))
        return flat_material(name, [c * 0.3 for c in tint], rough=0.85, spec=spec, noise_amt=0.1)
    mat = new_material(name)
    wp = node(mat, unreal.MaterialExpressionWorldPosition, 0)
    mask = node(mat, unreal.MaterialExpressionComponentMask, 1, r=True, g=True)
    mel.connect_material_expressions(wp, "", mask, "")
    div = node(mat, unreal.MaterialExpressionDivide, 2)
    mel.connect_material_expressions(mask, "", div, "A")
    tc = node(mat, unreal.MaterialExpressionConstant, 3, r=float(tile_cm))
    mel.connect_material_expressions(tc, "", div, "B")
    col = node(mat, unreal.MaterialExpressionTextureSample, 10, texture=ta, sampler_type=sampler_for(ta))
    mel.connect_material_expressions(div, "", col, "UVs")
    tnt = node(mat, unreal.MaterialExpressionMultiply, 11)
    tcol = node(mat, unreal.MaterialExpressionConstant3Vector, 12,
                constant=unreal.LinearColor(tint[0], tint[1], tint[2], 1))
    mel.connect_material_expressions(col, "", tnt, "A")
    mel.connect_material_expressions(tcol, "", tnt, "B")
    mel.connect_material_property(tnt, "", MP.MP_BASE_COLOR)
    nrm = node(mat, unreal.MaterialExpressionTextureSample, 13, texture=tn, sampler_type=sampler_for(tn))
    mel.connect_material_expressions(div, "", nrm, "UVs")
    mel.connect_material_property(nrm, "", MP.MP_NORMAL)
    rgh = node(mat, unreal.MaterialExpressionTextureSample, 14, texture=tr, sampler_type=sampler_for(tr))
    mel.connect_material_expressions(div, "", rgh, "UVs")
    r_chan = "A" if tr.get_editor_property("compression_settings") == unreal.TextureCompressionSettings.TC_ALPHA else "R"
    mel.connect_material_property(rgh, r_chan, MP.MP_ROUGHNESS)
    sp = node(mat, unreal.MaterialExpressionConstant, 15, r=float(spec))
    mel.connect_material_property(sp, "", MP.MP_SPECULAR)
    for u in USAGES:
        mel.set_material_usage(mat, u)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    errs = lib.get_material_compile_errors(mat)
    eal.save_asset(mat.get_path_name())
    log("material %s (textured %s)%s" % (name, key, "" if not errs else "  COMPILE ERRORS: " + " | ".join(errs)))
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
    sun = spawn(unreal.DirectionalLight, (0, 0, 3000), pitch=-53.0, yaw=315.0, label="OB_Sun")
    sc = sun.light_component
    setp(sc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(sc, "intensity", 90000.0)
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
    setp(kc, "intensity", 2.0)

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
    Asphalt=textured_material("M_PL_Asphalt", "asphalt", 400.0, tint=(0.42, 0.44, 0.48), spec=0.2),
    Concrete=textured_material("M_PL_Concrete", "concrete", 300.0, tint=(0.95, 0.93, 0.9), spec=0.25),
    Grass=flat_material("M_PL_Grass", (0.045, 0.11, 0.035), rough=0.95, spec=0.1, noise_amt=0.25),
    Canopy=flat_material("M_PL_Canopy", (0.06, 0.16, 0.05), rough=0.9, spec=0.1, noise_amt=0.35),
    Wood=flat_material("M_PL_Wood", (0.19, 0.12, 0.06), rough=0.7, spec=0.3),
    Metal=flat_material("M_PL_Metal", (0.30, 0.31, 0.33), rough=0.4, spec=0.6, metallic=0.9),
    Cone=flat_material("M_PL_Cone", (0.85, 0.22, 0.03), rough=0.6, spec=0.3, emissive=(0.5, 0.1, 0.0)),
    # markings read in any light: a bright base plus a matching emissive so they are never crushed
    White=flat_material("M_PL_White", (0.82, 0.82, 0.80), rough=0.55, spec=0.2, emissive=(0.35, 0.35, 0.34)),
    Yellow=flat_material("M_PL_Yellow", (0.82, 0.68, 0.04), rough=0.55, spec=0.2, emissive=(0.45, 0.36, 0.0)),
    Black=flat_material("M_PL_Black", (0.02, 0.02, 0.02), rough=0.7, spec=0.1),
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


# --- surrounding world: no black void; a city parking lot ----------------------------------------
# Hero buildings: (path, ground Z offset, height m). A packed level actor shows nothing in the
# headless editor, so the base Z is measured from each building's referenced SUBLEVEL map (the
# meshes live there): base +0 / +500 / +548 cm, so the spawn Z is the negative of that. SFC_A and
# SFC_B are tall towers (138 / 170 m); SFD_Long is low (12 m). The near ring uses only the LOW
# building, so it does not shadow the lot; the two towers stand far to the north as a skyline.
BLD = dict(
    SFD=("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Low_SFD_Long_01", -548.0),
    SFC_A=("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_A01", 0.0),
    SFC_B=("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_B01", -500.0),
)


def building_class(path):
    bp = unreal.load_asset(path)
    if bp is None:
        return None
    if isinstance(bp, unreal.Blueprint):
        return bp.generated_class()
    return unreal.load_object(None, path + "_C")


def ground_box(x_m, y_m, lx, ly, top_z, thick, mat, label):
    """A flat slab (MuJoCo metres) with its top at top_z, for the streets, pavement and grass."""
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    ux, uy = ue_xy(x_m, y_m)
    a = spawn(unreal.StaticMeshActor, (ux, uy, (top_z - thick / 2) * 100.0), label=label)
    a.static_mesh_component.set_static_mesh(cube)
    a.static_mesh_component.set_material(0, mat)
    a.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    a.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    a.set_actor_scale3d(unreal.Vector(lx, ly, thick))


def dressing():
    n = 0
    # a big grass plane to the far horizon, just below the lot, so the world is never black
    ground_box(0, 0, 900.0, 900.0, -0.02, 0.4, mats["Grass"], "OB_PL_Grass_Plane")
    # a street on two sides (south along X, east along Y), with a concrete pavement beside each
    ground_box(0, -58.0, 220.0, 16.0, 0.0, 0.3, mats["Asphalt"], "OB_PL_Street_S")
    ground_box(80.0, 0.0, 16.0, 190.0, 0.0, 0.3, mats["Asphalt"], "OB_PL_Street_E")
    ground_box(0, -49.5, 220.0, 7.0, 0.01, 0.2, mats["Concrete"], "OB_PL_Pave_S")
    ground_box(71.5, 0.0, 7.0, 190.0, 0.01, 0.2, mats["Concrete"], "OB_PL_Pave_E")

    # The low building rings the lot ~55 m outside the verge, with gaps, so it does not shadow the
    # lot. Two tall towers stand far to the north as a skyline. All render only in the -game MRQ
    # still (packed level actors). Each tuple: (key, x_m, y_m, yaw).
    placements = []
    for x in (-66.0, -14.0, 34.0):                        # north row, facing south
        placements.append(("SFD", x, 104.0, 180.0))
    for x in (-54.0, 10.0):                               # beyond the south street, facing north
        placements.append(("SFD", x, -104.0, 0.0))
    for y in (-30.0, 34.0):                               # west row, facing east
        placements.append(("SFD", -118.0, y, 270.0))
    for y in (-34.0, 30.0):                               # beyond the east street, facing west
        placements.append(("SFD", 126.0, y, 90.0))
    placements.append(("SFC_A", -55.0, 230.0, 180.0))     # far-north skyline towers
    placements.append(("SFC_B", 70.0, 250.0, 180.0))
    for i, (key, x, y, yaw) in enumerate(placements):
        path, zoff = BLD[key]
        cls = building_class(path)
        if cls is None:
            log("  building MISSING %s" % path)
            continue
        ux, uy = ue_xy(x, y)
        spawn(cls, (ux, uy, zoff), yaw=yaw, label="OB_PL_Bldg_%d" % i)
        n += 1
    log("buildings placed: %d" % n)

    # leafed trees along the verge (a green canopy proxy, since the City Sample street trees are a
    # bare-branch winter variant): a brown trunk and a green canopy, so the verge reads as summer.
    for x in range(-50, 51, 25):
        proxy_tree(x, 46.5, n); n += 1
        proxy_tree(x, -46.5, n); n += 1
    # street lamps along the two streets
    lamp = unreal.load_asset("/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B")
    spots = [(67.0, float(y)) for y in range(-30, 31, 30)]
    spots += [(float(x), -54.0) for x in range(-45, 46, 30)]
    for x, y in spots:
        if not lamp:
            continue
        ux, uy = ue_xy(x, y)
        a = sm_actor(lamp, "OB_PL_Lamp_%d" % n, collide=False)
        a.set_actor_location(unreal.Vector(ux, uy, 0.0), False, False)
        n += 1
    log("dressing: %d actors total" % n)


def proxy_tree(x_m, y_m, i):
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    sphere = unreal.load_asset("/Engine/BasicShapes/Sphere")
    ux, uy = ue_xy(float(x_m), float(y_m))
    trunk = spawn(unreal.StaticMeshActor, (ux, uy, 150.0), label="OB_PL_TreeTrunk_%d" % i)
    trunk.static_mesh_component.set_static_mesh(cube)
    trunk.static_mesh_component.set_material(0, mats["Wood"])
    trunk.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    trunk.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    trunk.set_actor_scale3d(unreal.Vector(0.35, 0.35, 3.0))
    canopy = spawn(unreal.StaticMeshActor, (ux, uy, 420.0), label="OB_PL_TreeCanopy_%d" % i)
    canopy.static_mesh_component.set_static_mesh(sphere)
    canopy.static_mesh_component.set_material(0, mats["Canopy"])
    canopy.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    canopy.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    canopy.set_actor_scale3d(unreal.Vector(2.6, 2.6, 2.2))


dressing()
build_look()

# NoGround: no motion-reference markers or placeholder ground on a real level.
world.get_world_settings().set_editor_property("default_game_mode", unreal.load_class(
    None, "/Script/OverboardGame.OverboardGameMode_NoGround"))
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map failed")
log("saved %s" % MAP)
log("DONE")
_log.close()
