# build_carve_level.py -- UE editor python. Builds OB_Carve, its Level Sequence and the Movie
# Render Queue configs for the carve render. Run headless:
#
#   OB_CAMERAS=/tmp/ob-render/cameras.json \
#   "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor-Cmd" <abs>/OverboardGame.uproject \
#     -run=pythonscript -script=<abs>/tools/render/build_carve_level.py -stdout -unattended -nosplash
#
# Order matters (docs/citypark-level.md): build and SAVE the persistent level first, and only then
# add City Park's Showcase as a streaming sublevel and save by explicit path. Never save Showcase.
#
# The script deletes and rebuilds only its own assets: /Game/Maps/OB_Carve and /Game/Cinematics/.

import json
import os
import sys

import unreal

LOG_PATH = os.environ.get("OB_BUILD_LOG", "/tmp/ob-render/build.txt")
CAMERAS = os.environ.get("OB_CAMERAS", "/tmp/ob-render/cameras.json")
FRAMES_DIR = os.environ.get("OB_FRAMES_DIR", "/tmp/ob-render/frames")
LOOK = json.loads(os.environ.get("OB_LOOK", "{}"))  # optional overrides, see LOOK_DEFAULTS

MAP = "/Game/Maps/OB_Carve"
CINE = "/Game/Cinematics"
SHOWCASE = "/Game/CityPark/Maps/Showcase"

LOOK_DEFAULTS = dict(
    sun_pitch=-14.0,      # elevation 14 degrees: late afternoon
    sun_yaw=22.0,         # sun ahead-left of the riding direction (heading ~142)        # direction the light travels
    sun_lux=10.0,
    sun_temp=4700.0,
    sky_intensity=1.4,
    fog_density=0.012,
    fog_falloff=0.25,
    vol_fog=True,
    vol_fog_extinction=0.6,
    exposure_bias=0.4,
    ev_min=3.5,
    ev_max=5.8,
    clouds=True,
)
L = dict(LOOK_DEFAULTS, **LOOK)

_log = open(LOG_PATH, "w")


def log(msg):
    _log.write("%s\n" % (msg,))
    _log.flush()


def fail(msg):
    log("FAIL: " + msg)
    raise RuntimeError(msg)


def setp(obj, name, value):
    try:
        obj.set_editor_property(name, value)
    except Exception as e:  # keep going; record what this engine version calls it
        log("  could not set %s.%s: %s" % (obj.__class__.__name__, name, e))


eal = unreal.EditorAssetLibrary
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()

cams = json.load(open(CAMERAS))
log("cameras: %d frames, shots %s" % (cams["frames"], [s["name"] for s in cams["shots"]]))

# --- fresh start, own assets only -------------------------------------------------------------
les.load_level("/Game/Maps/OB_Main")  # release OB_Carve if it is the loaded map
for path in (MAP, CINE):
    if eal.does_asset_exist(path) or eal.does_directory_exist(path):
        if path == CINE:
            eal.delete_directory(path)
        else:
            eal.delete_asset(path)
        log("deleted %s" % path)

if not les.new_level(MAP):
    fail("new_level(%s) returned False" % MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
log("new level: %s" % world.get_path_name())

# --- GameMode override: reads the PlayerStart, no placeholder ground ---------------------------
gm = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")
if not gm:
    fail("OverboardGameMode_NoGround class not found")
world.get_world_settings().set_editor_property("default_game_mode", gm)


def spawn(cls, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0, label=None):
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw))
    if not a:
        fail("spawn %s failed" % cls)
    if label:
        a.set_actor_label(label)
    return a


# --- MuJoCo origin for this run ---------------------------------------------------------------
o = cams["origin_cm"]
ps = spawn(unreal.PlayerStart, (o[0], o[1], o[2]), yaw=cams["origin_yaw_deg"], label="OB_CarveOrigin")
log("PlayerStart at %s yaw %s" % (ps.get_actor_location(), ps.get_actor_rotation().yaw))

# --- lighting rig: replaces City Park's (see ARenderLookOverride) ------------------------------
spawn(unreal.load_class(None, "/Script/OverboardGame.RenderLookOverride"), (o[0], o[1], o[2] + 500), label="OB_RenderLookOverride")

sun = spawn(unreal.DirectionalLight, (o[0], o[1], o[2] + 1000), pitch=L["sun_pitch"], yaw=L["sun_yaw"], label="OB_Sun")
sc = sun.light_component
setp(sc, "mobility", unreal.ComponentMobility.MOVABLE)
setp(sc, "intensity", L["sun_lux"])
setp(sc, "use_temperature", True)
setp(sc, "temperature", L["sun_temp"])
setp(sc, "atmosphere_sun_light", True)
setp(sc, "light_source_angle", 0.8)
setp(sc, "cast_shadows", True)
setp(sc, "bloom_scale", 0.3)
setp(sc, "enable_light_shaft_bloom", True)
setp(sc, "volumetric_scattering_intensity", 1.5)

spawn(unreal.SkyAtmosphere, (o[0], o[1], o[2]), label="OB_SkyAtmosphere")

sky = spawn(unreal.SkyLight, (o[0], o[1], o[2] + 300), label="OB_SkyLight")
kc = sky.light_component
setp(kc, "mobility", unreal.ComponentMobility.MOVABLE)
setp(kc, "real_time_capture", True)
setp(kc, "intensity", L["sky_intensity"])

fog = spawn(unreal.ExponentialHeightFog, (o[0], o[1], o[2] - 200), label="OB_HeightFog")
fc = fog.component
setp(fc, "fog_density", L["fog_density"])
setp(fc, "fog_height_falloff", L["fog_falloff"])
setp(fc, "enable_volumetric_fog", L["vol_fog"])
setp(fc, "volumetric_fog_extinction_scale", L["vol_fog_extinction"])
setp(fc, "volumetric_fog_scattering_distribution", 0.75)
setp(fc, "start_distance", 1500.0)

if L["clouds"]:
    spawn(unreal.VolumetricCloud, (o[0], o[1], o[2]), label="OB_Clouds")

ppv = spawn(unreal.PostProcessVolume, (o[0], o[1], o[2]), label="OB_Look")
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
    lumen_max_trace_distance=20000.0,
    auto_exposure_method=unreal.AutoExposureMethod.AEM_HISTOGRAM,
    auto_exposure_bias=L["exposure_bias"],
    auto_exposure_min_brightness=L["ev_min"],
    auto_exposure_max_brightness=L["ev_max"],
    auto_exposure_speed_up=0.6,
    auto_exposure_speed_down=0.6,
    bloom_intensity=0.5,
    lens_flare_intensity=0.25,
    vignette_intensity=0.35,
    film_grain_intensity=0.12,
    scene_fringe_intensity=0.25,
    motion_blur_amount=0.5,
    motion_blur_max=5.0,
    white_temp=6100.0,
    color_saturation=unreal.Vector4(1.0, 1.0, 1.0, 1.06),
    color_contrast=unreal.Vector4(1.0, 1.0, 1.0, 1.04),
    film_toe=0.58,
    film_shoulder=0.24,
    ambient_occlusion_intensity=0.5,
)
for k, v in PP.items():
    try:
        s.set_editor_property(k, v)
        s.set_editor_property("override_" + k, True)
    except Exception as e:
        log("  pp %s: %s" % (k, e))
ppv.set_editor_property("settings", s)

# --- one CineCamera per shot ------------------------------------------------------------------
cam_actors = {}
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
    cam_actors[shot["name"]] = cam

if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("first save_map failed")
log("saved %s (before sublevel)" % MAP)

# --- City Park as an always-loaded streaming sublevel, then save the PERSISTENT map by path ----
streaming = unreal.EditorLevelUtils.add_level_to_world(world, SHOWCASE, unreal.LevelStreamingAlwaysLoaded)
if not streaming:
    fail("add_level_to_world(%s) failed" % SHOWCASE)
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    fail("second save_map failed")
log("saved %s with Showcase streaming" % MAP)

# --- Level Sequence ---------------------------------------------------------------------------
seq = tools.create_asset("SEQ_Carve", CINE, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
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
    sec.set_range(shot["start"], shot["end"] + 1)
    chans = {str(c.channel_name): c for c in sec.get_all_channels()}
    log("  transform channels: %s" % list(chans))
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
    fsec.set_range(shot["start"], shot["end"] + 1)
    fch = fsec.get_all_channels()[0]
    for key in shot["keys"]:
        fch.add_key(unreal.FrameNumber(key[0]), key[7], interpolation=INTERP)

    cut = cut_track.add_section()
    cut.set_range(shot["start"], shot["end"] if shot["end"] < cams["frames"] else cams["frames"])
    cut.set_camera_binding_id(seq.get_binding_id(b))
    log("shot %s: frames %d..%d, %d keys" % (shot["name"], shot["start"], shot["end"], len(shot["keys"])))

eal.save_asset(seq.get_path_name())
log("saved sequence %s" % seq.get_path_name())

# --- Movie Render Queue configs ----------------------------------------------------------------
CVARS = {
    "r.MotionBlurQuality": 4,
    "r.DepthOfFieldQuality": 4,
    "r.BloomQuality": 5,
    "r.Tonemapper.Quality": 5,
    "r.ScreenPercentage": 100,
    "r.ViewDistanceScale": 3,
    "r.Shadow.Virtual.Enable": 1,
    "r.Shadow.Virtual.ResolutionLodBiasDirectional": -1.5,
    "r.Lumen.TraceMeshSDFs": 1,
    "r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.FullResDepth": 1,
    "r.Lumen.Reflections.DownsampleFactor": 1,
    "r.Lumen.ScreenProbeGather.DownsampleFactor": 8,
    "r.VolumetricFog.GridPixelSize": 4,
    "r.VolumetricFog.GridSizeZ": 128,
    "r.VolumetricCloud.ViewRaySampleMaxCount": 1024,
    "r.SkyAtmosphere.SampleCountMax": 64,
    "foliage.DensityScale": 1,
    "grass.DensityScale": 1,
    "foliage.LODDistanceScale": 3,
    "r.StaticMeshLODDistanceScale": 0.25,
    "r.Streaming.FullyLoadUsedTextures": 1,
}


def make_config(name, res, spatial, temporal, png=True, warmup=48, cvars=True, frame_range=None, out_dir=None):
    cfg = tools.create_asset(name, CINE, unreal.MoviePipelinePrimaryConfig, unreal.MoviePipelinePrimaryConfigFactory())
    out = cfg.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
    out.output_directory = unreal.DirectoryPath(os.path.join(FRAMES_DIR, out_dir or name))
    if frame_range:
        # One camera cut per job. With temporal samples, MRQ 5.7 rendered every shot after the
        # first but wrote none of them ("Not all frames were fully submitted"); a job per shot
        # range does not hit that.
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
    if cvars:
        cv = cfg.find_or_add_setting_by_class(unreal.MoviePipelineConsoleVariableSetting)
        for k, v in CVARS.items():
            try:
                cv.add_or_update_console_variable(k, float(v))
            except Exception as e:
                log("  cvar %s: %s" % (k, e))
    eal.save_asset(cfg.get_path_name())
    log("saved config %s %sx%s spatial %d temporal %d" % (cfg.get_path_name(), res[0], res[1], spatial, temporal))


make_config("MRQ_Preview", (960, 540), 1, 1, png=False, warmup=16, cvars=False)
make_config("MRQ_Final", (1920, 1080), 1, 8, png=True, warmup=64, cvars=True)
make_config("MRQ_Still", (1920, 1080), 2, 16, png=True, warmup=64, cvars=True)
for shot in cams["shots"]:
    end = min(shot["end"], cams["frames"])
    make_config("MRQ_Final_" + shot["name"], (1920, 1080), 1, 8, png=True, warmup=64, cvars=True,
                frame_range=(shot["start"], end), out_dir="MRQ_Final_shots")
    make_config("MRQ_Preview_" + shot["name"], (960, 540), 1, 1, png=False, warmup=16, cvars=False,
                frame_range=(shot["start"], end), out_dir="MRQ_Preview_shots")
log("DONE")
