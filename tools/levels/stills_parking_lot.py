# stills_parking_lot.py -- UE editor python. Builds a small Level Sequence of static cameras and a
# Movie Render Queue still config for OB_ParkingLot, so tools/render/render.sh renders review stills
# offscreen (an editor screenshot does not flush in a one-shot headless commandlet; MRQ does). One
# camera per frame: an overhead of the whole lot and three low views (the start straight, the
# aisles/slalom, ramp B). Run headless from tools/levels/build_parking_lot.sh; the render follows.
import os

import unreal

WORK = os.environ.get("OB_LEVELS_WORK", "/tmp/ob-levels-map")
MAP = "/Game/Maps/OB_ParkingLot"
CINE = "/Game/ParkingLot/Cinematics"
FRAMES = os.environ.get("OB_FRAMES_DIR", os.path.join(WORK, "frames"))

eal = unreal.EditorAssetLibrary
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()
les.load_level(MAP)

# (name, location_cm, pitch, yaw, focal_mm) in the Unreal frame.
SHOTS = [
    ("overhead", (0.0, 0.0, 7000.0), -90.0, -90.0, 13.0),
    ("start_straight", (-4700.0, 3600.0, 350.0), -5.0, -35.0, 24.0),
    ("aisles_slalom", (3400.0, 300.0, 1100.0), -22.0, 178.0, 20.0),
    ("ramp_b", (-5250.0, 1400.0, 650.0), -10.0, -115.0, 24.0),
]

if eal.does_directory_exist(CINE):
    eal.delete_directory(CINE)

gm = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")

cams = []
for name, loc, pitch, yaw, focal in SHOTS:
    cam = eas.spawn_actor_from_class(unreal.CineCameraActor, unreal.Vector(*loc),
                                     unreal.Rotator(pitch=pitch, yaw=yaw, roll=0.0))
    cam.set_actor_label("OB_PL_Cam_" + name)
    cc = cam.get_cine_camera_component()
    cc.set_editor_property("filmback", unreal.CameraFilmbackSettings(sensor_width=36.0, sensor_height=20.25))
    cc.set_editor_property("current_focal_length", focal)
    cc.set_editor_property("current_aperture", 8.0)
    fs = cc.focus_settings
    fs.set_editor_property("focus_method", unreal.CameraFocusMethod.DISABLE)
    cc.set_editor_property("focus_settings", fs)
    cams.append((name, cam, loc, pitch, yaw))

les.save_current_level()

seq = tools.create_asset("SEQ_ParkingLot", CINE, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
seq.set_display_rate(unreal.FrameRate(24, 1))
seq.set_playback_start(0)
seq.set_playback_end(len(SHOTS))
cut = seq.add_track(unreal.MovieSceneCameraCutTrack)
for i, (name, cam, loc, pitch, yaw) in enumerate(cams):
    b = seq.add_possessable(cam)
    tr = b.add_track(unreal.MovieScene3DTransformTrack)
    sec = tr.add_section()
    sec.set_range(i, i + 1)
    chans = {str(c.channel_name): c for c in sec.get_all_channels()}
    vals = {"Location.X": loc[0], "Location.Y": loc[1], "Location.Z": loc[2],
            "Rotation.X": 0.0, "Rotation.Y": pitch, "Rotation.Z": yaw,
            "Scale.X": 1.0, "Scale.Y": 1.0, "Scale.Z": 1.0}
    for k, v in vals.items():
        chans[k].set_default(v)
    cs = cut.add_section()
    cs.set_range(i, i + 1)
    cs.set_camera_binding_id(seq.get_binding_id(b))
eal.save_asset(seq.get_path_name())

cfg = tools.create_asset("MRQ_Still_ParkingLot", CINE, unreal.MoviePipelinePrimaryConfig,
                         unreal.MoviePipelinePrimaryConfigFactory())
out = cfg.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
out.output_directory = unreal.DirectoryPath(os.path.join(FRAMES, "MRQ_Still_ParkingLot"))
out.file_name_format = "pl_{frame_number}"
out.output_resolution = unreal.IntPoint(1920, 1080)
out.set_editor_property("zero_pad_frame_numbers", 1)
out.set_editor_property("override_existing_output", True)
cfg.find_or_add_setting_by_class(unreal.MoviePipelineDeferredPassBase)
cfg.find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_PNG)
aa = cfg.find_or_add_setting_by_class(unreal.MoviePipelineAntiAliasingSetting)
aa.set_editor_property("spatial_sample_count", 2)
aa.set_editor_property("temporal_sample_count", 1)
aa.set_editor_property("override_anti_aliasing", True)
aa.set_editor_property("anti_aliasing_method", unreal.AntiAliasingMethod.AAM_TSR)
aa.set_editor_property("use_camera_cut_for_warm_up", False)
aa.set_editor_property("engine_warm_up_count", 32)
aa.set_editor_property("render_warm_up_count", 32)
aa.set_editor_property("render_warm_up_frames", True)
go = cfg.find_or_add_setting_by_class(unreal.MoviePipelineGameOverrideSetting)
go.set_editor_property("game_mode_override", gm)
go.set_editor_property("cinematic_quality_settings", True)
eal.save_asset(cfg.get_path_name())
print("stills: sequence + MRQ config built, %d shots" % len(SHOTS))
