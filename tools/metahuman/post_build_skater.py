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

Head base colour: without texture synthesis (optional content) the baked head base colour is flat
grey and the face and scalp render white. fix_head_basecolor repaints it in the neck skin tone.

Hair: no binding is made. City Sample's Hair_S_Messy is authored on the crowd head m_002, and a
transfer binding to the skater face put the hair behind the skull (the two heads differ in vertex
order and position). ABoardActor attaches that groom rigidly to the head bone instead.
HAIR_GROOMS stays for grooms that do bind (for example the Creator's own, after the install).
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
FACE_DIR = "/Game/MetaHumans/Skater/Face"
HEAD_BC = FACE_DIR + "/Baked/T_Head_BC_VT"
HEAD_BC_SKIN = FACE_DIR + "/Baked/T_Head_BC_Skin_VT"
VENV_PYTHON = "/Users/mike/projects/overboard/.venv/bin/python"  # numpy + Pillow
GROOM_DIR = "/Game/MetaHumans/Skater/Grooms"
GROOM_SOURCE_HEAD = "/Game/Crowd/Character/Male/m_002/Face/m_002_nrw_FaceMesh"
HAIR_GROOMS = []

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


def synthesis_ran():
    plugin = unreal.Paths.convert_relative_path_to_full(unreal.Paths.engine_plugins_dir())
    return os.path.isdir(os.path.join(plugin, "MetaHuman/MetaHumanCharacter/Content/Optional/TextureSynthesis"))


def fix_head_basecolor():
    """Without texture synthesis the baked head base colour is flat grey: the face and scalp render
    white. Repaint it in the neck skin tone (repaint_head_basecolor.py) and use it on every head
    skin material. Skipped when the optional content (texture synthesis) is installed."""
    if synthesis_ran():
        log("head base colour: texture synthesis is installed, kept as built")
        return
    import subprocess
    src_tex = unreal.load_asset(HEAD_BC)
    if not src_tex:
        log("MISSING", HEAD_BC)
        return
    raw, painted = "/tmp/ob-render/mh_head_bc_raw.png", "/tmp/ob-render/T_Head_BC_Skin_VT.png"
    task = unreal.AssetExportTask()
    task.object, task.filename, task.automated, task.replace_identical = src_tex, raw, True, True
    task.exporter = unreal.TextureExporterPNG()
    if not unreal.Exporter.run_asset_export_task(task):
        log("FAIL export", HEAD_BC)
        return
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "repaint_head_basecolor.py")
    res = subprocess.run([VENV_PYTHON, tool, raw, painted, "4096"], capture_output=True, text=True)
    log("repaint:", res.stdout.strip(), res.stderr.strip()[-400:])
    if res.returncode != 0:
        return
    fresh(HEAD_BC_SKIN)
    imp = unreal.AssetImportTask()
    imp.filename, imp.destination_path, imp.destination_name = painted, FACE_DIR + "/Baked", HEAD_BC_SKIN.split("/")[-1]
    imp.automated, imp.replace_existing, imp.save = True, True, False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([imp])
    tex = unreal.load_asset(HEAD_BC_SKIN)
    tex.set_editor_property("srgb", True)
    tex.set_editor_property("virtual_texture_streaming", True)  # the baked skin material samples a VT
    unreal.EditorAssetLibrary.save_loaded_asset(tex, False)
    mel = unreal.MaterialEditingLibrary
    for path in unreal.EditorAssetLibrary.list_assets(FACE_DIR + "/Materials", recursive=False):
        mi = unreal.load_asset(path)
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            continue
        for tp in mi.get_editor_property("texture_parameter_values"):
            if tp.parameter_value and tp.parameter_value.get_path_name().split(".")[0] == HEAD_BC:
                name = str(tp.parameter_info.name)
                mel.set_material_instance_texture_parameter_value(mi, name, tex)
                unreal.EditorAssetLibrary.save_loaded_asset(mi, False)
                log("head base colour:", path.split(".")[0].split("/")[-1], name, "->", HEAD_BC_SKIN)


def main():
    fix_head_basecolor()
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
