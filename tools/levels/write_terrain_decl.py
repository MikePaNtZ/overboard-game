#!/usr/bin/env python3
"""Write terrain/levels/OB_ParkingLot.terrain and regenerate the verification header.

    python tools/levels/write_terrain_decl.py <repo_root>

The terrain CI (.github/workflows/terrain.yml) requires every committed .umap to declare its
drivable surfaces in a .terrain file whose level_hash is the FNV-1a 64 of the .umap bytes. Any
re-save of the map changes the hash, so this runs AFTER the final save. It then rebuilds the
terraincheck tool and regenerates Source/OverboardGame/Public/TerrainVerification.g.h, so the
on-screen "unverified" table cannot drift from the declarations.
"""
import os
import subprocess
import sys

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
UMAP = os.path.join(REPO, "Content/Maps/OB_ParkingLot.umap")
DECL = os.path.join(REPO, "terrain/levels/OB_ParkingLot.terrain")
HEADER = os.path.join(REPO, "Source/OverboardGame/Public/TerrainVerification.g.h")


def fnv1a64(path):
    h = 0xCBF29CE484222325
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            for byte in chunk:
                h ^= byte
                h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


NOTE = (
    "The ground, every obstacle box and every marking are meshes sampled from the MuJoCo course "
    "height (parking_lot course, course_height.npy) by tools/levels/gen_parking_lot.py, so the "
    "DRAWN ground is the RIDDEN heightfield plus 4.0 mm. tools/levels/verify_parking_lot.py checks "
    "the drawn ground against course_height along the demo path (worst gap 2.0 mm). sim-host rides "
    "course_hfield.bin itself, so the terrain reaches the physics through the heightfield, not "
    "through this level. The 0.15 m kerbs and the ramps are real steps and slopes in that "
    "heightfield. Unreal computes no board physics (HARD RULE)."
)
SOURCE = (
    "Generated meshes (SM_PL_Lot_*, SM_PL_Ramp, SM_PL_Boxes) built at level-build time from /tmp "
    "OBM files that are not in this repo, so this check cannot open them. To verify: commit the "
    "ground OBM (or an STL export) and declare it as kind mesh."
)


def main():
    if not os.path.exists(UMAP):
        sys.exit("map not found: %s (build it first)" % UMAP)
    h = fnv1a64(UMAP)
    with open(DECL, "w") as f:
        f.write("level      Content/Maps/OB_ParkingLot.umap\n")
        f.write("level_hash %016x\n\n" % h)
        f.write("note %s\n\n" % NOTE)
        f.write("surface parking_lot_ground\n")
        f.write("  kind      external\n")
        f.write("  status    unverified\n")
        f.write("  reach_m   100.0\n")
        f.write("  source    %s\n" % SOURCE)
    print("wrote %s (level_hash %016x)" % (DECL, h))
    terrain = os.path.join(REPO, "terrain")
    subprocess.run(["make", "terraincheck"], cwd=terrain, check=True)
    subprocess.run(["./terraincheck", "--repo-root", "..", "--emit-header", HEADER], cwd=terrain, check=True)
    print("regenerated %s" % HEADER)


if __name__ == "__main__":
    main()
