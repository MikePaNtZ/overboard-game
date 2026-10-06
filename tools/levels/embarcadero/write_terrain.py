#!/usr/bin/env python3
"""Write terrain/levels/OB_Embarcadero.terrain and regenerate the verification header.

    python tools/levels/embarcadero/write_terrain.py <repo_root>

The terrain CI requires every committed .umap to declare its drivable surfaces in a .terrain file
whose level_hash is the FNV-1a 64 of the .umap bytes. Runs AFTER the final save.
"""
import os
import subprocess
import sys

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
UMAP = os.path.join(REPO, "Content/Maps/OB_Embarcadero.umap")
DECL = os.path.join(REPO, "terrain/levels/OB_Embarcadero.terrain")
HEADER = os.path.join(REPO, "Source/OverboardGame/Public/TerrainVerification.g.h")


def fnv1a64(path):
    h = 0xCBF29CE484222325
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            for byte in chunk:
                h ^= byte
                h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


NOTE = ("The ridden corridor, the kerbs, the rails and the buildings are meshes sampled from the "
        "MuJoCo course height (embarcadero course, course_height.npy) by tools/levels/embarcadero/"
        "gen_embarcadero.py, so the DRAWN corridor is the RIDDEN heightfield plus 4.0 mm. "
        "tools/levels/embarcadero/verify_embarcadero.py checks the corridor against course_height "
        "along the demo path. sim-host rides course_hfield.bin itself, so the terrain reaches the "
        "physics through the heightfield, not through this level. Unreal computes no board physics.")
SOURCE = ("Generated meshes built at level-build time from /tmp OBM files that are not in this "
          "repo, so this check cannot open them. To verify: commit the corridor OBM (or an STL "
          "export) and declare it as kind mesh. Map data (c) OpenStreetMap contributors, ODbL 1.0.")


def main():
    if not os.path.exists(UMAP):
        sys.exit("map not found: %s" % UMAP)
    h = fnv1a64(UMAP)
    with open(DECL, "w") as f:
        f.write("level      Content/Maps/OB_Embarcadero.umap\n")
        f.write("level_hash %016x\n\n" % h)
        f.write("note %s\n\n" % NOTE)
        f.write("surface embarcadero_corridor\n")
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
