# build_trail_level.py -- UE editor python. Builds OB_Trail from the files tools/trail/gen_course.py
# and tools/trail/gen_foliage.py wrote: the landscape (exact course heights), the asphalt path,
# the bridge, the creek, the dressing, the golden-hour look, and (with a camera plan) the Level
# Sequence and the Movie Render Queue configs. Run headless (see tools/trail/build_trail.sh):
#
#   OB_TRAIL_DATA=/tmp/ob-trail/valley_gentle OB_TRAIL_FOLIAGE=/tmp/ob-trail/foliage \
#   OB_CAMERAS=/tmp/ob-trail/cameras.json \
#   UnrealEditor-Cmd <abs>/OverboardGame.uproject -run=pythonscript -script=<abs>/tools/trail/build_trail_level.py
#
# It rebuilds only its own assets: /Game/Maps/OB_Trail and /Game/Trail/. build_trail.sh deletes the
# map and the generated folders on disk first (the editor cannot delete a map it may hold).
# The level does not reference City Park. Log: /tmp/ob-trail/build.txt (print() from a commandlet
# does not reach the log).

import json
import os

import unreal

DATA = os.environ.get("OB_TRAIL_DATA", "/tmp/ob-trail/valley_gentle")
FOLIAGE = os.environ.get("OB_TRAIL_FOLIAGE", "/tmp/ob-trail/foliage")
CAMERAS = os.environ.get("OB_CAMERAS", "")
FRAMES_DIR = os.environ.get("OB_FRAMES_DIR", "/tmp/ob-trail/frames")
LOOK = json.loads(os.environ.get("OB_LOOK", "{}"))
LOG_PATH = os.environ.get("OB_BUILD_LOG", "/tmp/ob-trail/build.txt")
SKIP = set(filter(None, os.environ.get("OB_TRAIL_SKIP", "").split(",")))  # e.g. "scatter" for fast look-dev

MAP = "/Game/Maps/OB_Trail"
ROOT = "/Game/Trail"
CINE = ROOT + "/Cinematics"
MATS = ROOT + "/Materials"
MESHES = ROOT + "/Meshes"
PVE = "/ProceduralVegetationEditor/SampleAssets"
MS = "/Game/Megascans"

LOOK_DEFAULTS = dict(
    sun_pitch=-12.0,       # 12 degrees above the horizon
    sun_yaw=20.0,          # direction the light travels: down the valley toward +X, so the sun
                           # sits low ahead of a rider who rides toward -X (backlight, long shadows)
    sun_lux=60000.0,
    sun_temp=4300.0,
    sky_intensity=1.4,
    fog_density=0.018,
    fog_falloff=0.12,
    fog_start=800.0,
    vol_fog_extinction=0.9,
    vol_fog_albedo=(0.95, 0.88, 0.78),
    ev100=12.5,            # manual exposure; the frame is graded, not metered
    white_temp=6000.0,
    saturation=1.04,
    contrast=1.06,
    gain=(1.04, 1.0, 0.94),
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
log("data", DATA, "course", meta["course_dir"], "verify", meta["verify"])

unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/ProceduralVegetationEditor"], True)


# --- materials -----------------------------------------------------------------------------------
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
    """Thin helper over MaterialEditingLibrary. Every node gets its own column slot."""

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
            import math
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

    def noise(self, scale, levels=4, out_min=0.0, out_max=1.0, func=None):
        e = self.node(unreal.MaterialExpressionNoise, scale=scale, levels=levels, output_min=out_min, output_max=out_max,
                      quality=1, turbulence=False)
        if func is not None:
            e.set_editor_property("noise_function", func)
        return e  # an unconnected Position input is the absolute world position

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


def finish(mat, usages=()):
    for u in usages:
        mel.set_material_usage(mat, u)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    errs = lib.get_material_compile_errors(mat)
    eal.save_asset(mat.get_path_name())
    log("material %s: %d expressions%s" % (mat.get_name(), mel.get_num_material_expressions(mat),
                                            "" if not errs else "  COMPILE ERRORS: " + " | ".join(errs)))


MP = unreal.MaterialProperty
TEX = dict(
    asphalt=(MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Albedo",
             MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Normal",
             MS + "/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Roughness"),
    gravel=(MS + "/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Albedo",
            MS + "/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Normal",
            MS + "/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Roughness"),
    dirt=("/Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Albedo", "/Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Normal",
          "/Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Roughness"),
    sand=(MS + "/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Albedo", MS + "/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Normal",
          MS + "/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Roughness"),
    rock=(MS + "/3D_Assets/Mossy_Rock_ulldfii/T_Mossy_Rock_ulldfii_2K_D", MS + "/3D_Assets/Mossy_Rock_ulldfii/T_Mossy_Rock_ulldfii_2K_N", None),
    rubble=(MS + "/Surfaces/Ground_Rubble_2x2_00/Ground_Rubble_2x2_00_Albedo", MS + "/Surfaces/Ground_Rubble_2x2_00/Ground_Rubble_2x2_00_Normal", None),
    wood=(MS + "/Surfaces/Wooden_Planks_tixjedrbw/Albedo_4K__tixjedrbw", MS + "/Surfaces/Wooden_Planks_tixjedrbw/Normal_4K__tixjedrbw",
          MS + "/Surfaces/Wooden_Planks_tixjedrbw/RAODM_4K__tixjedrbw"),
    concrete=(MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Albedo", MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Normal",
              MS + "/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Roughness"),
)


def layer_set(g, key, tile_cm, tint=(1, 1, 1), rough=None, rot=0.0, anti_tile=True):
    """Albedo (tinted, anti-tiled by a rotated second sample), normal and roughness for one texture set."""
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
        r = g.tex(r_p, uv1)
        r = g.mask(r, r=True)
        if rough is not None:
            r = g.lerp(r, rough[0], rough[1])
    else:
        r = g.const(rough[0] if rough else 0.8)
    return col, nrm, r


def build_ground_material():
    mat = new_material("M_TrailGround")
    g = Graph(mat)
    names = meta["main"]["layers"]
    sets = {
        "Asphalt": layer_set(g, "asphalt", 200.0, (0.9, 0.9, 0.9)),
        "Gravel": layer_set(g, "gravel", 120.0, (0.95, 0.88, 0.78)),
        "Meadow": layer_set(g, "dirt", 220.0, (0.42, 0.58, 0.22), rough=(0.92, 0.5)),
        "Forest": layer_set(g, "dirt", 260.0, (0.42, 0.40, 0.30), rough=(0.9, 0.4), rot=23.0),
        "Rock": layer_set(g, "rock", 320.0, (1.6, 1.5, 1.35), rough=(0.85, 0.0)),
        "Creek": layer_set(g, "sand", 160.0, (0.55, 0.5, 0.45), rough=(0.35, 0.6)),
    }
    # moss on the flatter forest floor: a green multiply driven by the up-facing normal and noise
    up = g.mask(g.node(unreal.MaterialExpressionVertexNormalWS), b=True)
    moss_amt = g.mul(g.sat(g.mul(g.sub(up, 0.88), 8.0)), g.noise(0.004, 3, 0.2, 1.0))
    fc, fn, fr = sets["Forest"]
    sets["Forest"] = (g.lerp(fc, g.mul(fc, g.const((0.55, 0.85, 0.35))), moss_amt), fn, fr)
    acc_c = acc_n = acc_r = None
    for name in names:
        w = g.node(unreal.MaterialExpressionLandscapeLayerSample, parameter_name=name, preview_weight=1.0 if name == "Forest" else 0.0)
        c, n, r = sets[name]
        cw, nw, rw = g.mul(c, w), g.mul(n, w), g.mul(r, w)
        acc_c = cw if acc_c is None else g.add(acc_c, cw)
        acc_n = nw if acc_n is None else g.add(acc_n, nw)
        acc_r = rw if acc_r is None else g.add(acc_r, rw)
    macro = g.noise(0.00025, 4, 0.82, 1.12)
    acc_c = g.mul(acc_c, macro)
    g.out(acc_c, MP.MP_BASE_COLOR)
    g.out(acc_n, MP.MP_NORMAL)
    g.out(acc_r, MP.MP_ROUGHNESS)
    g.out(g.const(0.4), MP.MP_SPECULAR)
    finish(mat)
    return mat


def build_far_material():
    mat = new_material("M_TrailFar")
    g = Graph(mat)
    # distant forest canopy: dark greens broken up at two scales, rock on steep ground, all
    # under aerial perspective. Only seen beyond the scattered trees.
    n1 = g.noise(0.0004, 4, 0.0, 1.0)
    n2 = g.noise(0.003, 3, 0.0, 1.0)
    c = g.lerp(g.const((0.018, 0.03, 0.01)), g.const((0.05, 0.065, 0.02)), n1)
    c = g.lerp(c, g.const((0.075, 0.07, 0.03)), g.mul(n2, 0.35))
    up = g.mask(g.node(unreal.MaterialExpressionVertexNormalWS), b=True)
    steep = g.sat(g.mul(g.sub(0.8, up), 4.0))
    c = g.lerp(c, g.const((0.09, 0.085, 0.075)), steep)
    g.out(c, MP.MP_BASE_COLOR)
    g.out(g.const(0.95), MP.MP_ROUGHNESS)
    g.out(g.const(0.2), MP.MP_SPECULAR)
    finish(mat)
    return mat


def build_asphalt_material():
    """The path mesh: Megascans asphalt in world space, worn white edge lines and a dashed centre
    line from the mesh UVs (u across in m, v along in m)."""
    mat = new_material("M_TrailAsphalt")
    g = Graph(mat)
    col, nrm, rough = layer_set(g, "asphalt", 200.0, (1.18, 1.16, 1.12), anti_tile=False)
    width = 2.0 * meta["path"]["half_width_m"]
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    u = g.mask(uv, r=True)
    v = g.mask(uv, g=True)
    cu = g.custom("float W = %f;\n"
                  "float d0 = abs(U - 0.17), d1 = abs(U - (W - 0.17));\n"
                  "float edge = 1 - smoothstep(0.045, 0.06, min(d0, d1));\n"
                  "float c = (1 - smoothstep(0.045, 0.06, abs(U - W * 0.5))) * step(frac(V / 6.0), 0.5);\n"
                  "return saturate(edge + c);" % width, {"U": u, "V": v})
    wear = g.noise(0.02, 4, 0.0, 1.0)
    wear = g.sat(g.mul(g.sub(wear, 0.18), 3.0))
    line = g.mul(cu, wear)
    paint = g.const((0.62, 0.61, 0.57))
    col = g.lerp(col, paint, line)
    rough = g.lerp(rough, 0.55, line)
    # a little dust toward the edges
    edge_d = g.custom("return 1 - smoothstep(0.0, 0.5, min(U, %f - U));" % width, {"U": u})
    col = g.lerp(col, g.mul(col, g.const((1.25, 1.15, 1.0))), g.mul(edge_d, 0.6))
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(rough, MP.MP_ROUGHNESS)
    g.out(g.const(0.5), MP.MP_SPECULAR)
    finish(mat, USAGES)
    return mat


def build_box_material(name, key, tile_cm, tint, rough_scale=1.0, plank_var=False):
    """Bridge timber and concrete, mapped by the mesh's UV0 (metres)."""
    mat = new_material(name)
    g = Graph(mat)
    a_p, n_p, r_p = TEX[key]
    uv = g.div(g.node(unreal.MaterialExpressionTextureCoordinate), tile_cm / 100.0)
    col = g.tex(a_p, uv)
    nrm = g.tex(n_p, uv)
    r = g.mask(g.tex(r_p, uv), r=True)
    col = g.mul(col, g.const(tint))
    if plank_var:
        # per-plank tone from UV1.x, then a weathered grey toward the ends
        uv1 = g.mask(g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1), r=True)
        col = g.mul(col, g.add(0.82, g.mul(uv1, 0.35)))
        lum = g.node(unreal.MaterialExpressionDesaturation)
        g.link(col, lum, "")
        col = g.lerp(col, g.mul(lum, g.const((1.05, 1.0, 0.92))), 0.45)
    g.out(col, MP.MP_BASE_COLOR)
    g.out(nrm, MP.MP_NORMAL)
    g.out(g.mul(r, rough_scale), MP.MP_ROUGHNESS)
    finish(mat, USAGES)
    return mat


def build_water_material(name, flow):
    mat = new_material(name)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    g = Graph(mat)
    t = g.node(unreal.MaterialExpressionTime)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    tile = 1.6 if flow else 3.0
    base = g.div(uv, tile)
    speed = (0.0, -0.35) if flow else (0.02, 0.015)
    p1 = g.add(base, g.mul(t, g.const(speed)))
    p2 = g.add(g.mul(base, 0.63), g.mul(t, g.const((speed[0] * 0.7 + 0.01, speed[1] * 0.8 - 0.01))))
    n_tex = "/Engine/Functions/Engine_MaterialFunctions02/ExampleContent/Textures/water_n"
    n1 = g.tex(n_tex, p1)
    n2 = g.tex(n_tex, p2)
    n = g.add(n1, n2)
    strength = 0.55 if flow else 0.25
    n = g.mul(n, g.const((strength, strength, 1.0)))
    nn = g.fn1(unreal.MaterialExpressionNormalize, n)
    g.out(nn, MP.MP_NORMAL)
    g.out(g.const(0.04), MP.MP_ROUGHNESS)
    g.out(g.const(0.0), MP.MP_METALLIC)
    g.out(g.const(0.5), MP.MP_SPECULAR)
    g.out(g.const((0.0, 0.0, 0.0)), MP.MP_BASE_COLOR)
    o = g.node(unreal.MaterialExpressionSingleLayerWaterMaterialOutput)
    log("  SLW inputs: %s" % list(mel.get_material_expression_input_names(o)))
    g.link(g.const((0.02, 0.035, 0.03) if flow else (0.015, 0.03, 0.025)), o, "ScatteringCoefficients")
    g.link(g.const((0.35, 0.09, 0.07) if flow else (0.45, 0.12, 0.09)), o, "AbsorptionCoefficients")
    g.link(0.1, o, "PhaseG")
    finish(mat)
    return mat


def build_foliage_material():
    mat = new_material("M_TrailFoliage")
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    mat.set_editor_property("two_sided", True)
    g = Graph(mat)
    vc = g.node(unreal.MaterialExpressionVertexColor)
    rgb = g.mask(vc, r=True, g=True, b=True)
    lin = g.node(unreal.MaterialExpressionPower)
    g.link(rgb, lin, "Base")
    g.link(2.2, lin, "Exp")
    pr = g.node(unreal.MaterialExpressionPerInstanceRandom)
    tint = g.lerp(g.const((0.85, 0.95, 0.8)), g.const((1.12, 1.04, 0.86)), pr)
    patch = g.noise(0.004, 3, 0.0, 1.0)
    tint = g.mul(tint, g.lerp(g.const((0.9, 1.0, 0.85)), g.const((1.1, 1.0, 0.8)), patch))
    col = g.mul(lin, tint)
    a = g.node(unreal.MaterialExpressionMultiply)
    g.link(vc, a, "A", a_out="A")
    g.link(1.0, a, "B")
    g.out(col, MP.MP_BASE_COLOR)
    g.out(g.lerp(0.35, 1.0, a), MP.MP_AMBIENT_OCCLUSION)
    g.out(g.const(0.62), MP.MP_ROUGHNESS)
    g.out(g.const(0.35), MP.MP_SPECULAR)
    g.out(g.mul(col, g.const((0.75, 0.85, 0.35))), MP.MP_SUBSURFACE_COLOR)
    finish(mat, USAGES)
    return mat


def spawn(cls, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0, label=None):
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw))
    if not a:
        fail("spawn %s failed" % cls)
    if label:
        a.set_actor_label(label)
    return a


# --- look: golden hour --------------------------------------------------------------------------
LOOK_LABELS = ("OB_Sun", "OB_SkyAtmosphere", "OB_SkyLight", "OB_ValleyFog", "OB_Clouds", "OB_Look")


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
    setp(sc, "volumetric_scattering_intensity", 1.6)
    setp(sc, "dynamic_shadow_distance_movable_light", 30000.0)

    sky_atm = spawn(unreal.SkyAtmosphere, (0, 0, 0), label="OB_SkyAtmosphere")
    sac = sky_atm.get_component_by_class(unreal.SkyAtmosphereComponent)
    setp(sac, "aerial_pespective_view_distance_scale", 2.5)
    setp(sac, "height_fog_contribution", 1.0)
    setp(sac, "multi_scattering_factor", 1.0)
    setp(sac, "mie_scattering_scale", 0.006)
    setp(sac, "mie_anisotropy", 0.8)

    sky = spawn(unreal.SkyLight, (0, 0, 500), label="OB_SkyLight")
    kc = sky.light_component
    setp(kc, "mobility", unreal.ComponentMobility.MOVABLE)
    setp(kc, "real_time_capture", True)
    setp(kc, "intensity", L["sky_intensity"])

    fog = spawn(unreal.ExponentialHeightFog, (0, 0, -300), label="OB_ValleyFog")
    fc = fog.component
    setp(fc, "fog_density", L["fog_density"])
    setp(fc, "fog_height_falloff", L["fog_falloff"])
    setp(fc, "start_distance", L["fog_start"])
    setp(fc, "enable_volumetric_fog", True)
    setp(fc, "volumetric_fog_extinction_scale", L["vol_fog_extinction"])
    setp(fc, "volumetric_fog_scattering_distribution", 0.8)
    setp(fc, "volumetric_fog_albedo", unreal.Color(*[int(255 * v) for v in L["vol_fog_albedo"]], 255))
    setp(fc, "volumetric_fog_distance", 12000.0)
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
        lumen_scene_lighting_quality=2.0,
        lumen_scene_detail=2.0,
        lumen_final_gather_quality=2.0,
        lumen_reflection_quality=2.0,
        lumen_max_trace_distance=30000.0,
        lumen_scene_view_distance=40000.0,
        # Manual exposure, no physical camera: the result is 2^bias / luminance(EV100 0), so the
        # bias is minus the scene's EV100. Deterministic from frame one, which MRQ stills need.
        auto_exposure_method=unreal.AutoExposureMethod.AEM_MANUAL,
        auto_exposure_apply_physical_camera_exposure=False,
        auto_exposure_bias=-L["ev100"],
        bloom_intensity=0.45,
        lens_flare_intensity=0.2,
        vignette_intensity=0.3,
        film_grain_intensity=0.08,
        scene_fringe_intensity=0.15,
        motion_blur_amount=0.4,
        motion_blur_max=4.0,
        white_temp=L["white_temp"],
        color_saturation=unreal.Vector4(1.0, 1.0, 1.0, L["saturation"]),
        color_contrast=unreal.Vector4(1.0, 1.0, 1.0, L["contrast"]),
        color_gain=unreal.Vector4(L["gain"][0], L["gain"][1], L["gain"][2], 1.0),
        film_toe=0.56,
        film_shoulder=0.26,
        ambient_occlusion_intensity=0.55,
    )
    for k, v in PP.items():
        try:
            s.set_editor_property(k, v)
            s.set_editor_property("override_" + k, True)
        except Exception as e:
            log("  pp %s: %s" % (k, e))
    ppv.set_editor_property("settings", s)


# --- level --------------------------------------------------------------------------------------
if os.environ.get("OB_TRAIL_ONLY") == "look":
    # Re-light the saved level only: replace the look actors, save, stop. Fast look-dev.
    les.load_level(MAP)
    for act in eas.get_all_level_actors():
        if act.get_actor_label() in LOOK_LABELS:
            eas.destroy_actor(act)
    build_look()
    les.save_current_level()
    log("look rebuilt", L)
    raise SystemExit(0)

les.load_level("/Game/Maps/OB_Main")
if eal.does_asset_exist(MAP):
    fail("%s exists; tools/trail/build_trail.sh deletes it before the editor starts" % MAP)
for d in (CINE, ROOT + "/Landscape", MESHES):
    if eal.does_directory_exist(d):
        eal.delete_directory(d)
log("cleaned")

mats = dict(
    ground=build_ground_material(),
    far=build_far_material(),
    asphalt=build_asphalt_material(),
    wood=build_box_material("M_TrailWood", "wood", 200.0, (0.95, 0.85, 0.72), 1.0, plank_var=True),
    wood_dark=build_box_material("M_TrailWoodDark", "wood", 200.0, (0.55, 0.47, 0.4), 1.0, plank_var=True),
    concrete=build_box_material("M_TrailConcrete", "concrete", 250.0, (0.85, 0.83, 0.8)),
    creek=build_water_material("M_TrailCreek", True),
    pond=build_water_material("M_TrailPond", False),
    foliage=build_foliage_material(),
)


def make_mesh(obm_path, name, nanite=True, shape=0, slots=None):
    path = MESHES + "/" + name
    m = lib.create_static_mesh_from_obm(obm_path, path + "." + name, nanite, shape, False)
    if not m:
        fail("mesh %s from %s" % (name, obm_path))
    for i, sm in enumerate(m.get_editor_property("static_materials")):
        slot = str(sm.material_slot_name)
        if slots and slot in slots:
            m.set_material(i, slots[slot])
    eal.save_asset(path)
    return m


path_mesh = make_mesh(os.path.join(DATA, "path.obm"), "SM_TrailPath", slots={"Asphalt": mats["asphalt"]})
bridge_mesh = water_mesh = None
if meta.get("bridge"):
    bridge_mesh = make_mesh(os.path.join(DATA, "bridge.obm"), "SM_TrailBridge",
                            slots={"Wood": mats["wood"], "WoodDark": mats["wood_dark"], "Concrete": mats["concrete"]})
    water_mesh = make_mesh(os.path.join(DATA, "water.obm"), "SM_TrailWater", nanite=False,
                           slots={"Creek": mats["creek"], "Pond": mats["pond"]})
foliage_meshes = {}
for fn in sorted(os.listdir(FOLIAGE)):
    if fn.endswith(".obm"):
        k = fn[:-4]
        foliage_meshes[k] = make_mesh(os.path.join(FOLIAGE, fn), "SM_" + k, nanite=True, shape=1,
                                      slots={"Foliage": mats["foliage"]})
log("meshes done", sorted(foliage_meshes))

world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
if not world:
    fail("new_blank_map failed")
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("first save_map(%s) failed" % MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
log("new level %s" % world.get_path_name())
gm = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")
world.get_world_settings().set_editor_property("default_game_mode", gm)


# MuJoCo origin: the landscape centre, yaw 0 (gen_course.py builds every coordinate that way)
o = meta["origin_cm"]
ps = spawn(unreal.PlayerStart, tuple(o), yaw=meta["origin_yaw_deg"], label="OB_TrailOrigin")
log("PlayerStart at %s yaw %s" % (ps.get_actor_location(), ps.get_actor_rotation().yaw))


def landscape(spec, material, label, layers=True):
    names = spec.get("layers", []) if layers else []
    files = [os.path.join(DATA, f) for f in spec.get("layer_files", [])] if layers else []
    ls = lib.import_landscape_from_raw(os.path.join(DATA, spec["heights"]), spec["size"], spec["sections"], spec["quads"],
                                       unreal.Vector(*spec["location"]), unreal.Vector(*spec["scale"]), material,
                                       names, files, ROOT + "/Landscape", label)
    if not ls:
        fail("landscape %s" % label)
    log("landscape %s ok" % label)
    return ls


main_ls = landscape(meta["main"], mats["ground"], "OB_TrailGround")
far_ls = landscape(meta["far"], mats["far"], "OB_TrailFar", layers=False)
for ls in (main_ls, far_ls):
    setp(ls, "cast_shadow", True)
eal.save_directory(ROOT + "/Landscape", only_if_is_dirty=False, recursive=True)
log("layer infos saved: %s" % eal.list_assets(ROOT + "/Landscape"))

sm_actor = lambda mesh, label: (lambda a: (a.static_mesh_component.set_static_mesh(mesh), a)[1])(
    spawn(unreal.StaticMeshActor, label=label))
sm_actor(path_mesh, "OB_TrailPath")
if bridge_mesh:
    sm_actor(bridge_mesh, "OB_TrailBridge")
    w = sm_actor(water_mesh, "OB_TrailWater")
    w.static_mesh_component.set_editor_property("cast_shadow", False)

# --- dressing -----------------------------------------------------------------------------------
SKINNED = {
    "aspen_01": PVE + "/Tree_European_QuakingAspen_01/SK_European_QuakingAspen_01",
    "aspen_02": PVE + "/Tree_European_QuakingAspen_01/SK_European_QuakingAspen_02",
    "aspen_03": PVE + "/Tree_European_QuakingAspen_01/SK_European_QuakingAspen_03",
    "aspen_04": PVE + "/Tree_European_QuakingAspen_01/SK_European_QuakingAspen_04",
    "hazel_01": PVE + "/Tree_Common_Hazel_01/SK_CommonHazel_01",
    "hazel_02": PVE + "/Tree_Common_Hazel_01/SK_CommonHazel_02",
    "hazel_03": PVE + "/Tree_Common_Hazel_01/SK_CommonHazel_03",
    "hazel_04": PVE + "/Tree_Common_Hazel_01/SK_CommonHazel_04",
}
STATIC = {
    "rock": MS + "/3D_Assets/Mossy_Rock_ulldfii/S_Mossy_Rock_ulldfii_lod3_Var1",
    "snag_birch_a": "/Game/Prop/Kit_Tree_Birch/Mesh/SM_Tree_Birch_a",
    "snag_birch_c": "/Game/Prop/Kit_Tree_Birch/Mesh/SM_Tree_Birch_c",
    "snag_alder_b": "/Game/Prop/Kit_Tree_Alder/Mesh/Tree_Alder_B",
}
CULL = {"grass": 9000.0, "flower": 6000.0, "fern": 9000.0}

for snag in ("/Game/Prop/Kit_Tree_Birch/Material/M_tree_birch", "/Game/Prop/Kit_Tree_Alder/Material/Bark_Mat",
             MS + "/3D_Assets/Mossy_Rock_ulldfii/MI_Mossy_Rock_ulldfii_2K"):
    m = unreal.load_asset(snag)
    base = m.get_base_material() if isinstance(m, unreal.MaterialInstance) else m
    if base and not mel.has_material_usage(base, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES):
        mel.set_material_usage(base, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES)
        eal.save_asset(base.get_path_name())
        log("  usage ISM set on %s" % base.get_path_name())

if "scatter" not in SKIP:
    scat = json.load(open(os.path.join(DATA, "scatter.json")))
    holder = spawn(unreal.load_class(None, "/Script/OverboardGame.TrailScatterActor"), label="OB_TrailDressing")
    for kind, rows in sorted(scat["kinds"].items()):
        xf = [unreal.Transform(unreal.Vector(r[0], r[1], r[2]), unreal.Rotator(roll=r[5], pitch=r[4], yaw=r[3]),
                               unreal.Vector(r[6], r[6], r[6])) for r in rows]
        if kind in SKINNED:
            mesh = unreal.load_asset(SKINNED[kind])
            n = holder.add_skinned_instances(mesh, xf, True, "ISK_" + kind) if mesh else 0
        else:
            mesh = foliage_meshes.get(kind) or (unreal.load_asset(STATIC[kind]) if kind in STATIC else None)
            cull = next((v for k, v in CULL.items() if kind.startswith(k)), 0.0)
            small = kind.startswith(("grass", "flower"))
            n = holder.add_static_instances(mesh, xf, cull, True, "HISM_" + kind) if mesh else 0
        log("  %-18s %6d instances%s" % (kind, n, "" if mesh else "  (MESH MISSING)"))
    log("scatter min clearance from the path edge: %.2f m" % scat["min_clearance_from_path_edge_m"])

build_look()

# --- cameras, sequence, MRQ (only with a camera plan) ----------------------------------------------
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
        if "ev100" in shot:  # a per-shot exposure on top of the level's look (a shaded spot)
            pp = cc.get_editor_property("post_process_settings")
            pp.set_editor_property("override_auto_exposure_bias", True)
            pp.set_editor_property("auto_exposure_bias", -shot["ev100"])
            setp(cc, "post_process_settings", pp)
            setp(cc, "post_process_blend_weight", 1.0)
        if "motion_blur" in shot:  # a per-shot motion blur amount (a fast aerial move smears the foliage)
            pp = cc.get_editor_property("post_process_settings")
            pp.set_editor_property("override_motion_blur_amount", True)
            pp.set_editor_property("motion_blur_amount", shot["motion_blur"])
            setp(cc, "post_process_settings", pp)
            setp(cc, "post_process_blend_weight", 1.0)
        cam_actors[shot["name"]] = cam

if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("save_map failed")
log("saved %s" % MAP)

if cams:
    seq = tools.create_asset("SEQ_Trail", CINE, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
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
        pre = shot.get("pre", shot["start"])  # the pre-roll frames run on this camera; see plan_cameras.py
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
