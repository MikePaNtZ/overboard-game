# verify_trail.py -- UE editor python. Checks that a replayed track rides ON the OB_Trail surface.
#
#   OB_TRACK_BIN=/tmp/ob-trail/track.bin tools/trail/ue.sh tools/trail/verify_trail.py
#
# For samples along the track it maps the board pose MuJoCo -> UE exactly as ABoardActor does
# (wire/CoordinateTransform: (x, -y, z) * 100 cm, then the PlayerStart yaw and location), traces
# straight down at the wheel onto the landscape, and compares the tyre's lowest point with it.
# The generated meshes (asphalt, deck) carry no physics in the editor scene, so the trace sees the
# landscape only. The asphalt mesh is the landscape plus ribbon_lift_m by construction
# (gen_course.py); over the bridge the landscape is cut for the creek, so there the reference is
# the deck top (meta.json bridge.deck_z). It writes /tmp/ob-trail/verify.txt.
# The tyre is a circle of radius R about the axle in the board's x-z plane, so its lowest point is
# R below the axle on flat ground and R / cos(pitch) above the ground along the vertical on a grade.
import json
import math
import os
import struct

import unreal

BIN = os.environ.get("OB_TRACK_BIN", "/tmp/ob-trail/track.bin")
OUT = os.environ.get("OB_VERIFY_OUT", "/tmp/ob-trail/verify.txt")
STEP_M = float(os.environ.get("OB_VERIFY_STEP_M", "5.0"))
WHEEL_R = 0.1454
FMT = "<IHHQd3f4f5f2f3f3f"
SIZE = struct.calcsize(FMT)
META = json.load(open(os.path.join(os.environ.get("OB_TRAIL_DATA", "/tmp/ob-trail/valley_gentle"), "meta.json")))
BRIDGE = META.get("bridge")
LIFT_MM = META["path"]["ribbon_lift_m"] * 1000.0

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
les.load_level("/Game/Maps/OB_Trail")
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
starts = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PlayerStart)]
ps = starts[0]
origin = ps.get_actor_location()
yaw = math.radians(ps.get_actor_rotation().yaw)
c, s = math.cos(yaw), math.sin(yaw)

data = open(BIN, "rb").read()
rows = [struct.unpack_from(FMT, data, k) for k in range(0, len(data) - SIZE + 1, SIZE)]
out = open(OUT, "w")
out.write("track %s: %d samples; PlayerStart %s yaw %.2f\n" % (BIN, len(rows), origin, math.degrees(yaw)))
out.write("%8s %8s %9s %9s %9s %8s  %s\n" % ("t_s", "x_mj_m", "axle_z_cm", "tyre_z_cm", "surf_z_cm", "gap_mm", "surface"))
last_x, worst, n = None, 0.0, 0
for r in rows:
    t, px, py, pz, qw, qx, qy, qz = r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]
    if last_x is not None and abs(px - last_x) < STEP_M:
        continue
    last_x = px
    # pitch from the quaternion (rotation about MuJoCo +Y)
    pitch = math.asin(max(-1.0, min(1.0, 2 * (qw * qy - qz * qx))))
    ux, uy, uz = px * 100.0, -py * 100.0, pz * 100.0
    wx, wy = ux * c - uy * s + origin.x, ux * s + uy * c + origin.y
    wz = uz + origin.z
    tyre = wz - WHEEL_R * 100.0 / max(math.cos(pitch), 0.5)
    hit = unreal.SystemLibrary.line_trace_single(world, unreal.Vector(wx, wy, wz + 300.0), unreal.Vector(wx, wy, wz - 600.0),
                                                 unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, [], unreal.DrawDebugTrace.NONE,
                                                 True)
    if hit is None:
        out.write("%8.2f %8.2f %9.2f %9.2f %9s %8s  NO HIT\n" % (t, px, wz, tyre, "-", "-"))
        continue
    tup = hit.to_tuple()
    loc, actor = tup[4], tup[9]
    surf_z, label = loc.z, (actor.get_actor_label() if actor else "?")
    if BRIDGE and BRIDGE["x0"] <= px <= BRIDGE["x1"]:
        surf_z, label = BRIDGE["deck_z"] * 100.0, "bridge deck (meta.json deck_z)"
    gap = (tyre - surf_z) * 10.0
    worst = max(worst, abs(gap))
    n += 1
    out.write("%8.2f %8.2f %9.2f %9.2f %9.2f %8.2f  %s\n" % (t, px, wz, tyre, surf_z, gap, label))
out.write("samples %d, worst |gap| %.2f mm (tyre bottom vs the landscape or the deck; + = tyre above). The asphalt "
          "mesh adds %.1f mm on top of the landscape.\n" % (n, worst, LIFT_MM))
out.write("PASS\n" if n and worst < 10.0 else "FAIL\n")
out.close()
