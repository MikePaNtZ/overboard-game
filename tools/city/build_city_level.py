# build_city_level.py -- UE editor python. Builds OB_CityHill from the files tools/city/gen_city.py
# wrote: the street, the cross street, the kerbs and the sidewalks (exact course heights), the
# City Sample buildings and their concrete plinths, the cheap massing layer, the street dressing,
# the golden-hour look, and (with a camera plan) the Level Sequence and the Movie Render Queue
# configs. Run headless (see tools/city/build_city.sh):
#
#   OB_CITY_DATA=/tmp/ob-city/city_hill OB_CAMERAS=/tmp/ob-city/cameras.json \
#   UnrealEditor-Cmd <abs>/OverboardGame.uproject -run=pythonscript -script=<abs>/tools/city/build_city_level.py
#
# It rebuilds only its own assets: /Game/Maps/OB_CityHill and /Game/CityHill/. build_city.sh deletes the
# map and the generated folders on disk first (the editor cannot delete a map it may hold). The
# builder NEVER opens the City Park "Showcase" sublevel; it makes a fresh blank map from OB_Main.
# Log: /tmp/ob-city/build.txt (print() from a commandlet does not reach the log).

import json
import math
import os

import unreal

DATA = os.environ.get("OB_CITY_DATA", "/tmp/ob-city/city_hill")
CAMERAS = os.environ.get("OB_CAMERAS", "")
FRAMES_DIR = os.environ.get("OB_FRAMES_DIR", "/tmp/ob-city/frames")
LOOK = json.loads(os.environ.get("OB_LOOK", "{}"))
LOG_PATH = os.environ.get("OB_BUILD_LOG", "/tmp/ob-city/build.txt")
SKIP = set(filter(None, os.environ.get("OB_CITY_SKIP", "").split(",")))   # e.g. "buildings,scatter"

MAP = "/Game/Maps/OB_CityHill"
ROOT = "/Game/CityHill"
CINE = ROOT + "/Cinematics"
MATS = ROOT + "/Materials"
MESHES = ROOT + "/Meshes"
MS = "/Game/Megascans"

LOOK_DEFAULTS = dict(
    sun_pitch=-30.0,       # 30 degrees above the horizon: high enough to reach the floor of the deep
                           # 12 m street canyon, while still a warm late-afternoon raking light
    sun_yaw=22.0,          # light travels mostly down the street (+X) and a little across (+Y), so the
                           # street floor and one side's facades are lit and the shadows are long
    sun_lux=52000.0,
    sun_temp=4150.0,
    sky_intensity=4.0,     # a strong sky fill so the shadowed parts of the street are not crushed
    # The street sits 4-11 m down in the valley. HEIGHT FOG IS OFF: even a thin layer fills the deep
    # channel with bright golden inscatter down the long sightline and washes the street white. The
    # light haze comes from the sky-atmosphere aerial perspective instead; the volumetric fog is off.
    fog_density=0.0,
    fog_falloff=0.02,
    fog_start=5000.0,
    aerial_scale=1.6,
    vol_fog=False,
    vol_fog_extinction=0.04,
    vol_fog_scatter=0.3,
    vol_fog_albedo=(0.9, 0.86, 0.8),
    ev100=13.0,
    white_temp=5600.0,
    saturation=1.05,
    contrast=1.0,
    gain=(1.05, 1.0, 0.93),
    shadow_lift=0.02,     # raise the crushed canyon-shadow floor so the asphalt and rider read
)
L = dict(LOOK_DEFAULTS, **LOOK)

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
log("data", DATA, "course", meta["course_dir"])


# --- materials (the Graph helper is copied from build_trail_level.py) ----------------------------
def sampler_for(tex):
    cs = tex.get_editor_property("compression_settings")
    srgb = tex.get_editor_property("srgb")
    T = unreal.MaterialSamplerType
    vt = tex.get_editor_property("virtual_texture_streaming")
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


class Graph:
    def __init__(self, mat):
        self.m = mat
        self.n = 0

    def node(self, cls, **props):
        self.n += 1
        e = mel.create_material_expression(self.m, cls, -300 * (1 + self.n // 25), 120 * (self.n % 25))
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def link(self, a, b, b_in, a_out=""):
        if isinstance(a, (int, float)):
            a = self.const(a)
        if not mel.connect_material_expressions(a, a_out, b, b_in):
            log("  link failed %s.%s -> %s.%s" % (a.get_name(), a_out, b.get_name(), b_in))
        return b

    def const(self, v):
        if isinstance(v, (tuple, list)):
            if len(v) == 3:
                return self.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(v[0], v[1], v[2], 1))
            return self.node(unreal.MaterialExpressionConstant2Vector, r=v[0], g=v[1])
        return self.node(unreal.MaterialExpressionConstant, r=float(v))

    def op(self, cls, a, b):
        e = self.node(cls)
        self.link(a, e, "A")
        self.link(b, e, "B")
        return e

    def mul(self, a, b): return self.op(unreal.MaterialExpressionMultiply, a, b)
    def add(self, a, b): return self.op(unreal.MaterialExpressionAdd, a, b)
    def sub(self, a, b): return self.op(unreal.MaterialExpressionSubtract, a, b)
    def div(self, a, b): return self.op(unreal.MaterialExpressionDivide, a, b)

    def lerp(self, a, b, alpha):
        e = self.node(unreal.MaterialExpressionLinearInterpolate)
        self.link(a, e, "A"); self.link(b, e, "B"); self.link(alpha, e, "Alpha")
        return e

    def mask(self, a, r=False, g=False, b=False, al=False):
        e = self.node(unreal.MaterialExpressionComponentMask, r=r, g=g, b=b, a=al)
        self.link(a, e, "")
        return e

    def fn1(self, cls, a, inp="", **props):
        e = self.node(cls, **props)
        self.link(a, e, inp)
        return e

    def sat(self, a): return self.fn1(unreal.MaterialExpressionSaturate, a)

    def custom(self, code, inputs, out=unreal.CustomMaterialOutputType.CMOT_FLOAT1):
        e = self.node(unreal.MaterialExpressionCustom, output_type=out, code=code)
        ins = []
        for name in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", name)
            ins.append(ci)
        e.set_editor_property("inputs", ins)
        for name, src in inputs.items():
            self.link(src, e, name)
        return e

    def tex(self, path, uv, out="RGB"):
        t = unreal.load_asset(path)
        if not t:
            fail("texture missing: " + path)
        e = self.node(unreal.MaterialExpressionTextureSample, texture=t, sampler_type=sampler_for(t))
        if uv is not None:
            self.link(uv, e, "UVs")
        return e

    def world_uv(self, tile_cm, rot=0.0, offset=(0.0, 0.0)):
        wp = self.node(unreal.MaterialExpressionWorldPosition)
        xy = self.mask(wp, r=True, g=True)
        if rot:
            c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
            u = self.node(unreal.MaterialExpressionDotProduct)
            self.link(xy, u, "A"); self.link(self.const((c, -s)), u, "B")
            v = self.node(unreal.MaterialExpressionDotProduct)
            self.link(xy, v, "A"); self.link(self.const((s, c)), v, "B")
            xy = self.node(unreal.MaterialExpressionAppendVector)
            self.link(u, xy, "A"); self.link(v, xy, "B")
        uv = self.div(xy, tile_cm)
        if offset != (0.0, 0.0):
            uv = self.add(uv, self.const(offset))
        return uv

    def noise(self, scale, levels=4, out_min=0.0, out_max=1.0):
        return self.node(unreal.MaterialExpressionNoise, scale=scale, levels=levels, output_min=out_min,
                         output_max=out_max, quality=1, turbulence=False)

    def out(self, e, prop, a_out=""):
        if not mel.connect_material_property(e, a_out, prop):
            log("  output link failed -> %s" % prop)


def new_material(name, path=MATS):
    full = path + "/" + name
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    return tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())


USAGES = [unreal.MaterialUsage.MATUSAGE_NANITE, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
          unreal.MaterialUsage.MATUSAGE_STATIC_LIGHTING]
MP = unreal.MaterialProperty
TEX = dict(
    asphalt=(MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Albedo",
             MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Normal",
             MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Roughness"),
    concrete=(MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Albedo",
              MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Normal",
              MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Roughness"),
)


def layer_set(g, key, tile_cm, tint=(1, 1, 1), rough=None, rot=0.0, anti_tile=True):
    a_p, n_p, r_p = TEX[key]
    uv1 = g.world_uv(tile_cm, rot)
    col = g.tex(a_p, uv1)
    nrm = g.tex(n_p, uv1)
    if anti_tile:
        uv2 = g.world_uv(tile_cm * 2.71, rot + 37.0, (0.31, 0.77))
        col2 = g.tex(a_p, uv2)
        blend = g.noise(0.0009, 3)
        col = g.lerp(col, col2, g.mul(blend, 0.6))
    col = g.mul(col, g.const(tuple(tint)))
    if r_p:
        r = g.mask(g.tex(r_p, uv1), r=True)
        if rough is not None:
            r = g.lerp(r, rough[0], rough[1])
    else:
        r = g.const(rough[0] if rough else 0.8)
    return col, nrm, r


def finish(mat, usages=()):
    for u in usages:
        mel.set_material_usage(mat, u)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    errs = lib.get_material_compile_errors(mat)
    eal.save_asset(mat.get_path_name())
    log("material %s: %d expressions%s" % (mat.get_name(), mel.get_num_material_expressions(mat),
                                            "" if not errs else "  COMPILE ERRORS: " + " | ".join(errs)))


def build_street_material():
    """Megascans asphalt in world space; a double-yellow centre line, white edge lines and crosswalks
    from the mesh UVs (u across in m 0..12, v along in m = -x). The intersection box blanks the lines."""
    mat = new_material("M_CityStreet")
    g = Graph(mat)
    # The street sits in the buildings' shadow for much of its length. Dark asphalt (~0.08 albedo)
    # reads as black there, so the surface is lifted to a lighter worn-pavement grey (~0.22) that
    # still reads in shadow, as a real San Francisco concrete-and-asphalt street does.
    col, nrm, rough = layer_set(g, "asphalt", 220.0, (2.3, 2.25, 2.2), anti_tile=False)
    W = 2.0 * meta["street"]["half_width_m"]
    xw0, xw1 = meta["intersection"]["x_lo"], meta["intersection"]["x_hi"]
    cw = meta["crosswalk_v"]
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    u = g.mask(uv, r=True)
    v = g.mask(uv, g=True)
    # lines: white edges, a double-yellow centre; crosswalk bands at the two flat-block ends.
    code = ("float W=%f, cwa=%f, cwb=%f, vlo=%f, vhi=%f;\n"
            "float inx = (-V > vlo && -V < vhi) ? 1.0 : 0.0;\n"   # blank lane lines inside the intersection
            "float d0=abs(U-0.2), d1=abs(U-(W-0.2));\n"
            "float edge=1-smoothstep(0.05,0.07,min(d0,d1));\n"
            "float dyc=min(abs(U-(W*0.5-0.16)),abs(U-(W*0.5+0.16)));\n"
            "float ctr=1-smoothstep(0.05,0.07,dyc);\n"
            "float cw=(1-smoothstep(0.6,0.8,min(abs(V-cwa),abs(V-cwb))))*step(frac(U/0.8),0.5);\n"
            "float white=saturate(max(edge,cw))*(1-inx*step(0.5,cw*0.0));\n"   # edges/crosswalk white
            "float yellow=saturate(ctr)*(1-inx);\n"
            "return float3(white, yellow, max(white,yellow));" % (W, cw[0], cw[1], xw0, xw1))
    lines = g.custom(code, {"U": u, "V": v}, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    white = g.mask(lines, r=True)
    yellow = g.mask(lines, g=True)
    anyline = g.mask(lines, b=True)
    wear = g.sat(g.mul(g.sub(g.noise(0.02, 4), 0.12), 3.0))
    whitef = g.mul(white, wear)
    yellowf = g.mul(yellow, wear)
    col = g.lerp(col, g.const((0.72, 0.70, 0.66)), whitef)
    col = g.lerp(col, g.const((0.62, 0.52, 0.10)), yellowf)
    rough = g.lerp(rough, 0.55, g.mul(anyline, wear))
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(rough, MP.MP_ROUGHNESS)
    g.out(g.const(0.2), MP.MP_SPECULAR)
    finish(mat, USAGES)
    return mat


def build_sidewalk_material():
    """Megascans concrete with a scored-joint grid from the mesh UV0 (metres)."""
    mat = new_material("M_CitySidewalk")
    g = Graph(mat)
    col, nrm, rough = layer_set(g, "concrete", 180.0, (0.92, 0.91, 0.89))
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    u = g.mask(uv, r=True)
    v = g.mask(uv, g=True)
    joints = g.custom("float ju=1-smoothstep(0.02,0.04,abs(frac(U/1.5)-0.5));\n"
                      "float jv=1-smoothstep(0.02,0.04,abs(frac(V/1.5)-0.5));\n"
                      "return saturate(max(ju,jv));", {"U": u, "V": v})
    col = g.lerp(col, g.mul(col, g.const((0.6, 0.6, 0.62))), g.mul(joints, 0.8))
    rough = g.lerp(rough, 0.9, joints)
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(rough, MP.MP_ROUGHNESS)
    g.out(g.const(0.15), MP.MP_SPECULAR)
    finish(mat, USAGES)
    return mat


def build_concrete_material(name, tint):
    mat = new_material(name)
    g = Graph(mat)
    col, nrm, rough = layer_set(g, "concrete", 240.0, tint)
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(rough, MP.MP_ROUGHNESS)
    g.out(g.const(0.3), MP.MP_SPECULAR)
    finish(mat, USAGES)
    return mat


def build_massing_material():
    """A cheap facade for the background blocks: concrete tinted warm, with a window grid that is
    dark by day and faintly emissive, so the far skyline reads as buildings, not blank boxes."""
    mat = new_material("M_CityMassing")
    g = Graph(mat)
    col, nrm, rough = layer_set(g, "concrete", 300.0, (0.80, 0.78, 0.74))
    # window grid from world Z and horizontal position (box-mapped enough for a background block)
    wp = g.node(unreal.MaterialExpressionWorldPosition)
    z = g.mask(wp, b=True)
    xy = g.mask(wp, r=True, g=True)
    h = g.node(unreal.MaterialExpressionDotProduct)
    g.link(xy, h, "A"); g.link(g.const((0.0075, 0.0075)), h, "B")
    zz = g.mul(z, 0.00333)
    win = g.custom("float wx=step(0.18,frac(H))*step(frac(H),0.82);\n"
                   "float wz=step(0.25,frac(Z))*step(frac(Z),0.80);\n"
                   "return wx*wz;", {"H": h, "Z": zz})
    glass = g.const((0.05, 0.06, 0.08))
    col = g.lerp(col, glass, g.mul(win, 0.85))
    rough = g.lerp(rough, 0.2, win)
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(rough, MP.MP_ROUGHNESS)
    g.out(g.mul(win, g.const((0.9, 0.7, 0.35))), MP.MP_EMISSIVE_COLOR)
    g.out(g.const(0.4), MP.MP_SPECULAR)
    finish(mat, USAGES)
    return mat


def spawn(cls, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0, label=None):
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw))
    if not a:
        fail("spawn %s failed" % cls)
    if label:
        a.set_actor_label(label)
    return a


# --- look: golden hour (copied from build_trail_level.py) ----------------------------------------
LOOK_LABELS = ("OB_Sun", "OB_SkyAtmosphere", "OB_SkyLight", "OB_CityHillFog", "OB_Clouds", "OB_Look")


def build_look():
    sun = spawn(unreal.DirectionalLight, (0, 0, 3000), pitch=L["sun_pitch"], yaw=L["sun_yaw"], label="OB_Sun")
    sc = sun.light_component
    setp(sc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(sc, "intensity", L["sun_lux"])
    setp(sc, "use_temperature", True)
    setp(sc, "temperature", L["sun_temp"])
    setp(sc, "atmosphere_sun_light", True)
    setp(sc, "light_source_angle", 0.6)
    setp(sc, "cast_shadows", True)
    setp(sc, "bloom_scale", 0.2)
    setp(sc, "enable_light_shaft_bloom", True)
    setp(sc, "volumetric_scattering_intensity", L.get("sun_vol_scatter", 0.25))
    setp(sc, "dynamic_shadow_distance_movable_light", 40000.0)

    sky_atm = spawn(unreal.SkyAtmosphere, (0, 0, 0), label="OB_SkyAtmosphere")
    sac = sky_atm.get_component_by_class(unreal.SkyAtmosphereComponent)
    setp(sac, "aerial_pespective_view_distance_scale", L.get("aerial_scale", 1.2))
    setp(sac, "height_fog_contribution", 1.0)
    setp(sac, "multi_scattering_factor", 1.0)
    setp(sac, "mie_scattering_scale", 0.006)
    setp(sac, "mie_anisotropy", 0.8)

    sky = spawn(unreal.SkyLight, (0, 0, 2000), label="OB_SkyLight")
    kc = sky.light_component
    setp(kc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(kc, "real_time_capture", True)
    setp(kc, "intensity", L["sky_intensity"])

    fog = spawn(unreal.ExponentialHeightFog, (0, 0, -300), label="OB_CityHillFog")
    fc = fog.component
    setp(fc, "fog_density", L["fog_density"])
    setp(fc, "fog_height_falloff", L["fog_falloff"])
    setp(fc, "start_distance", L["fog_start"])
    setp(fc, "enable_volumetric_fog", L.get("vol_fog", False))
    setp(fc, "volumetric_fog_extinction_scale", L["vol_fog_extinction"])
    setp(fc, "volumetric_fog_scattering_distribution", L.get("vol_fog_scatter", 0.3))
    setp(fc, "volumetric_fog_albedo", unreal.Color(*[int(255 * v) for v in L["vol_fog_albedo"]], 255))
    setp(fc, "volumetric_fog_distance", 20000.0)
    setp(fc, "fog_inscattering_luminance", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))

    clouds = spawn(unreal.VolumetricCloud, (0, 0, 0), label="OB_Clouds")
    ccomp = clouds.get_component_by_class(unreal.VolumetricCloudComponent)
    setp(ccomp, "layer_bottom_altitude", 3.5)
    setp(ccomp, "layer_height", 6.0)

    ppv = spawn(unreal.PostProcessVolume, (0, 0, 0), label="OB_Look")
    setp(ppv, "unbound", True)
    setp(ppv, "priority", 100.0)
    s = ppv.settings
    PP = dict(
        dynamic_global_illumination_method=unreal.DynamicGlobalIlluminationMethod.LUMEN,
        reflection_method=unreal.ReflectionMethod.LUMEN,
        lumen_scene_lighting_quality=2.0, lumen_scene_detail=2.0, lumen_final_gather_quality=2.0,
        lumen_reflection_quality=2.0, lumen_max_trace_distance=40000.0, lumen_scene_view_distance=50000.0,
        auto_exposure_method=unreal.AutoExposureMethod.AEM_MANUAL,
        auto_exposure_apply_physical_camera_exposure=False,
        auto_exposure_bias=-L["ev100"],
        bloom_intensity=0.45, lens_flare_intensity=0.2, vignette_intensity=0.3, film_grain_intensity=0.08,
        scene_fringe_intensity=0.15, motion_blur_amount=0.4, motion_blur_max=4.0,
        white_temp=L["white_temp"],
        color_saturation=unreal.Vector4(1.0, 1.0, 1.0, L["saturation"]),
        color_contrast=unreal.Vector4(1.0, 1.0, 1.0, L["contrast"]),
        color_gain=unreal.Vector4(L["gain"][0], L["gain"][1], L["gain"][2], 1.0),
        film_toe=0.30, film_shoulder=0.26, ambient_occlusion_intensity=0.2,
        color_offset=unreal.Vector4(L["shadow_lift"], L["shadow_lift"], L["shadow_lift"], 0.0),
    )
    for k, v in PP.items():
        try:
            s.set_editor_property(k, v)
            s.set_editor_property("override_" + k, True)
        except Exception as e:
            log("  pp %s: %s" % (k, e))
    ppv.set_editor_property("settings", s)


# --- level ---------------------------------------------------------------------------------------
if os.environ.get("OB_CITY_ONLY") == "look":
    les.load_level(MAP)
    for act in eas.get_all_level_actors():
        if act.get_actor_label() in LOOK_LABELS:
            eas.destroy_actor(act)
    build_look()
    les.save_current_level()
    log("look rebuilt", L)
    raise SystemExit(0)

les.load_level("/Game/Maps/OB_Main")           # a safe starting level; never the Showcase sublevel
if eal.does_asset_exist(MAP):
    fail("%s exists; tools/city/build_city.sh deletes it before the editor starts" % MAP)
for d in (CINE, MESHES, MATS):
    if eal.does_directory_exist(d):
        eal.delete_directory(d)
log("cleaned")

mats = dict(
    street=build_street_material(),
    sidewalk=build_sidewalk_material(),
    kerb=build_concrete_material("M_CityKerb", (0.86, 0.85, 0.83)),
    plinth=build_concrete_material("M_CityPlinth", (0.78, 0.76, 0.73)),
    massing=build_massing_material(),
)


def make_mesh(obm_path, name, nanite=True, slots=None):
    path = MESHES + "/" + name
    m = lib.create_static_mesh_from_obm(obm_path, path + "." + name, nanite, 0, False)
    if not m:
        fail("mesh %s from %s" % (name, obm_path))
    for i, sm in enumerate(m.get_editor_property("static_materials")):
        slot = str(sm.material_slot_name)
        if slots and slot in slots:
            m.set_material(i, slots[slot])
    eal.save_asset(path)
    return m


street_mesh = make_mesh(os.path.join(DATA, "street.obm"), "SM_CityStreet", slots={"Street": mats["street"]})
cross_mesh = make_mesh(os.path.join(DATA, "cross.obm"), "SM_CityCross", slots={"Street": mats["street"]})
kerb_mesh = make_mesh(os.path.join(DATA, "kerbs.obm"), "SM_CityKerbs", slots={"Kerb": mats["kerb"]})
side_mesh = make_mesh(os.path.join(DATA, "sidewalks.obm"), "SM_CitySidewalks", slots={"Sidewalk": mats["sidewalk"]})
massing_mesh = make_mesh(os.path.join(DATA, "massing.obm"), "SM_CityMassing", slots={"Massing": mats["massing"]})
log("ground meshes done")

world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
if not world:
    fail("new_blank_map failed")
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("first save_map(%s) failed" % MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
log("new level %s" % world.get_path_name())
gm = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")
world.get_world_settings().set_editor_property("default_game_mode", gm)
log("WorldSettings GameMode override: %s" % (gm.get_name() if gm else "FAILED TO LOAD"))

o = meta["origin_cm"]
ps = spawn(unreal.PlayerStart, tuple(o), yaw=meta["origin_yaw_deg"], label="OB_CityHillOrigin")
all_starts = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PlayerStart)]
log("PlayerStart '%s' at %s yaw %s; total PlayerStarts in level: %d"
    % (ps.get_actor_label(), ps.get_actor_location(), ps.get_actor_rotation().yaw, len(all_starts)))


def sm_actor(mesh, label, material_none=False):
    a = spawn(unreal.StaticMeshActor, label=label)
    smc = a.static_mesh_component
    smc.set_static_mesh(mesh)
    # Collision on, so verify_city.py can trace the tyre onto the street (no landscape here).
    smc.set_mobility(unreal.ComponentMobility.STATIC)
    smc.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
    smc.set_collision_profile_name("BlockAll")   # BlockAll blocks the Visibility channel verify uses
    return a


sm_actor(street_mesh, "OB_CityHillStreet")
sm_actor(cross_mesh, "OB_CityHillCross")
sm_actor(kerb_mesh, "OB_CityHillKerbs")
sm_actor(side_mesh, "OB_CityHillSidewalks")
m_actor = sm_actor(massing_mesh, "OB_CityHillMassing")
m_actor.static_mesh_component.set_editor_property("cast_shadow", True)


# --- sidewalk height lookup (from meta table) ----------------------------------------------------
_swx = meta["sidewalk_z"]["x"]
_swz = meta["sidewalk_z"]["z"]


def side_z_cm(x_m):
    import bisect
    if x_m <= _swx[0]:
        return _swz[0] * 100.0
    if x_m >= _swx[-1]:
        return _swz[-1] * 100.0
    i = bisect.bisect(_swx, x_m)
    t = (x_m - _swx[i - 1]) / (_swx[i] - _swx[i - 1])
    return (_swz[i - 1] + t * (_swz[i] - _swz[i - 1])) * 100.0


# --- buildings -----------------------------------------------------------------------------------
# label -> (content path, fallback footprint along the street m, depth m, facade-faces-+Y yaw).
# The measured footprint is used when the editor reports it; the fallback keeps the layout going
# when a packed level actor does not populate its bounds headless (it still renders in -game).
BUILDINGS = {
    "SFA_Ref": ("/Game/Building/Library/Kit_Ref_Bldg/BPP_SFA_Ref_N1", 24.0, 20.0),
    "SFA_Ref_L1": ("/Game/Building/Library/Kit_Ref_Bldg/BPP_SFA_Ref_Level01_N1", 22.0, 18.0),
    "SFB_Ref": ("/Game/Building/Library/Kit_Ref_Bldg/BPP_SFB_Ref_N1", 30.0, 24.0),
    "SFJ_Ref": ("/Game/Building/Library/Kit_Ref_Bldg/BPP_SFJ_Ref", 44.0, 40.0),
    "Hero_SFD_Long": ("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Low_SFD_Long_01", 40.0, 24.0),
    "Hero_SFA_Tri": ("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFA_Triangle_A01", 34.0, 30.0),
    "Hero_SFC_A": ("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_A01", 28.0, 24.0),
    "Hero_SFC_B": ("/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_B01", 28.0, 24.0),
}
# The front row cycles through the FACADE-scale buildings so no two neighbours repeat. The whole-block
# "Ref" buildings (SFA_Ref 78 m, SFB_Ref 90-140 m, SFJ_Ref tower) are too wide for this 12 m street on
# a steep grade (they would need metres-tall plinths), so they are kept out of the front row and used
# as the tall far-end massing instead.
FRONT_ORDER = ["SFA_Ref_L1", "Hero_SFC_A", "Hero_SFA_Tri", "SFA_Ref_L1", "Hero_SFC_B",
               "Hero_SFC_A", "SFA_Ref_L1", "Hero_SFC_B"]
# Tall buildings at the far ends (behind the front row): a tower each end, both sides.
FAR_END = [("SFJ_Ref", 1.0, -82.0), ("SFJ_Ref", -1.0, -82.0), ("SFJ_Ref", 1.0, 80.0), ("SFJ_Ref", -1.0, 80.0)]
# Cap a plinth at this height; a building whose footprint drops more than this over the grade is
# lowered to sit on its mid-corner height so it does not grow a skyscraper-tall base.
MAX_PLINTH_CM = 450.0
PLINTH_CUBE = "/Engine/BasicShapes/Cube"


def building_class(path):
    bp = unreal.load_asset(path)
    if bp is None:
        return None
    if isinstance(bp, unreal.Blueprint):
        return bp.generated_class()
    return unreal.load_object(None, path + "_C")


def measure(actor):
    """Footprint and vertical extent of a packed building. get_actor_bounds is unreliable for a
    packed level actor (the instanced components populate asynchronously and it returns a changing
    value), so union every primitive component's own world bounds, which are stable once built."""
    try:
        actor.rerun_construction_scripts()
    except Exception:
        pass
    lo = [1e12, 1e12, 1e12]
    hi = [-1e12, -1e12, -1e12]
    found = False
    for comp in actor.get_components_by_class(unreal.PrimitiveComponent):
        try:
            bb = comp.get_editor_property("bounds") if hasattr(comp, "get_editor_property") else None
            c, e = bb.origin, bb.box_extent
        except Exception:
            continue
        if max(e.x, e.y, e.z) < 1.0:
            continue
        found = True
        lo = [min(lo[0], c.x - e.x), min(lo[1], c.y - e.y), min(lo[2], c.z - e.z)]
        hi = [max(hi[0], c.x + e.x), max(hi[1], c.y + e.y), max(hi[2], c.z + e.z)]
    if not found:
        o, e = actor.get_actor_bounds(False, True)
        lo = [o.x - e.x, o.y - e.y, o.z - e.z]
        hi = [o.x + e.x, o.y + e.y, o.z + e.z]
    origin = unreal.Vector((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2)
    extent = unreal.Vector((hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2)
    return origin, extent


def place_buildings():
    st = meta["street"]
    inter = meta["intersection"]
    x_lo, x_hi = st["x_lo"] + 2.0, st["x_hi"] - 2.0
    setback = st["setback_m"]
    gap = (inter["x_lo"] - 11.0, inter["x_hi"] + 11.0)        # leave the cross-street corridor clear
    cube = unreal.load_asset(PLINTH_CUBE)
    n = 0
    for s in (1.0, -1.0):
        x = x_lo
        idx = int((s + 1) * 3)           # offset the two sides so they do not mirror
        while x < x_hi:
            label = FRONT_ORDER[idx % len(FRONT_ORDER)]
            idx += 1
            path, foot_fb, depth_fb = BUILDINGS[label][0], BUILDINGS[label][1], BUILDINGS[label][2]
            if gap[0] < x < gap[1]:                           # skip the intersection mouth
                x = gap[1]
                continue
            cls = building_class(path)
            if cls is None:
                log("  building %s MISSING %s" % (label, path))
                x += foot_fb + 1.0
                continue
            n, foot = place_one(label, s, x, cls, n, cube, setback)
            x += foot + 0.5                      # advance by the MEASURED footprint, no overlap
    # tall far-end massing behind the front row
    for label, s, x in FAR_END:
        cls = building_class(BUILDINGS[label][0])
        if cls is not None:
            n, _ = place_one(label, s, x, cls, n, cube, setback + 14.0)
    log("placed %d buildings" % n)


def place_one(label, s, x, cls, n, cube, setback):
    path, foot_fb, depth_fb = BUILDINGS[label]
    yaw = 180.0 if s > 0 else 0.0                        # facade faces the street; pitch/roll stay 0
    a = spawn(cls, (x * 100.0, 0.0, 0.0), yaw=yaw, label="OB_Bldg_%s_%d" % (label, n))
    origin, extent = measure(a)
    foot = 2 * extent.x / 100.0 if extent.x > 300 else foot_fb
    depth = 2 * extent.y / 100.0 if extent.y > 300 else depth_fb
    cx = x + foot / 2.0                                   # centre on the measured footprint
    up = max(side_z_cm(x), side_z_cm(x + foot))          # uphill front corner sidewalk height
    dn = min(side_z_cm(x), side_z_cm(x + foot))          # downhill front corner sidewalk height
    base_z = up                                          # the building stands on its uphill corner
    bottom_off = origin.z - extent.z                     # measured mesh bottom, relative to spawn z
    spawn_z = base_z - bottom_off                        # so the bounds bottom sits on the sidewalk
    front_y_cm = -s * setback * 100.0                    # UE Y = -100 * y_mujoco
    cy = front_y_cm + (depth * 50.0) * (-1.0 if s > 0 else 1.0)
    a.set_actor_location(unreal.Vector(cx * 100.0, cy, spawn_z), False, False)
    # the plinth fills the slope: from the uphill base down to just below the downhill corner (no float)
    low = dn - 20.0
    ph = base_z - low
    if ph > 10.0:
        pl = spawn(unreal.StaticMeshActor, (cx * 100.0, cy, (base_z + low) / 2.0), label="OB_Plinth_%d" % n)
        pl.static_mesh_component.set_static_mesh(cube)
        pl.set_actor_scale3d(unreal.Vector(foot, max(depth, depth_fb), ph / 100.0))
        pl.static_mesh_component.set_material(0, mats["plinth"])
    o2, e2 = measure(a)                                   # verify the placed bounds bottom
    log("  %-14s x %.1f foot %.1f depth %.1f yaw %.0f  sidewalk %.0f base_z %.0f boundsMinZ %.0f (dz %.0f)  plinth %.0f cm"
        % (label, cx, foot, depth, yaw, side_z_cm(cx), base_z, o2.z - e2.z, (o2.z - e2.z) - base_z, ph))
    return n + 1, foot


if "buildings" not in SKIP:
    place_buildings()


# --- dressing ------------------------------------------------------------------------------------
DRESS = {
    "street_lamp": "/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B",
    "tree_grate": "/Game/Prop/Kit_TreeBase_A/Mesh/SM_TreeBase_SquareGrill_A",
    "street_tree": "/Game/Prop/Kit_Tree_Maple_Red/Mesh/Tree_Maple_Red_A",
    "trash_can": "/Game/Prop/Kit_Trashcan_A/Mesh/SM_Trashcan_A_01",
    "stop_sign": "/Game/Prop/Kit_StopSign_A/Mesh/SM_StopSign_A",
    "street_sign": "/Game/Prop/Kit_StreetSign_A/Mesh/SM_SteetSign_A_NoTurn_01",
    "bus_sign": "/Game/Prop/Kit_BusStopSign_A/Mesh/SM_BusStopSign_A",
}

if "scatter" not in SKIP:
    scat = json.load(open(os.path.join(DATA, "scatter.json")))
    holder = spawn(unreal.load_class(None, "/Script/OverboardGame.TrailScatterActor"), label="OB_CityHillDressing")
    for kind, rows in sorted(scat["kinds"].items()):
        mesh = unreal.load_asset(DRESS.get(kind, ""))
        if not mesh:
            log("  dressing %-12s MESH MISSING (%s)" % (kind, DRESS.get(kind)))
            continue
        xf = [unreal.Transform(unreal.Vector(r[0], r[1], r[2]), unreal.Rotator(roll=r[5], pitch=r[4], yaw=r[3]),
                               unreal.Vector(r[6], r[6], r[6])) for r in rows]
        cull = 0.0
        n = holder.add_static_instances(mesh, xf, cull, True, "HISM_" + kind)
        log("  dressing %-12s %4d instances" % (kind, n))


build_look()

if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map failed")
log("saved %s" % MAP)


# --- cameras, sequence, MRQ (copied from build_trail_level.py) -----------------------------------
cams = json.load(open(CAMERAS)) if CAMERAS and os.path.exists(CAMERAS) else None
cam_actors = {}
if cams:
    for shot in cams["shots"]:
        k0 = shot["keys"][0]
        cam = spawn(unreal.CineCameraActor, (k0[1], k0[2], k0[3]), roll=k0[4], pitch=k0[5], yaw=k0[6], label="OB_Cam_" + shot["name"])
        cc = cam.get_cine_camera_component()
        setp(cc, "filmback", unreal.CameraFilmbackSettings(sensor_width=36.0, sensor_height=20.25))
        lens = cc.lens_settings
        lens.set_editor_property("min_f_stop", 1.2)
        setp(cc, "lens_settings", lens)
        setp(cc, "current_focal_length", shot["focal"])
        setp(cc, "current_aperture", shot["fstop"])
        fs = cc.focus_settings
        fs.set_editor_property("focus_method", unreal.CameraFocusMethod.MANUAL)
        fs.set_editor_property("manual_focus_distance", k0[7])
        fs.set_editor_property("smooth_focus_changes", False)
        setp(cc, "focus_settings", fs)
        if "ev100" in shot:
            pp = cc.get_editor_property("post_process_settings")
            pp.set_editor_property("override_auto_exposure_bias", True)
            pp.set_editor_property("auto_exposure_bias", -shot["ev100"])
            setp(cc, "post_process_settings", pp)
            setp(cc, "post_process_blend_weight", 1.0)
        if "motion_blur" in shot:
            pp = cc.get_editor_property("post_process_settings")
            pp.set_editor_property("override_motion_blur_amount", True)
            pp.set_editor_property("motion_blur_amount", shot["motion_blur"])
            setp(cc, "post_process_settings", pp)
            setp(cc, "post_process_blend_weight", 1.0)
        cam_actors[shot["name"]] = cam

if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map (cameras) failed")

if cams:
    seq = tools.create_asset("SEQ_CityHill", CINE, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(cams["fps"], 1))
    seq.set_playback_start(0)
    seq.set_playback_end(cams["frames"])
    cut_track = seq.add_track(unreal.MovieSceneCameraCutTrack)
    INTERP = unreal.MovieSceneKeyInterpolation.AUTO
    for shot in cams["shots"]:
        cam = cam_actors[shot["name"]]
        b = seq.add_possessable(cam)
        tr = b.add_track(unreal.MovieScene3DTransformTrack)
        sec = tr.add_section()
        pre = shot.get("pre", shot["start"])
        sec.set_range(pre, shot["end"] + 1)
        chans = {str(c.channel_name): c for c in sec.get_all_channels()}
        order = ["Location.X", "Location.Y", "Location.Z", "Rotation.X", "Rotation.Y", "Rotation.Z"]
        for key in shot["keys"]:
            f = unreal.FrameNumber(key[0])
            for i, name in enumerate(order):
                chans[name].add_key(f, key[1 + i], interpolation=INTERP)
        for name in ("Scale.X", "Scale.Y", "Scale.Z"):
            chans[name].set_default(1.0)
        cb = seq.add_possessable(cam.get_cine_camera_component())
        ft = cb.add_track(unreal.MovieSceneFloatTrack)
        ft.set_property_name_and_path("ManualFocusDistance", "FocusSettings.ManualFocusDistance")
        fsec = ft.add_section()
        fsec.set_range(pre, shot["end"] + 1)
        fch = fsec.get_all_channels()[0]
        for key in shot["keys"]:
            fch.add_key(unreal.FrameNumber(key[0]), key[7], interpolation=INTERP)
        cut = cut_track.add_section()
        cut.set_range(pre, min(shot["end"], cams["frames"]))
        cut.set_camera_binding_id(seq.get_binding_id(b))
        log("shot %s: frames %d..%d" % (shot["name"], shot["start"], shot["end"]))
    eal.save_asset(seq.get_path_name())

    CVARS = {
        "r.MotionBlurQuality": 4, "r.DepthOfFieldQuality": 4, "r.BloomQuality": 5, "r.Tonemapper.Quality": 5,
        "r.ScreenPercentage": 100, "r.ViewDistanceScale": 3,
        "r.Shadow.Virtual.Enable": 1, "r.Shadow.Virtual.ResolutionLodBiasDirectional": -1.5,
        "r.Lumen.TraceMeshSDFs": 1, "r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.FullResDepth": 1,
        "r.Lumen.Reflections.DownsampleFactor": 1, "r.Lumen.ScreenProbeGather.DownsampleFactor": 8,
        "r.VolumetricFog.GridPixelSize": 4, "r.VolumetricFog.GridSizeZ": 128,
        "r.VolumetricCloud.ViewRaySampleMaxCount": 1024, "r.SkyAtmosphere.SampleCountMax": 64,
        "r.StaticMeshLODDistanceScale": 0.25, "r.Streaming.FullyLoadUsedTextures": 1,
        "r.Nanite.MaxPixelsPerEdge": 0.5, "r.Shadow.Virtual.SMRT.RayCountDirectional": 16,
    }

    def make_config(name, res, spatial, temporal, png=True, warmup=48, frame_range=None, out_dir=None):
        cfg = tools.create_asset(name, CINE, unreal.MoviePipelinePrimaryConfig, unreal.MoviePipelinePrimaryConfigFactory())
        out = cfg.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
        out.output_directory = unreal.DirectoryPath(os.path.join(FRAMES_DIR, out_dir or name))
        if frame_range:
            setp(out, "use_custom_playback_range", True)
            setp(out, "custom_start_frame", frame_range[0])
            setp(out, "custom_end_frame", frame_range[1])
        out.file_name_format = "{sequence_name}.{frame_number}"
        out.output_resolution = unreal.IntPoint(res[0], res[1])
        setp(out, "zero_pad_frame_numbers", 4)
        setp(out, "override_existing_output", True)
        cfg.find_or_add_setting_by_class(unreal.MoviePipelineDeferredPassBase)
        cfg.find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_PNG if png else unreal.MoviePipelineImageSequenceOutput_JPG)
        aa = cfg.find_or_add_setting_by_class(unreal.MoviePipelineAntiAliasingSetting)
        setp(aa, "spatial_sample_count", spatial)
        setp(aa, "temporal_sample_count", temporal)
        setp(aa, "override_anti_aliasing", True)
        setp(aa, "anti_aliasing_method", unreal.AntiAliasingMethod.AAM_TSR)
        setp(aa, "use_camera_cut_for_warm_up", False)
        setp(aa, "engine_warm_up_count", warmup)
        setp(aa, "render_warm_up_count", warmup)
        setp(aa, "render_warm_up_frames", True)
        cam = cfg.find_or_add_setting_by_class(unreal.MoviePipelineCameraSetting)
        setp(cam, "shutter_timing", unreal.MoviePipelineShutterTiming.FRAME_CENTER)
        go = cfg.find_or_add_setting_by_class(unreal.MoviePipelineGameOverrideSetting)
        setp(go, "cinematic_quality_settings", True)
        setp(go, "texture_streaming", unreal.MoviePipelineTextureStreamingMethod.FULLY_LOAD)
        setp(go, "use_lod_zero", True)
        setp(go, "disable_hlo_ds", True)
        setp(go, "flush_grass_streaming", True)
        setp(go, "flush_streaming_managers", True)
        setp(go, "game_mode_override", gm)
        cv = cfg.find_or_add_setting_by_class(unreal.MoviePipelineConsoleVariableSetting)
        for k, v in CVARS.items():
            cv.add_or_update_console_variable(k, float(v))
        eal.save_asset(cfg.get_path_name())
        log("config %s %sx%s spatial %d temporal %d range %s" % (name, res[0], res[1], spatial, temporal, frame_range))

    for shot in cams["shots"]:
        end = min(shot["end"], cams["frames"])
        still = shot.get("still", (shot["start"] + end) // 2)
        make_config("MRQ_Still_" + shot["name"], (1920, 1080), 2, 16, frame_range=(still, still + 1))
        make_config("MRQ_Look_" + shot["name"], (960, 540), 1, 4, png=False, warmup=32, frame_range=(still, still + 1))
        make_config("MRQ_Final_" + shot["name"], (1920, 1080), 1, 8, frame_range=(shot["start"], end))

log("DONE")
_log.close()
