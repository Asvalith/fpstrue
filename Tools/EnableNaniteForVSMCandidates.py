import csv
from pathlib import Path

import unreal


ASSET_PATHS = [
    "/Game/FactoryDistrict/Meshes/RollupDoor_mdl",
    "/Game/FactoryDistrict/Meshes/SemiTrailer_mdl",
    "/Game/FactoryDistrict/Meshes/CargoContainer_mdl",
    "/Game/FactoryDistrict/Meshes/Building_TypeA_C",
    "/Game/FactoryDistrict/Meshes/Pipe_B_Mounted",
    "/Game/FactoryDistrict/Meshes/Shelfing_Large",
    "/Game/FactoryDistrict/Meshes/Balcony_Rail",
    "/Game/FactoryDistrict/Meshes/Intersection_T_4x4_lane",
    "/Game/FactoryDistrict/Meshes/Road_4L_VarB",
]


subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
rows = []

for asset_path in ASSET_PATHS:
    mesh = unreal.load_asset(asset_path)
    if not isinstance(mesh, unreal.StaticMesh):
        rows.append([asset_path, "Missing", "False"])
        continue

    settings = subsystem.get_nanite_settings(mesh)
    settings.set_editor_property("enabled", True)
    subsystem.set_nanite_settings(mesh, settings, True)
    saved = unreal.EditorAssetLibrary.save_loaded_asset(mesh, False)
    enabled = subsystem.get_nanite_settings(mesh).get_editor_property("enabled")
    rows.append([asset_path, str(enabled), str(saved)])

output_path = Path(unreal.Paths.project_saved_dir()) / "Profiling" / "VSM_NaniteApply.csv"
output_path.parent.mkdir(parents=True, exist_ok=True)
with output_path.open("w", newline="", encoding="utf-8-sig") as output_file:
    writer = csv.writer(output_file)
    writer.writerow(["Asset", "NaniteEnabled", "Saved"])
    writer.writerows(rows)

unreal.log(f"VSM Nanite apply report written to {output_path}")
