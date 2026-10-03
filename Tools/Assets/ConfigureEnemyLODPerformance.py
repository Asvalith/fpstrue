import json
import os

import unreal


LOD_SETTINGS_PATH = "/Game/FPS/Demo/Characters/Mannequins/Meshes/Mannequin_LODSettingsnew"
ENEMY_MESH_PATH = "/Game/EnemyWarriorAnimPack/SkeletalMesh/SK_Mannequin_UE4_WithWeapon"

TRIANGLE_PERCENTAGES = [1.0, 0.25, 0.125, 0.06]
MAX_BONE_INFLUENCES = [8, 4, 4, 4]
LOD_HYSTERESIS = [0.02, 0.02, 0.02, 0.02]


settings = unreal.load_asset(LOD_SETTINGS_PATH)
mesh = unreal.load_asset(ENEMY_MESH_PATH)
if not settings or not mesh:
    raise RuntimeError("Required enemy LOD assets could not be loaded")

groups = list(settings.get_editor_property("lod_groups"))
if len(groups) != 4:
    raise RuntimeError("Expected 4 LOD groups, found {}".format(len(groups)))

for index, group in enumerate(groups):
    reduction = group.get_editor_property("reduction_settings")
    reduction.set_editor_property(
        "num_of_triangles_percentage", TRIANGLE_PERCENTAGES[index]
    )
    reduction.set_editor_property("max_bones_per_vertex", MAX_BONE_INFLUENCES[index])
    reduction.set_editor_property("base_lod", 0)
    group.set_editor_property("reduction_settings", reduction)
    group.set_editor_property("lod_hysteresis", LOD_HYSTERESIS[index])
    groups[index] = group

settings.set_editor_property("lod_groups", groups)
if not unreal.EditorAssetLibrary.save_loaded_asset(settings, only_if_is_dirty=False):
    raise RuntimeError("Failed to save enemy LOD settings")

subsystem = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
if not subsystem.regenerate_lod(
    mesh,
    new_lod_count=4,
    regenerate_even_if_imported=False,
    generate_base_lod=False,
):
    raise RuntimeError("Failed to regenerate enemy skeletal mesh LODs")

if not unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False):
    raise RuntimeError("Failed to save regenerated enemy skeletal mesh")

lod_info = mesh.get_editor_property("lod_info")
report = {
    "lod_settings": LOD_SETTINGS_PATH,
    "enemy_mesh": ENEMY_MESH_PATH,
    "lod_count": len(lod_info),
    "triangle_percentages": [],
    "max_bone_influences": [],
    "bone_filter_counts": [],
}

for index, info in enumerate(lod_info):
    reduction = info.get_editor_property("reduction_settings")
    report["triangle_percentages"].append(
        reduction.get_editor_property("num_of_triangles_percentage")
    )
    report["max_bone_influences"].append(
        reduction.get_editor_property("max_bones_per_vertex")
    )
    report["bone_filter_counts"].append(
        len(groups[index].get_editor_property("bone_list"))
    )

output_dir = os.path.join(unreal.Paths.project_saved_dir(), "Profiling")
os.makedirs(output_dir, exist_ok=True)
output_path = os.path.join(output_dir, "EnemyLODPerformanceConfiguration.json")
with open(output_path, "w", encoding="utf-8") as output_file:
    json.dump(report, output_file, ensure_ascii=False, indent=2)

unreal.log("Enemy LOD performance configuration written to {}".format(output_path))
