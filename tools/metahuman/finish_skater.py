"""Finish MHC_Skater: the cloud steps and the build. Run it in the editor, signed in to Epic.

In the editor Output Log, set the input box to "Cmd" and paste:
  py "/Users/mike/projects/overboard-game-render/tools/metahuman/finish_skater.py"

It does, in order:
  1. Face auto-rig (Epic cloud, joints and blend shapes).
  2. High-resolution texture sources (Epic cloud).
  3. BuildMetaHuman, Cinematic pipeline, to /Game/MetaHumans/Skater/ (Blueprint BP_Skater).
The editor freezes while each cloud request runs (blocking requests). Log:
/tmp/ob-render/mh_finish.txt. Run create_skater.py first; it makes the asset.
"""
import os
import unreal

LOG_PATH = "/tmp/ob-render/mh_finish.txt"
CHARACTER = "/Game/MetaHumans/Skater/MHC_Skater.MHC_Skater"
# Material baking runs Texture Graphs, which crash in a commandlet (SIGSEGV in
# UTG_AsyncExportTask, 2026-10-04). Run this in the full editor: the Output Log, or headless with
#   UnrealEditor <uproject> -ExecutePythonScript=<this file> -OBQuitWhenDone
QUIT_WHEN_DONE = "-obquitwhendone" in unreal.SystemLibrary.get_command_line().lower()

os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
_log = open(LOG_PATH, "w")


def log(*parts):
    line = " ".join(str(p) for p in parts)
    _log.write(line + "\n")
    _log.flush()
    unreal.log("finish_skater: " + line)


def main():
    character = unreal.load_asset(CHARACTER)
    if character is None:
        log("FAIL: no character at", CHARACTER, "- run create_skater.py first")
        return
    mhs = unreal.get_editor_subsystem(unreal.MetaHumanCharacterEditorSubsystem)
    if not mhs.try_add_object_to_edit(character):
        log("FAIL: try_add_object_to_edit (close the MetaHuman Character editor tab for MHC_Skater, then run again)")
        return
    try:
        rig = unreal.MetaHumanCharacterAutoRiggingRequestParams()
        rig.blocking = True
        rig.report_progress = False
        rig.rig_type = unreal.MetaHumanRigType.JOINTS_AND_BLEND_SHAPES
        save = lambda: unreal.EditorAssetLibrary.save_loaded_asset(character, False)
        rigged = mhs.can_build_meta_human(character, False) or character.get_editor_property("has_face_dna_blendshapes")
        if rigged:
            log("auto-rig: already rigged, skipped")
        else:
            log("auto-rig: requesting")
            mhs.request_auto_rigging(character, rig)
            log("auto-rig: done; saved", save())

        tex = unreal.MetaHumanCharacterTextureRequestParams()
        tex.blocking = True
        tex.report_progress = False
        if character.get_editor_property("has_high_resolution_textures"):
            log("textures: already present, skipped")
        else:
            log("textures: requesting")
            mhs.request_texture_sources(character, tex)
            log("textures: done; has_high_resolution_textures", character.get_editor_property("has_high_resolution_textures"), "saved", save())

        if not mhs.can_build_meta_human(character, True):
            log("FAIL: the character cannot be built yet (see the Output Log for the reason)")
            return
        params = unreal.MetaHumanCharacterEditorBuildParameters()
        params.pipeline_type = unreal.MetaHumanDefaultPipelineType.CINEMATIC
        params.absolute_build_path = "/Game/MetaHumans"
        params.common_folder_path = "/Game/MetaHumans/Common"
        params.name_override = "Skater"
        params.enable_wardrobe_item_validation = False
        log("build: cinematic to /Game/MetaHumans/Skater")
        mhs.build_meta_human(character, params)
        built = unreal.EditorAssetLibrary.list_assets("/Game/MetaHumans/Skater", recursive=False)
        log("build: assets in /Game/MetaHumans/Skater:", [a for a in built if "/BP_" in a or "/SKM_" in a])
    finally:
        if mhs.is_object_added_for_editing(character):
            mhs.remove_object_to_edit(character)
    log("saved character", unreal.EditorAssetLibrary.save_loaded_asset(character, False))
    unreal.EditorAssetLibrary.save_directory("/Game/MetaHumans", only_if_is_dirty=True, recursive=True)
    log("OK: done. Tell Claude the build finished; the render picks BP_Skater up from here.")


try:
    main()
except Exception:
    import traceback
    log("EXCEPTION", traceback.format_exc())
finally:
    _log.close()
    if QUIT_WHEN_DONE:
        unreal.SystemLibrary.quit_editor()
