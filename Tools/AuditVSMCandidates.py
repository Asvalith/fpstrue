import csv
from pathlib import Path

import unreal


ASSET_PATHS = [
    "/Game/FactoryDistrict/Meshes/RollupDoor_mdl",
    "/Game/FactoryDistrict/Meshes/SemiTrailer_mdl",
    "/Game/FactoryDistrict/Meshes/CargoContainer_mdl",
    "/Game/FactoryDistrict/Meshes/Balcony_Rail",
    "/Game/FactoryDistrict/Meshes/Building_TypeA_C",
    "/Game/FactoryDistrict/Meshes/Pipe_B_Mounted",
    "/Game/FactoryDistrict/Meshes/Shelfing_Large",
    "/Game/FactoryDistrict/Meshes/Intersection_T_4x4_lane",
    "/Game/FactoryDistrict/Meshes/Road_4L_VarB",
]


def resolve_base_material(material):
    current = material
    visited = set()
    while isinstance(current, unreal.MaterialInstance):
        path = current.get_path_name()
        if path in visited:
            break
        visited.add(path)
        parent = current.get_editor_property("parent")
        if parent is None:
            break
        current = parent
    return current


def material_description(material):
    if material is None:
        return "None", "None"
    base_material = resolve_base_material(material)
    blend_mode = "Unknown"
    if isinstance(base_material, unreal.Material):
        blend_mode = str(base_material.get_editor_property("blend_mode"))
    return material.get_path_name(), blend_mode


subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
rows = []

for asset_path in ASSET_PATHS:
    mesh = unreal.load_asset(asset_path)
    if not isinstance(mesh, unreal.StaticMesh):
        rows.append([asset_path, "Missing", "", "", ""])
        continue

    nanite_settings = subsystem.get_nanite_settings(mesh)
    material_entries = []
    blend_modes = set()
    for static_material in mesh.get_editor_property("static_materials"):
        material = static_material.get_editor_property("material_interface")
        material_path, blend_mode = material_description(material)
        material_entries.append(material_path)
        blend_modes.add(blend_mode)

    rows.append([
        asset_path,
        str(nanite_settings.get_editor_property("enabled")),
        str(subsystem.get_lod_count(mesh)),
        "|".join(sorted(blend_modes)),
        "|".join(material_entries),
    ])

output_path = Path(unreal.Paths.project_saved_dir()) / "Profiling" / "VSM_CandidateAudit.csv"
output_path.parent.mkdir(parents=True, exist_ok=True)
with output_path.open("w", newline="", encoding="utf-8-sig") as output_file:
    writer = csv.writer(output_file)
    writer.writerow([
        "Asset",
        "NaniteEnabled",
        "LODCount",
        "BlendModes",
        "Materials",
    ])
    writer.writerows(rows)

unreal.log(f"VSM candidate audit written to {output_path}")
