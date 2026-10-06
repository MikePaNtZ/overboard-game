# stills_embarcadero.py -- UE editor python. Builds a Level Sequence of static cameras and an MRQ
# still config for OB_Embarcadero, so tools/render/render.sh renders the review stills offscreen.
# Shots: an overhead of the whole route, a chase at the spawn, the Brannan bike lane, the promenade
# with the bay, the racket loop at Bryant Street, and King Street by the ballpark corner.
import json
import math
import os

import unreal

WORK = os.environ.get("OB_LEVELS_WORK", "/tmp/ob-levels-sf")
CDIR = os.environ.get("OB_COURSE_DIR", "")
MAP = "/Game/Maps/OB_Embarcadero"
CINE = "/Game/Embarcadero/Cinematics"
FRAMES = os.environ.get("OB_FRAMES_DIR", os.path.join(WORK, "frames"))

lvl = json.load(open(os.path.join(CDIR, "level.json")))
J = lvl["junctions"]
path = lvl["demo_path"]
sp = lvl["spawn"]
HM = json.load(open(os.path.join(os.environ.get("OB_SF_DATA", "/tmp/ob-levels-sf/data"), "meta.json")))["heightmap"]

eal = unreal.EditorAssetLibrary
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()
les.load_level(MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def ground_z(x, y):
    """Bilinear sample of the coarse 5 m heightmap the generator wrote (m, MuJoCo frame)."""
    dx, x0, y0, nx, ny = HM["dx"], HM["x0"], HM["y0"], HM["nx"], HM["ny"]
    z = HM["z"]
    fx = min(max((x - x0) / dx, 0), nx - 1.001)
    fy = min(max((y - y0) / dx, 0), ny - 1.001)
    ix, iy = int(fx), int(fy)
    tx, ty = fx - ix, fy - iy
    g = lambda a, b: z[a * ny + b]
    return (g(ix, iy) * (1 - tx) * (1 - ty) + g(ix + 1, iy) * tx * (1 - ty)
            + g(ix, iy + 1) * (1 - tx) * ty + g(ix + 1, iy + 1) * tx * ty)


def nearest_heading(x, y):
    best, bh = 1e18, 0.0
    for i in range(0, len(path), 3):
        a = path[i]
        d = (a[0] - x) ** 2 + (a[1] - y) ** 2
        if d < best:
            b = path[(i + 2) % len(path)]
            best, bh = d, math.atan2(b[1] - a[1], b[0] - a[0])
    return bh


def look(ax, ay, tx, ty, up=1.6, pitch=-4.0):
    """A camera at (ax, ay) up metres above the ground, looking toward (tx, ty)."""
    z = ground_z(ax, ay) + up
    hdg = math.atan2(ty - ay, tx - ax)
    return (100.0 * ax, -100.0 * ay, 100.0 * z), pitch, -math.degrees(hdg)


# route bbox (from the demo path) for the overhead
xs = [p[0] for p in path]; ys = [p[1] for p in path]
cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
span = max(max(xs) - min(xs), max(ys) - min(ys))

# spawn chase
sloc, spitch, syaw = look(sp["x"], sp["y"], sp["x"] + math.cos(nearest_heading(sp["x"], sp["y"])),
                          sp["y"] + math.sin(nearest_heading(sp["x"], sp["y"])), up=1.6, pitch=-3.0)
# Brannan bike lane (2nd -> Embarcadero), a third along
bx = J["2nd_brannan"][0] + 0.45 * (J["brannan_emb"][0] - J["2nd_brannan"][0])
by = J["2nd_brannan"][1] + 0.45 * (J["brannan_emb"][1] - J["2nd_brannan"][1])
bloc, bpitch, byaw = look(bx, by, J["brannan_emb"][0], J["brannan_emb"][1], up=1.6, pitch=-3.0)
# promenade + bay: the easternmost route point, looking east toward the water
epx = max(path, key=lambda p: p[0])
ploc, ppitch, pyaw = look(epx[0] - 8.0, epx[1], epx[0] + 40.0, epx[1], up=2.0, pitch=-4.0)
# racket loop at Bryant: a little back and up, looking at the junction
rloc, rpitch, ryaw = look(J["bryant_emb"][0] - 30.0, J["bryant_emb"][1] - 30.0,
                          J["bryant_emb"][0], J["bryant_emb"][1], up=12.0, pitch=-18.0)
# King Street by the ballpark corner (king_2nd), looking east toward king_emb
kloc, kpitch, kyaw = look(J["king_2nd"][0] - 5.0, J["king_2nd"][1] + 5.0,
                          J["king_emb"][0], J["king_emb"][1], up=1.8, pitch=-3.0)
# promenade looking north along the bay (the Bay Bridge is north); pick a promenade point near y=-60
prom_pts = [p for p in path if p[0] > 140.0]
pn = min(prom_pts, key=lambda p: abs(p[1] + 60.0)) if prom_pts else [160.0, -60.0, 2.0]
nloc, npitch, nyaw = look(pn[0], pn[1], pn[0], pn[1] + 60.0, up=1.6, pitch=-2.0)

SHOTS = [
    ("overhead", (100.0 * cx, -100.0 * cy, 100.0 * 520.0), -90.0, 0.0, 20.0, span * 115.0),
    ("chase", sloc, spitch, syaw, 24.0, 0.0),
    ("brannan_bike", bloc, bpitch, byaw, 28.0, 0.0),
    ("promenade", ploc, ppitch, pyaw, 24.0, 0.0),
    ("racket_bryant", rloc, rpitch, ryaw, 24.0, 0.0),
    ("king_ballpark", kloc, kpitch, kyaw, 24.0, 0.0),
    ("promenade_north", nloc, npitch, nyaw, 24.0, 0.0),
]

if eal.does_directory_exist(CINE):
    eal.delete_directory(CINE)
gm = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")

cams = []
for name, loc, pitch, yaw, focal, *ortho in SHOTS:
    cam = eas.spawn_actor_from_class(unreal.CineCameraActor, unreal.Vector(*loc),
                                     unreal.Rotator(pitch=pitch, yaw=yaw, roll=0.0))
    cam.set_actor_label("OB_SF_Cam_" + name)
    cc = cam.get_cine_camera_component()
    cc.set_editor_property("filmback", unreal.CameraFilmbackSettings(sensor_width=36.0, sensor_height=20.25))
    cc.set_editor_property("current_focal_length", focal)
    cc.set_editor_property("current_aperture", 8.0)
    fs = cc.focus_settings
    fs.set_editor_property("focus_method", unreal.CameraFocusMethod.DISABLE)
    cc.set_editor_property("focus_settings", fs)
    if ortho and ortho[0]:
        cc.set_editor_property("projection_mode", unreal.CameraProjectionMode.ORTHOGRAPHIC)
        cc.set_editor_property("ortho_width", float(ortho[0]))
    cams.append((name, cam, loc, pitch, yaw))

les.save_current_level()

seq = tools.create_asset("SEQ_Embarcadero", CINE, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
seq.set_display_rate(unreal.FrameRate(24, 1))
seq.set_playback_start(0)
seq.set_playback_end(len(SHOTS))
cut = seq.add_track(unreal.MovieSceneCameraCutTrack)
for i, (name, cam, loc, pitch, yaw) in enumerate(cams):
    b = seq.add_possessable(cam)
    tr = b.add_track(unreal.MovieScene3DTransformTrack)
    sec = tr.add_section(); sec.set_range(i, i + 1)
    ch = {str(c.channel_name): c for c in sec.get_all_channels()}
    for k, v in {"Location.X": loc[0], "Location.Y": loc[1], "Location.Z": loc[2],
                 "Rotation.X": 0.0, "Rotation.Y": pitch, "Rotation.Z": yaw,
                 "Scale.X": 1.0, "Scale.Y": 1.0, "Scale.Z": 1.0}.items():
        ch[k].set_default(v)
    cs = cut.add_section(); cs.set_range(i, i + 1); cs.set_camera_binding_id(seq.get_binding_id(b))
eal.save_asset(seq.get_path_name())

cfg = tools.create_asset("MRQ_Still_Embarcadero", CINE, unreal.MoviePipelinePrimaryConfig,
                         unreal.MoviePipelinePrimaryConfigFactory())
out = cfg.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
out.output_directory = unreal.DirectoryPath(os.path.join(FRAMES, "MRQ_Still_Embarcadero"))
out.file_name_format = "sf_{frame_number}"
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
