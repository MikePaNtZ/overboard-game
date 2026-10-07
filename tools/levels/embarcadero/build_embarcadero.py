# build_embarcadero.py -- UE editor python. Builds OB_Embarcadero (Level 2) from the files
# gen_embarcadero.py wrote: the ground mesh (below the exact surface), the exact ridden corridor,
# the kerb and rail boxes, the water plane, the OSM buildings, the street markings and the dressing.
# Run headless (see build_embarcadero.sh). HARD RULE: Unreal computes no board physics; every mesh
# is the exact geometry the exporter wrote. Log: /tmp/ob-levels-sf/build.txt.
import json
import math
import os

import unreal

DATA = os.environ.get("OB_SF_DATA", "/tmp/ob-levels-sf/data")
LOG_PATH = os.environ.get("OB_BUILD_LOG", "/tmp/ob-levels-sf/build.txt")
MAP = "/Game/Maps/OB_Embarcadero"
ROOT = "/Game/Embarcadero"
MATS = ROOT + "/Materials"
MESHES = ROOT + "/Meshes"

os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
_log = open(LOG_PATH, "w")


def log(*m):
    _log.write(" ".join(str(x) for x in m) + "\n"); _log.flush()


def fail(m):
    log("FAIL: " + m); raise RuntimeError(m)


def setp(o, k, v):
    try:
        o.set_editor_property(k, v)
    except Exception as e:
        log("  set %s.%s: %s" % (o.__class__.__name__, k, e))


eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.TrailBuildLibrary
meta = json.load(open(os.path.join(DATA, "meta.json")))

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


def finish(mat):
    for u in USAGES:
        mel.set_material_usage(mat, u)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    errs = lib.get_material_compile_errors(mat)
    eal.save_asset(mat.get_path_name())
    log("material %s%s" % (mat.get_name(), "" if not errs else "  COMPILE ERRORS: " + " | ".join(errs)))


def flat_material(name, color, rough=0.85, spec=0.3, noise_amt=0.0, emissive=None, metallic=0.0):
    mat = new_material(name)
    base = node(mat, unreal.MaterialExpressionConstant3Vector, 0,
                constant=unreal.LinearColor(color[0], color[1], color[2], 1))
    if noise_amt > 0:
        wp = node(mat, unreal.MaterialExpressionWorldPosition, 1)
        ns = node(mat, unreal.MaterialExpressionNoise, 2, scale=0.02, levels=3,
                  output_min=1 - noise_amt, output_max=1 + noise_amt, quality=1, turbulence=False)
        mel.connect_material_expressions(wp, "", ns, "Position")
        mul = node(mat, unreal.MaterialExpressionMultiply, 3)
        mel.connect_material_expressions(base, "", mul, "A")
        mel.connect_material_expressions(ns, "", mul, "B")
        base = mul
    mel.connect_material_property(base, "", MP.MP_BASE_COLOR)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 10, r=float(rough)), "", MP.MP_ROUGHNESS)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 11, r=float(spec)), "", MP.MP_SPECULAR)
    if metallic > 0:
        mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 12, r=float(metallic)), "", MP.MP_METALLIC)
    if emissive is not None:
        em = node(mat, unreal.MaterialExpressionConstant3Vector, 13,
                  constant=unreal.LinearColor(emissive[0], emissive[1], emissive[2], 1))
        mel.connect_material_property(em, "", MP.MP_EMISSIVE_COLOR)
    finish(mat)
    return mat


def facade_material():
    """A PLAIN plaster facade (no window stripes): the per-building vertex colour with subtle
    world-space noise, matte. Real kit facades cover every street-facing face; this material only
    dresses the procedural body behind them and the hidden back and side faces, so no striped
    pattern shows anywhere."""
    mat = new_material("M_SF_Facade")
    wall = node(mat, unreal.MaterialExpressionVertexColor, 0)
    wp = node(mat, unreal.MaterialExpressionWorldPosition, 1)
    ns = node(mat, unreal.MaterialExpressionNoise, 2, scale=0.04, levels=3,
              output_min=0.82, output_max=1.08, quality=1, turbulence=False)
    mel.connect_material_expressions(wp, "", ns, "Position")
    mul = node(mat, unreal.MaterialExpressionMultiply, 3)
    mel.connect_material_expressions(wall, "", mul, "A")
    mel.connect_material_expressions(ns, "", mul, "B")
    mel.connect_material_property(mul, "", MP.MP_BASE_COLOR)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 4, r=0.9), "", MP.MP_ROUGHNESS)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 5, r=0.2), "", MP.MP_SPECULAR)
    finish(mat)
    return mat


def kit_glass_material():
    """A tinted semi-gloss glass for the kit window slots, so the panes read as blue glass (not
    black holes) at grazing angles. Non-metallic with moderate roughness, so a large pane reads as
    glass, not a mirror; a faint emissive keeps it from going black in shadow."""
    mat = new_material("M_SF_KitGlass")
    base = node(mat, unreal.MaterialExpressionConstant3Vector, 0,
                constant=unreal.LinearColor(0.05, 0.09, 0.14, 1))
    mel.connect_material_property(base, "", MP.MP_BASE_COLOR)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 2, r=0.35), "", MP.MP_ROUGHNESS)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 3, r=0.6), "", MP.MP_SPECULAR)
    em = node(mat, unreal.MaterialExpressionConstant3Vector, 4,
              constant=unreal.LinearColor(0.03, 0.05, 0.07, 1))
    mel.connect_material_property(em, "", MP.MP_EMISSIVE_COLOR)
    finish(mat)
    return mat


def water_material():
    """Deep blue-green bay water: a Fresnel lerp from a dark teal (looking down) to a bluer horizon
    tint (grazing), low roughness, high specular. The tint keeps the horizon reading as water, not
    a white sky mirror."""
    mat = new_material("M_SF_Water")
    deep = node(mat, unreal.MaterialExpressionConstant3Vector, 0, constant=unreal.LinearColor(0.006, 0.030, 0.038, 1))
    graze = node(mat, unreal.MaterialExpressionConstant3Vector, 1, constant=unreal.LinearColor(0.03, 0.10, 0.11, 1))
    fres = node(mat, unreal.MaterialExpressionFresnel, 2, exponent=4.0, base_reflect_fraction=0.04)
    lerp = node(mat, unreal.MaterialExpressionLinearInterpolate, 3)
    mel.connect_material_expressions(deep, "", lerp, "A")
    mel.connect_material_expressions(graze, "", lerp, "B")
    mel.connect_material_expressions(fres, "", lerp, "Alpha")
    mel.connect_material_property(lerp, "", MP.MP_BASE_COLOR)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 4, r=0.10), "", MP.MP_ROUGHNESS)
    mel.connect_material_property(node(mat, unreal.MaterialExpressionConstant, 5, r=1.0), "", MP.MP_SPECULAR)
    finish(mat)
    return mat


def spawn(cls, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0, label=None):
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw))
    if not a:
        fail("spawn %s" % cls)
    if label:
        a.set_actor_label(label)
    return a


def build_look():
    sun = spawn(unreal.DirectionalLight, (0, 0, 4000), pitch=-50.0, yaw=320.0, label="OB_Sun")
    sc = sun.light_component
    setp(sc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(sc, "intensity", 90000.0); setp(sc, "use_temperature", True); setp(sc, "temperature", 5600.0)
    setp(sc, "atmosphere_sun_light", True); setp(sc, "cast_shadows", True)
    setp(sc, "dynamic_shadow_distance_movable_light", 60000.0)
    spawn(unreal.SkyAtmosphere, (0, 0, 0), label="OB_SkyAtmosphere")
    sky = spawn(unreal.SkyLight, (0, 0, 3000), label="OB_SkyLight")
    setp(sky.light_component, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(sky.light_component, "real_time_capture", True); setp(sky.light_component, "intensity", 3.2)
    clouds = spawn(unreal.VolumetricCloud, (0, 0, 0), label="OB_Clouds")
    cc = clouds.get_component_by_class(unreal.VolumetricCloudComponent)
    setp(cc, "layer_bottom_altitude", 3.0); setp(cc, "layer_height", 8.0)
    ppv = spawn(unreal.PostProcessVolume, (0, 0, 0), label="OB_Look")
    setp(ppv, "unbound", True); setp(ppv, "priority", 100.0)
    s = ppv.settings
    for k, v in dict(dynamic_global_illumination_method=unreal.DynamicGlobalIlluminationMethod.LUMEN,
                     reflection_method=unreal.ReflectionMethod.LUMEN,
                     auto_exposure_method=unreal.AutoExposureMethod.AEM_MANUAL,
                     auto_exposure_apply_physical_camera_exposure=False, auto_exposure_bias=-13.6,
                     bloom_intensity=0.4, vignette_intensity=0.3,
                     lumen_max_trace_distance=80000.0, lumen_scene_view_distance=100000.0).items():
        try:
            s.set_editor_property(k, v); s.set_editor_property("override_" + k, True)
        except Exception as e:
            log("  pp %s: %s" % (k, e))
    ppv.set_editor_property("settings", s)


# --- build ---------------------------------------------------------------------------------------
les.load_level("/Game/Maps/OB_Main")
if eal.does_asset_exist(MAP):
    fail("%s exists; build_embarcadero.sh deletes it first" % MAP)
for d in (MESHES, MATS):
    if eal.does_directory_exist(d):
        eal.delete_directory(d)
log("cleaned")

mats = dict(
    Ground=flat_material("M_SF_Ground", (0.13, 0.14, 0.11), rough=0.95, spec=0.1, noise_amt=0.2),
    Road=flat_material("M_SF_Road", (0.095, 0.098, 0.10), rough=0.9, spec=0.2, noise_amt=0.1),
    Kerb=flat_material("M_SF_Kerb", (0.35, 0.34, 0.32), rough=0.85, spec=0.25),
    Pave=flat_material("M_SF_Pave", (0.17, 0.165, 0.155), rough=0.8, spec=0.2, noise_amt=0.14),
    Rail=flat_material("M_SF_Rail", (0.22, 0.24, 0.27), rough=0.4, spec=0.6, metallic=0.9),
    Facade=facade_material(),
    Roof=flat_material("M_SF_Roof", (0.12, 0.12, 0.13), rough=0.8, spec=0.2),
    Water=water_material(),
    White=flat_material("M_SF_White", (0.82, 0.82, 0.80), rough=0.55, spec=0.2, emissive=(0.3, 0.3, 0.29)),
    Yellow=flat_material("M_SF_Yellow", (0.82, 0.68, 0.04), rough=0.55, spec=0.2, emissive=(0.4, 0.33, 0.0)),
    Bike=flat_material("M_SF_Bike", (0.03, 0.22, 0.06), rough=0.7, spec=0.2, emissive=(0.0, 0.12, 0.02)),
    Canopy=flat_material("M_SF_Canopy", (0.06, 0.16, 0.05), rough=0.9, spec=0.1, noise_amt=0.3),
    Bark=flat_material("M_SF_Bark", (0.14, 0.10, 0.06), rough=0.9, spec=0.2),
    Plaster=flat_material("M_SF_Plaster", (0.56, 0.54, 0.50), rough=0.9, spec=0.2, noise_amt=0.1),
)
SLOT = {"Ground": "Ground", "Road": "Road", "Kerb": "Kerb", "Rail": "Rail", "Facade": "Facade",
        "Roof": "Roof", "White": "White", "Yellow": "Yellow", "Bike": "Bike"}
kitglass = kit_glass_material()


def make_mesh(obm, name, nanite=True):
    path = MESHES + "/" + name
    m = lib.create_static_mesh_from_obm(os.path.join(DATA, obm), path + "." + name, nanite, 0, False)
    if not m:
        fail("mesh %s" % obm)
    for i, sm in enumerate(m.get_editor_property("static_materials")):
        slot = str(sm.material_slot_name)
        if slot in mats:
            m.set_material(i, mats[slot])
    eal.save_asset(path)
    return path


def names_in(prefix):
    return sorted([f for f in os.listdir(DATA) if f.startswith(prefix) and f.endswith(".obm")])


# build every mesh ASSET while OB_Main is loaded
assets = []
for f in names_in("ground_"):
    assets.append(("Ground", make_mesh(f, "SM_" + f[:-4])))
for f in names_in("corridor_"):
    assets.append(("Road", make_mesh(f, "SM_" + f[:-4])))
for f, nm in (("kerbs.obm", "SM_SF_Kerbs"), ("rails.obm", "SM_SF_Rails"), ("marks.obm", "SM_SF_Marks")):
    assets.append((f[:-4], make_mesh(f, nm)))
# buildings non-Nanite, so the per-building vertex colour survives to the facade material
assets.append(("buildings", make_mesh("buildings.obm", "SM_SF_Buildings", nanite=False)))
log("built %d meshes" % len(assets))

world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("first save_map failed")
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def sm_actor(path, label, collide):
    a = spawn(unreal.StaticMeshActor, label=label)
    smc = a.static_mesh_component
    smc.set_static_mesh(unreal.load_asset(path))
    smc.set_mobility(unreal.ComponentMobility.STATIC)
    if collide:
        smc.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
        smc.set_collision_profile_name("BlockAll")
    else:
        smc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    return a


for i, (kind, path) in enumerate(assets):
    sm_actor(path, "OB_SF_" + path.split("/")[-1], collide=(kind != "Marks"))
log("placed %d mesh actors" % len(assets))

# water plane over the bay
w = json.load(open(os.path.join(DATA, "water.json")))
if w:
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    # extend the water east over the bay and far along the shore, so it reaches the horizon
    xlo, xhi = w["xlo"] - 20.0, w["xhi"] + 2600.0
    ylo, yhi = -1600.0, 1600.0
    cx, cy = (xlo + xhi) / 2, (ylo + yhi) / 2
    a = spawn(unreal.StaticMeshActor, (100 * cx, -100 * cy, 100 * w["z"] - 20.0), label="OB_SF_Water")
    a.static_mesh_component.set_static_mesh(cube)
    a.static_mesh_component.set_material(0, mats["Water"])
    a.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    a.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    a.set_actor_scale3d(unreal.Vector(xhi - xlo, yhi - ylo, 0.4))
    log("water plane %.0f x %.0f m" % (xhi - xlo, yhi - ylo))
    # a low dark shore across the bay, so the horizon reads as land, not white sky
    shore_mat = flat_material("M_SF_Shore", (0.04, 0.06, 0.05), rough=0.95, spec=0.05, noise_amt=0.3)
    for sx, sy, lenx, leny in ((w["xhi"] + 1900.0, 0.0, 120.0, 3400.0),      # east, across the bay
                               (w["xhi"] + 900.0, 1500.0, 1800.0, 120.0),    # north
                               (w["xhi"] + 900.0, -1500.0, 1800.0, 120.0)):  # south
        s = spawn(unreal.StaticMeshActor, (100 * sx, -100 * sy, 100 * (w["z"] + 18.0)), label="OB_SF_Shore")
        s.static_mesh_component.set_static_mesh(cube)
        s.static_mesh_component.set_material(0, shore_mat)
        s.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
        s.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        s.set_actor_scale3d(unreal.Vector(lenx, leny, 40.0))

# PlayerStart at the world origin
ps = spawn(unreal.PlayerStart, tuple(meta["origin_cm"]), yaw=meta["origin_yaw_deg"], label="OB_EmbarcaderoOrigin")
log("PlayerStart at %s yaw %s" % (ps.get_actor_location(), ps.get_actor_rotation().yaw))


# --- dressing ------------------------------------------------------------------------------------
def ue(x, y, z):
    return 100.0 * x, -100.0 * y, 100.0 * z


PROP = {"lamp": "/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B",
        "bench": "/Game/Prop/Kit_bench_RR/Mesh/SM_street_bench"}
dress = json.load(open(os.path.join(DATA, "dressing.json")))
nd = 0
for kind in ("lamp", "bench"):
    mesh = unreal.load_asset(PROP[kind])
    if not mesh:
        log("  prop %s MISSING %s" % (kind, PROP[kind]))
        continue
    holder = spawn(unreal.load_class(None, "/Script/OverboardGame.TrailScatterActor"), label="OB_SF_" + kind)
    xf = []
    for x, y, z, yaw, sc in dress.get(kind, []):
        ux, uy, uz = ue(x, y, z)
        xf.append(unreal.Transform(unreal.Vector(ux, uy, uz), unreal.Rotator(0, 0, -yaw), unreal.Vector(sc, sc, sc)))
    if xf:
        nd += holder.add_static_instances(mesh, xf, 0.0, True, "HISM_" + kind)
log("placed %d lamp/bench instances" % nd)


def proxy(kind, x, y, z, trunk_h, canopy_r, canopy_z):
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    sph = unreal.load_asset("/Engine/BasicShapes/Sphere")
    ux, uy, uz = ue(x, y, z)
    tr = spawn(unreal.StaticMeshActor, (ux, uy, uz + trunk_h * 50.0), label="OB_SF_%s_trunk" % kind)
    tr.static_mesh_component.set_static_mesh(cube)
    tr.static_mesh_component.set_material(0, mats["Bark"])
    tr.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    tr.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    tr.set_actor_scale3d(unreal.Vector(0.4, 0.4, trunk_h))
    cp = spawn(unreal.StaticMeshActor, (ux, uy, uz + canopy_z * 100.0), label="OB_SF_%s_canopy" % kind)
    cp.static_mesh_component.set_static_mesh(sph)
    cp.static_mesh_component.set_material(0, mats["Canopy"])
    cp.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    cp.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    cp.set_actor_scale3d(unreal.Vector(canopy_r, canopy_r, canopy_r * 0.8))


nt = 0
for x, y, z, yaw, sc in dress.get("tree", []):
    proxy("tree", x, y, z, 3.0, 2.4, 4.2); nt += 1
for x, y, z, yaw, sc in dress.get("palm", []):
    proxy("palm", x, y, z, 7.0, 1.6, 7.5); nt += 1   # a tall thin palm proxy
log("placed %d tree/palm proxies" % nt)

# --- real City Sample facades -------------------------------------------------------------------
# gen_embarcadero.py chose where whole kit window modules tile each street-facing edge near the
# route, plus blank-wall fills, plinths and corner boxes. The kit meshes are instanced (HISM), so
# the licensed art stays referenced, not duplicated. The procedural buildings stay behind as the
# body and roof, so the raised MuJoCo block stays covered.
def box_xf(cx, cy, cz, yaw, sx, sy, sz):
    return unreal.Transform(unreal.Vector(*ue(cx, cy, cz)), unreal.Rotator(0, 0, -yaw),
                            unreal.Vector(sx, sy, sz))


def box_hism(mesh, mat, xfs, label):
    """Place box instances on their own holder, with the material override on the HISM component."""
    if not xfs:
        return 0
    h = spawn(scatter_cls, label=label)
    h.add_static_instances(mesh, xfs, 0.0, True, label)
    for c in h.get_components_by_class(unreal.HierarchicalInstancedStaticMeshComponent):
        c.set_material(0, mat)
        c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    return len(xfs)


def glass_slot(mesh):
    """The material slot whose parent is M_Window (the kit glass), or -1 if the mesh has none."""
    for i, sm in enumerate(mesh.get_editor_property("static_materials")):
        mi = sm.material_interface
        try:
            if mi and mi.get_base_material().get_name() == "M_Window":
                return i
        except Exception:
            pass
    return -1


fac = json.load(open(os.path.join(DATA, "facades.json")))
scatter_cls = unreal.load_class(None, "/Script/OverboardGame.TrailScatterActor")
# group the kit piece instances by mesh; one HISM per mesh, with the glass slot brightened
by_mesh = {}
for mesh_idx, x, y, z, yaw in fac["pieces"]:
    by_mesh.setdefault(mesh_idx, []).append(
        unreal.Transform(unreal.Vector(*ue(x, y, z)), unreal.Rotator(0, 0, -yaw), unreal.Vector(1, 1, 1)))
nf = 0
for mesh_idx, mesh_path in enumerate(fac["meshes"]):
    mesh = unreal.load_asset(mesh_path)
    if not mesh:
        log("  facade mesh MISSING %s" % mesh_path)
        continue
    xf = by_mesh.get(mesh_idx, [])
    if not xf:
        continue
    holder = spawn(scatter_cls, label="OB_SF_Facade_%d" % mesh_idx)
    holder.add_static_instances(mesh, xf, 0.0, True, "HISM")
    gs = glass_slot(mesh)
    if gs >= 0:
        for c in holder.get_components_by_class(unreal.HierarchicalInstancedStaticMeshComponent):
            c.set_material(gs, kitglass)
    nf += len(xf)
log("placed %d facade kit instances (%d meshes)" % (nf, len(fac["meshes"])))

cube = unreal.load_asset("/Engine/BasicShapes/Cube")
# blank-wall fill: a plaster panel (depth, remainder width, module height)
blank_xf = [box_xf(x, y, z + h / 2, yaw, 0.6, w, h) for x, y, z, yaw, w, h in fac["blanks"]]
box_hism(cube, mats["Plaster"], blank_xf, "OB_SF_FacadeBlank")
# corner boxes: a slim plaster pier at each street-facing vertex
corner_xf = [box_xf(x, y, z + h / 2, 0.0, 0.4, 0.4, h) for x, y, z, h in fac["corners"]]
box_hism(cube, mats["Plaster"], corner_xf, "OB_SF_FacadeCorner")
# plinths: a low stone base under each veneered edge
plinth_xf = []
for ax, ay, bx, by, z in fac["plinths"]:
    L = math.hypot(bx - ax, by - ay)
    yaw = math.degrees(math.atan2(by - ay, bx - ax))
    plinth_xf.append(box_xf((ax + bx) / 2, (ay + by) / 2, z - 0.15, yaw, 0.4, L, 0.3))
box_hism(cube, mats["Kerb"], plinth_xf, "OB_SF_FacadePlinth")
log("placed %d blanks, %d corners, %d plinths"
    % (len(blank_xf), len(corner_xf), len(plinth_xf)))

build_look()
# NoGround: no motion-reference markers or placeholder ground on a real level.
world.get_world_settings().set_editor_property("default_game_mode", unreal.load_class(
    None, "/Script/OverboardGame.OverboardGameMode_NoGround"))
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map failed")
log("saved %s\nDONE" % MAP)
_log.close()
