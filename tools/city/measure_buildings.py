# measure_buildings.py -- UE editor python. Spawns each City Sample building once, measures its
# world bounds, logs them, and writes /tmp/ob-city/building_bounds.json. gen_city.py reads that
# file to lay the buildings out by their real footprint. Run headless (tools/city/ue.sh):
#
#   tools/city/ue.sh tools/city/measure_buildings.py
#
# Each building is a City Sample "packed level actor" (BPP_*). This script also reports whether it
# spawns from python in 5.7, so the build can fall back if one does not (see docs/city-level.md).
import json
import os

import unreal

OUT = os.environ.get("OB_CITY_BOUNDS", "/tmp/ob-city/building_bounds.json")
LOG = os.environ.get("OB_BUILD_LOG", "/tmp/ob-city/measure.txt")
os.makedirs(os.path.dirname(LOG), exist_ok=True)
_log = open(LOG, "w")


def log(*m):
    _log.write(" ".join(str(x) for x in m) + "\n")
    _log.flush()


# label -> content path. The front-row SF kit and the SF heroes the level uses.
BUILDINGS = {
    "SFA_Ref": "/Game/Building/Library/Kit_Ref_Bldg/BPP_SFA_Ref_N1",
    "SFA_Ref_L1": "/Game/Building/Library/Kit_Ref_Bldg/BPP_SFA_Ref_Level01_N1",
    "SFB_Ref": "/Game/Building/Library/Kit_Ref_Bldg/BPP_SFB_Ref_N1",
    "SFJ_Ref": "/Game/Building/Library/Kit_Ref_Bldg/BPP_SFJ_Ref",
    "Hero_SFD_Long": "/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Low_SFD_Long_01",
    "Hero_SFA_Tri": "/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFA_Triangle_A01",
    "Hero_SFC_A": "/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_A01",
    "Hero_SFC_B": "/Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_B01",
}

eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
les.load_level("/Game/Maps/OB_Main")


def spawn_building(path):
    """Spawn a BPP_ building and return (actor, how). Try the packed-level-actor class, then a
    plain Blueprint generated class."""
    asset = unreal.load_asset(path)
    if asset is None:
        return None, "asset missing"
    how = asset.get_class().get_name()
    cls = None
    if isinstance(asset, unreal.Blueprint):
        cls = asset.generated_class()
        how = "blueprint -> " + (cls.get_name() if cls else "None")
    if cls is None:
        cls = unreal.load_object(None, path + "_C") or unreal.load_object(None, path + ".%s_C" % os.path.basename(path))
    if cls is None:
        return None, how + " (no class)"
    a = eas.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    return a, how


out = {}
for label, path in BUILDINGS.items():
    a, how = spawn_building(path)
    if a is None:
        log("%-16s SPAWN FAILED (%s) path %s" % (label, how, path))
        out[label] = dict(path=path, ok=False, how=how)
        continue
    try:
        a.set_actor_location(unreal.Vector(0, 0, 0), False, False)
    except Exception:
        pass
    origin, extent = a.get_actor_bounds(False, True)
    if max(extent.x, extent.y, extent.z) < 50.0:
        # A packed level actor's instanced components may not report through the actor bounds.
        # Union every primitive component's world bounds instead.
        lo = unreal.Vector(1e9, 1e9, 1e9)
        hi = unreal.Vector(-1e9, -1e9, -1e9)
        found = False
        for comp in a.get_components_by_class(unreal.PrimitiveComponent):
            try:
                b = comp.get_local_bounds()  # (box_min, box_max) in local space
            except Exception:
                continue
            bo, be = comp.get_world_bounds() if hasattr(comp, "get_world_bounds") else (None, None)
            try:
                bb = comp.bounds
                c, e = bb.origin, bb.box_extent
            except Exception:
                continue
            found = True
            lo = unreal.Vector(min(lo.x, c.x - e.x), min(lo.y, c.y - e.y), min(lo.z, c.z - e.z))
            hi = unreal.Vector(max(hi.x, c.x + e.x), max(hi.y, c.y + e.y), max(hi.z, c.z + e.z))
        if found:
            origin = unreal.Vector((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2)
            extent = unreal.Vector((hi.x - lo.x) / 2, (hi.y - lo.y) / 2, (hi.z - lo.z) / 2)
    out[label] = dict(path=path, ok=True, how=how,
                      origin=[origin.x, origin.y, origin.z], extent=[extent.x, extent.y, extent.z])
    log("%-16s %-22s origin (%.0f %.0f %.0f) extent (%.0f %.0f %.0f) cm  foot %.1f x %.1f m  h %.1f m"
        % (label, how, origin.x, origin.y, origin.z, extent.x, extent.y, extent.z,
           2 * extent.x / 100.0, 2 * extent.y / 100.0, 2 * extent.z / 100.0))
    eas.destroy_actor(a)

json.dump(out, open(OUT, "w"), indent=1)
log("wrote", OUT)
_log.close()
