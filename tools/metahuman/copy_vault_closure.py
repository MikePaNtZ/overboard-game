#!/usr/bin/env python3
"""Copy the dependency closure of Fab vault packages (City Sample by default) from the local Fab vault into Content/.

usage: copy_vault_closure.py [--copy] [--vault=<VaultCache content dir>] /Game/Path/To/Asset [...]

The default vault is City Sample. For the MonoWheel pack use
--vault=/Users/Shared/UnrealEngine/Launcher/VaultCache/MonoWheeb9a4dee92f06V2/data/Content

The skater rider (tools/metahuman/build_skater.sh) needs:
  tools/metahuman/copy_vault_closure.py --copy \
    /Game/Crowd/Character/Male/NormalWeight/Meshes/m_tal_nrw_crewneck \
    /Game/Crowd/Character/Male/NormalWeight/Meshes/m_tal_nrw_jeans \
    /Game/Crowd/Character/Male/NormalWeight/Meshes/m_tal_nrw_loafers \
    /Game/Crowd/Character/Male/m_002/Face/m_002_nrw_FaceMesh \
    /Game/Crowd/Character/Male/m_002/Hair/Hair/Hair_S_Messy
  tools/metahuman/copy_vault_closure.py --copy \
    --vault=/Users/Shared/UnrealEngine/Launcher/VaultCache/MonoWheeb9a4dee92f06V2/data/Content \
    /Game/MonoWheel_Board/Demo/UE5/Mannequins/Meshes/SKM_Manny_Simple

Without --copy it only reports. The closure is found by scanning each .uasset for '/Game/...'
strings. An import table stores a name with a numeric suffix (T_ShirtPattern_10) split in two,
so a path that does not resolve also pulls in every numbered sibling. The copied content is
licensed to Mike's Epic account and is gitignored; never commit it (see docs/carve-render.md).
"""
import glob
import os
import re
import shutil
import sys

VAULT = "/Users/Shared/UnrealEngine/Launcher/VaultCache/CitySample_5.7/data/Content"
DEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Content")
PATTERN = re.compile(rb"/Game/[A-Za-z0-9_/\-]+")


def closure(start):
    seen, todo, files = set(), list(start), []
    while todo:
        pkg = todo.pop()
        if pkg in seen:
            continue
        seen.add(pkg)
        rel = pkg[len("/Game/"):]
        path = next((os.path.join(VAULT, rel + e) for e in (".uasset", ".umap")
                     if os.path.exists(os.path.join(VAULT, rel + e))), None)
        if path is None:
            for sib in glob.glob(os.path.join(VAULT, rel + "_*.uasset")):
                todo.append("/Game/" + os.path.relpath(sib, VAULT)[:-len(".uasset")])
            continue
        files.append(path)
        for m in set(PATTERN.findall(open(path, "rb").read())):
            todo.append(m.decode().split(".")[0])
    return files


def main():
    global VAULT
    for a in sys.argv[1:]:
        if a.startswith("--vault="):
            VAULT = a.split("=", 1)[1]
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    files = closure(args)
    new = 0
    for f in files:
        base = f.rsplit(".", 1)[0]
        for ext in (".uasset", ".umap", ".ubulk", ".uexp", ".uptnl"):
            src = base + ext
            if not os.path.exists(src):
                continue
            dst = os.path.join(DEST, os.path.relpath(src, VAULT))
            if os.path.exists(dst):
                continue
            new += 1
            if "--copy" in sys.argv:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
    print(f"packages {len(files)}, files not yet local {new}{' (copied)' if '--copy' in sys.argv else ''}")


if __name__ == "__main__":
    main()
