"""Post-build steps for the skater: retarget the riding animation, and bind the hair groom.

Step 3 of build_skater.sh, after finish_skater.py has built /Game/MetaHumans/Skater. The batch
retarget needs Slate, so it runs in the editor (not a commandlet):
  UnrealEditor <abs uproject> -ExecutePythonScript=<abs this file> -OBQuitWhenDone -RenderOffscreen
Log: /tmp/ob-render/mh_post_build.txt

- Source: the MonoWheel pack's SKM_Manny_Simple, on the pack's own SK_Mannequin skeleton (the
  skeleton the riding sequences are bound to). Copy it in with copy_vault_closure.py.
- Target: the MetaHuman body mesh from the build.
- Both IK rigs are auto-generated (retarget chains and full-body IK goals). The retargeter uses
  the default op stack: FK chains plus IK on the feet, so the feet keep the source foot positions.
  The deck contact itself is set at run time by ABoardActor's foot calibration.
- Output: /Game/MetaHumans/Skater/Anims/ (the blendspace and the sequences, suffix _MH).

Hair: the Creator has no grooms until the optional content is installed. HAIR_GROOMS binds City
Sample grooms to the built face mesh, transferred from the crowd head they were authored on (the
face was fitted to that head, so the transfer is close). Output: /Game/MetaHumans/Skater/Grooms/.
Empty the list when the Creator's own grooms are used (create_skater.py GROOMS).
"""
import os
import unreal

LOG_PATH = "/tmp/ob-render/mh_post_build.txt"
OUT_DIR = "/Game/MetaHumans/Skater/Anims"
RIG_DIR = "/Game/MetaHumans/Skater/Retarget"
SOURCE_MESH = "/Game/MonoWheel_Board/Demo/UE5/Mannequins/Meshes/SKM_Manny_Simple"
SOURCE_ANIMS_DIR = "/Game/MonoWheel_Board/Animations/UE5"
BUILD_DIR = "/Game/MetaHumans/Skater"
SUFFIX = "_MH"
GROOM_DIR = "/Game/MetaHumans/Skater/Grooms"
GROOM_SOURCE_HEAD = "/Game/Crowd/Character/Male/m_002/Face/m_002_nrw_FaceMesh"
HAIR_GROOMS = ["/Game/Crowd/Character/Male/m_002/Hair/Hair/Hair_S_Messy"]

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
    log("  root", ctrl.get_retarget_root(), "chains", [str(c.chain_name) for c in ctrl.get_retarget_chains()])
    return rig


def find_mesh(word):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for data in registry.get_assets_by_path(BUILD_DIR, recursive=True):
        if str(data.asset_class_path.asset_name) == "SkeletalMesh" and word in str(data.asset_name).lower():
            return unreal.load_asset(str(data.package_name))
    return None


def bind_grooms():
    face = find_mesh("face")
    source = unreal.load_asset(GROOM_SOURCE_HEAD)
    log("groom target face", face, "source head", source)
    for groom_path in HAIR_GROOMS:
        groom = unreal.load_asset(groom_path)
        if not groom or not face:
            log("MISSING groom or face", groom_path)
            continue
        name = f"GB_Skater_{groom_path.split('/')[-1]}"
        target = f"{GROOM_DIR}/{name}"
        fresh(target)
        binding = unreal.GroomLibrary.create_new_groom_binding_asset_with_path(
            target, groom, face, 100, source, 0)
        if not binding:
            log("FAIL groom binding", target)
            continue
        made = binding.get_path_name().split(".")[0]
        if made != target:
            # The create call picks a free name (…1, …2) when the old package is still there.
            log("groom binding written as", made, "- renaming to", target)
            fresh(target)
            unreal.EditorAssetLibrary.rename_asset(made, target)
        unreal.EditorAssetLibrary.save_asset(target, False)
        log("groom binding", target)


def main():
    bind_grooms()
    source_mesh = unreal.load_asset(SOURCE_MESH)
    target_mesh = find_mesh("body")
    log("source mesh", source_mesh, "\ntarget mesh", target_mesh)
    if not source_mesh or not target_mesh:
        log("FAIL: a mesh is missing")
        return

    source_rig = make_ik_rig("IK_MonoWheelManny", source_mesh)
    target_rig = make_ik_rig("IK_Skater", target_mesh)

    fresh(f"{RIG_DIR}/RTG_MonoWheelManny_to_Skater")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    rtg = tools.create_asset("RTG_MonoWheelManny_to_Skater", RIG_DIR, unreal.IKRetargeter, unreal.IKRetargetFactory())
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
    log("ops", [str(rc.get_op_name(i)) for i in range(rc.get_num_retarget_ops())])
    unreal.EditorAssetLibrary.save_loaded_asset(source_rig, False)
    unreal.EditorAssetLibrary.save_loaded_asset(target_rig, False)
    unreal.EditorAssetLibrary.save_loaded_asset(rtg, False)

    sources = []
    for path in unreal.EditorAssetLibrary.list_assets(SOURCE_ANIMS_DIR, recursive=False):
        asset = unreal.load_asset(path)
        if isinstance(asset, (unreal.AnimSequence, unreal.BlendSpace)) and not path.split(".")[0].endswith(SUFFIX):
            sources.append(unreal.EditorAssetLibrary.find_asset_data(path))
    log("retargeting", len(sources), "assets")
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
