"""Retarget the step-off animations onto the MetaHuman skater (game track).

The game plays these when a crash (the ADR-0012 handoff) comes at walking speed: the rider steps
off to the side and stands, instead of falling (MuJoCo's step_off case: 0.5 m to the side in
0.8 s). Same method as tools/metahuman/post_build_skater.py (auto IK rigs, default retarget ops),
with its own rig and retargeter names, so the render track's assets do not change.

Source: Epic's UE5 template mannequin (Content/Characters/Mannequins, gitignored template content).
Output: /Game/MetaHumans/Skater/Anims/StepOff/*_MH (gitignored with the skater).

Run in the editor (the batch retarget needs Slate):
  UnrealEditor <abs uproject> -ExecutePythonScript=<abs this file> -OBQuitWhenDone -RenderOffscreen
Log: /tmp/ob-game/step_off_retarget.txt
"""
import os

import unreal

LOG_PATH = "/tmp/ob-game/step_off_retarget.txt"
SOURCE_MESH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"
SOURCE_ANIMS = [
    "/Game/Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Left",
    "/Game/Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Right",
    "/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle",
]
BUILD_DIR = "/Game/MetaHumans/Skater"
RIG_DIR = "/Game/MetaHumans/Skater/Retarget"
OUT_DIR = "/Game/MetaHumans/Skater/Anims/StepOff"
SUFFIX = "_MH"

os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
_log = open(LOG_PATH, "w")


def log(*parts):
    _log.write(" ".join(str(p) for p in parts) + "\n")
    _log.flush()


def fresh(path):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)


def make_ik_rig(name, mesh):
    fresh(f"{RIG_DIR}/{name}")
    rig = unreal.IKRigDefinitionFactory.create_new_ik_rig_asset(RIG_DIR, name)
    ctrl = unreal.IKRigController.get_controller(rig)
    ctrl.set_skeletal_mesh(mesh)
    log(name, "retarget definition", ctrl.apply_auto_generated_retarget_definition(), "fbik", ctrl.apply_auto_fbik())
    return rig


def find_body_mesh():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for data in registry.get_assets_by_path(BUILD_DIR, recursive=True):
        if str(data.asset_class_path.asset_name) == "SkeletalMesh" and "body" in str(data.asset_name).lower():
            return unreal.load_asset(str(data.package_name))
    return None


def main():
    source_mesh = unreal.load_asset(SOURCE_MESH)
    target_mesh = find_body_mesh()
    log("source", source_mesh, "target", target_mesh)
    if not source_mesh or not target_mesh:
        log("FAIL: a mesh is missing")
        return
    source_rig = make_ik_rig("IK_Manny_StepOff", source_mesh)
    target_rig = make_ik_rig("IK_Skater_StepOff", target_mesh)
    fresh(f"{RIG_DIR}/RTG_Manny_to_Skater_StepOff")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    rtg = tools.create_asset("RTG_Manny_to_Skater_StepOff", RIG_DIR, unreal.IKRetargeter, unreal.IKRetargetFactory())
    rc = unreal.IKRetargeterController.get_controller(rtg)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, source_rig)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, target_rig)
    rc.set_preview_mesh(unreal.RetargetSourceOrTarget.SOURCE, source_mesh)
    rc.set_preview_mesh(unreal.RetargetSourceOrTarget.TARGET, target_mesh)
    if rc.get_num_retarget_ops() == 0:
        rc.add_default_ops()
    rc.assign_ik_rig_to_all_ops(unreal.RetargetSourceOrTarget.SOURCE, source_rig)
    rc.assign_ik_rig_to_all_ops(unreal.RetargetSourceOrTarget.TARGET, target_rig)
    rc.auto_map_chains(unreal.AutoMapChainType.FUZZY, True)
    for a in (source_rig, target_rig, rtg):
        unreal.EditorAssetLibrary.save_loaded_asset(a, False)

    sources = [unreal.EditorAssetLibrary.find_asset_data(p) for p in SOURCE_ANIMS]
    log("retargeting", [str(s.asset_name) for s in sources])
    out = unreal.IKRetargetBatchOperation.duplicate_and_retarget(
        sources, source_mesh, target_mesh, rtg, "", "", "", SUFFIX, True, True)
    unreal.EditorAssetLibrary.make_directory(OUT_DIR)
    for data in out:
        src = str(data.package_name)
        dst = f"{OUT_DIR}/{data.asset_name}"
        if src != dst:
            fresh(dst)
            unreal.EditorAssetLibrary.rename_asset(src, dst)
        log("  ->", dst)
    unreal.EditorAssetLibrary.save_directory(OUT_DIR, only_if_is_dirty=False, recursive=True)
    log("OK")


try:
    main()
except Exception:
    import traceback
    log("EXCEPTION", traceback.format_exc())
finally:
    _log.close()
    if "-obquitwhendone" in unreal.SystemLibrary.get_command_line().lower():
        unreal.SystemLibrary.quit_editor()
