# Import the split X7 skin into /Game/ThirdParty/X7 (editor; run by build_x7_skin.sh).
import unreal
out = open("/tmp/ob-board/import.txt", "w")
for name in ("x7_frame", "x7_motor"):
    t = unreal.AssetImportTask()
    t.filename = "/tmp/ob-board/%s.glb" % name
    t.destination_path = "/Game/ThirdParty/X7"
    t.destination_name = name
    t.automated = True
    t.replace_existing = True
    t.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([t])
    out.write("%s -> %s\n" % (name, list(t.imported_object_paths)))
ar = unreal.AssetRegistryHelpers.get_asset_registry()
for a in ar.get_assets_by_path("/Game/ThirdParty/X7", recursive=True):
    o = unreal.load_asset(str(a.package_name))
    line = "%s %s" % (a.asset_class_path.asset_name, a.package_name)
    if isinstance(o, unreal.StaticMesh):
        b = o.get_bounding_box()
        line += " bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f) slots %d" % (b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z, len(o.get_editor_property("static_materials")))
    out.write(line + "\n")
out.close()
