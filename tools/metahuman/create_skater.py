"""Create (or re-create) the MetaHuman Character asset /Game/MetaHumans/Skater/MHC_Skater.

Step 1 of build_skater.sh. Runs headless and offline:
  UnrealEditor-Cmd <abs uproject> -run=pythonscript -script=<abs this file> -stdout -unattended -nosplash
Log: /tmp/ob-render/mh_create.txt (print() from a commandlet does not reach the UE log).

All the look choices are the parameters below. Change them and run build_skater.sh again.
A missing preset, wardrobe item or groom is logged and skipped; the script does not substitute
another one. The log also lists every preset, wardrobe item and groom the Creator can see.
"""
import os
import unreal

# ---------------------------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------------------------
PKG = "/Game/MetaHumans/Skater"
NAME = "MHC_Skater"

# Face. Either a MetaHuman Character preset asset (it is duplicated as the starting point), or a
# head mesh with MetaHuman topology (fitted with ImportFromTemplate). Presets live in
# /MetaHumanCharacter/Optional/Presets once the optional content is installed; the log lists them.
FACE_PRESET = ""  # for example "/MetaHumanCharacter/Optional/Presets/<Name>"
FACE_TEMPLATE_MESH = "/Game/Crowd/Character/Male/m_002/Face/m_002_nrw_FaceMesh"  # City Sample crowd male

# Body. "parametric": the measurement constraints in BODY_TARGETS. "conform": take the shape of
# BODY_CONFORM_MESH. Conform rejects the City Sample crowd bodies (older MetaHuman skeleton, no
# pinky_03_bulge_l, other vertex layout), so the targets are set close to the crowd garment size
# (m_tal_nrw) and a little slimmer, so the body stays inside the City Sample clothes.
BODY_MODE = "parametric"
BODY_CONFORM_MESH = "/Game/Crowd/Character/Male/NormalWeight/Meshes/m_tal_nrw_body"
BODY_TARGETS = {  # cm, or slider units for the last three
    # Girths sit below the City Sample m_tal_nrw garments, or the skin shows through the clothes
    # (first still, 2026-10-04: arms, back and thighs poked through at the crowd-like defaults).
    "height": 180.0,
    "chest": 92.0,
    "waist": 76.0,
    "hip": 90.0,
    "neck": 36.0,
    "bicep": 27.0,
    "forearm": 24.0,
    "thigh": 50.0,
    "calf": 33.0,
    "masculine/feminine": -1.6,  # negative is masculine (measured: -2 gives 189 cm, +2 gives 154 cm)
    "muscularity": 0.0,
    "fat": -1.5,
}

# Creator wardrobe and grooms: (slot, wardrobe item asset). Slots: Outfits, Hair, Eyebrows,
# Eyelashes, Mustache, Beard, Peachfuzz. Empty now: the optional content is not installed, so the
# render dresses him with City Sample garments instead (see ABoardActor::SetupRenderRider).
WARDROBE = [
    # ("Outfits", "/MetaHumanCharacter/Optional/Clothing/<item>"),
]
GROOMS = [
    # ("Hair", "/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/WI_Hair_S_<item>"),
    # ("Eyebrows", "/MetaHumanCharacter/Optional/Grooms/Bindings/Eyebrows/WI_<item>"),
]
# ---------------------------------------------------------------------------------------------

LOG_PATH = "/tmp/ob-render/mh_create.txt"
OPTIONAL = "/MetaHumanCharacter/Optional"
os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
_log = open(LOG_PATH, "w")


def log(*parts):
    _log.write(" ".join(str(p) for p in parts) + "\n")
    _log.flush()


def inventory():
    """Log every preset, wardrobe item and groom the Creator can use."""
    for sub in ("Presets", "Clothing", "Grooms/Bindings/Hair", "Grooms/Bindings/Eyebrows",
                "Grooms/Bindings/Eyelashes", "Grooms/Bindings/Mustaches", "Grooms/Bindings/Beards",
                "Grooms/Bindings/Peachfuzz"):
        path = f"{OPTIONAL}/{sub}"
        items = unreal.EditorAssetLibrary.list_assets(path, recursive=False) if unreal.EditorAssetLibrary.does_directory_exist(path) else []
        log(f"inventory {sub}: {len(items)}", [i.split("/")[-1].split(".")[0] for i in items])


def make_character(tools):
    path = f"{PKG}/{NAME}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
        log("deleted old", path)
    if FACE_PRESET:
        if unreal.EditorAssetLibrary.does_asset_exist(FACE_PRESET):
            character = unreal.EditorAssetLibrary.duplicate_asset(FACE_PRESET, path)
            log("duplicated preset", FACE_PRESET)
            return character, True
        log("MISSING preset", FACE_PRESET, "- using a blank character")
    character = tools.create_asset(asset_name=NAME, package_path=PKG, asset_class=unreal.MetaHumanCharacter,
                                   factory=unreal.MetaHumanCharacterFactoryNew())
    return character, False


def set_face(mhs, character):
    face = unreal.load_asset(FACE_TEMPLATE_MESH) if FACE_TEMPLATE_MESH else None
    if not face:
        log("MISSING face template mesh", FACE_TEMPLATE_MESH, "- default face kept")
        return
    params = unreal.ImportFromTemplateParams()
    params.use_eye_meshes = False
    params.use_teeth_mesh = False
    # City Sample heads keep MetaHuman UVs but not the current vertex order, so match by UVs.
    params.match_vertices_by_u_vs = True
    log("face import_from_template", FACE_TEMPLATE_MESH, mhs.import_from_template(character, face, None, None, None, params))


def set_body(mhs, character):
    if BODY_MODE == "conform":
        mesh = unreal.load_asset(BODY_CONFORM_MESH)
        if not mesh:
            log("MISSING body conform mesh", BODY_CONFORM_MESH, "- default body kept")
            return
        result, vertices = mhs.get_mesh_for_body_conforming(character, mesh, None, False)
        log("body mesh for conforming", result, len(vertices))
        if result != unreal.ImportErrorCode.SUCCESS:
            result, vertices = mhs.get_mesh_for_body_conforming(character, mesh, None, True)
            log("body mesh for conforming (match by UVs)", result, len(vertices))
        result_j, _, rotations = mhs.get_joints_for_body_conforming(mesh)
        log("body joints for conforming", result_j)
        if result == unreal.ImportErrorCode.SUCCESS and result_j == unreal.ImportErrorCode.SUCCESS:
            log("conform_body", mhs.conform_body(character, vertices, rotations, True, False))
    else:
        by_name = {str(c.name).lower().replace(" ", "_"): c for c in mhs.get_body_constraints(character)}
        for key, value in BODY_TARGETS.items():
            c = by_name.get(key)
            if c is None:
                log("no body constraint named", key)
                continue
            c.is_active = True
            c.target_measurement = max(c.min_measurement, min(c.max_measurement, value))
        mhs.set_body_constraints(character, list(by_name.values()))
        mhs.commit_body_state(character)
    for c in mhs.get_body_constraints(character):
        if str(c.name).lower().replace(" ", "_") in BODY_TARGETS or str(c.name).lower() in ("across shoulder", "inseam"):
            log(f"  body {c.name}: {c.target_measurement:.1f}")


def add_items(character, items):
    collection = character.get_editor_property("internal_collection")
    for slot, item_path in items:
        item = unreal.load_asset(item_path) if unreal.EditorAssetLibrary.does_asset_exist(item_path) else None
        if item is None:
            log("MISSING wardrobe item", slot, item_path, "- skipped")
            continue
        key = collection.try_add_item_from_wardrobe_item(slot, item)
        selection = unreal.MetaHumanPipelineSlotSelection(slot_name=slot, selected_item=key)
        log("added", slot, item_path, collection.default_instance.try_add_slot_selection(selection))


def main():
    inventory()
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    character, from_preset = make_character(tools)
    log("character", character.get_path_name())
    add_items(character, WARDROBE + GROOMS)

    mhs = unreal.get_editor_subsystem(unreal.MetaHumanCharacterEditorSubsystem)
    if not mhs.try_add_object_to_edit(character):
        log("FAIL try_add_object_to_edit")
        return
    try:
        if not from_preset:
            set_face(mhs, character)
        set_body(mhs, character)
        # Skin is left at the default: commit_skin_settings asserts (BodyTexture) when the
        # texture-synthesis model from the optional content is missing.
        log("can_build", mhs.can_build_meta_human(character, False), "(false until finish_skater.py rigs it)")
    finally:
        if mhs.is_object_added_for_editing(character):
            mhs.remove_object_to_edit(character)
    log("saved", unreal.EditorAssetLibrary.save_loaded_asset(character, False))


try:
    main()
except Exception:
    import traceback
    log("EXCEPTION", traceback.format_exc())
finally:
    _log.close()
