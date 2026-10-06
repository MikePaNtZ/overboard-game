# set_gamemode.py -- editor script (run with tools/levels/ue.sh): set the World Settings game mode
# override of the levels-track maps to AOverboardGameMode_NoGround, and save them.
#
# Why: the base AOverboardGameMode spawns the motion-reference markers (an 8 m grid of grey cubes,
# cylinders and cones, overboard#162) and a placeholder ground for the featureless OB_Main plane.
# A level with its own ground and dressing must not get them. NoGround turns both off.
# The build scripts set the same override; this script patches already-built maps.
import os

import unreal

MAPS = os.environ.get("OB_GAMEMODE_MAPS", "/Game/Maps/OB_ParkingLot,/Game/Maps/OB_Embarcadero").split(",")


def apply_no_ground(world):
    cls = unreal.load_class(None, "/Script/OverboardGame.OverboardGameMode_NoGround")
    if not cls:
        raise RuntimeError("class OverboardGameMode_NoGround not found")
    ws = world.get_world_settings()
    ws.set_editor_property("default_game_mode", cls)
    return ws.get_editor_property("default_game_mode")


if __name__ == "__main__":
    for m in MAPS:
        world = unreal.EditorLoadingAndSavingUtils.load_map(m)
        if not world:
            raise RuntimeError("load_map(%s) failed" % m)
        got = apply_no_ground(world)
        if not unreal.EditorLoadingAndSavingUtils.save_map(world, m):
            raise RuntimeError("save_map(%s) failed" % m)
        unreal.log("set_gamemode: %s -> %s" % (m, got.get_name() if got else None))
