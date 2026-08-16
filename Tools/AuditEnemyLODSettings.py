import json
import os

import unreal


LOD_SETTINGS_PATH = "/Game/FPS/Demo/Characters/Mannequins/Meshes/Mannequin_LODSettingsnew"
ENEMY_MESH_PATH = "/Game/EnemyWarriorAnimPack/SkeletalMesh/SK_Mannequin_UE4_WithWeapon"


def value_text(owner, property_name):
    try:
        return str(owner.get_editor_property(property_name))
    except Exception as exc:
        return "<unavailable: {}>".format(exc)


def object_path(value):
    if value is None:
        return "None"
    try:
        return value.get_path_name()
    except Exception:
        return str(value)


def reduction_data(settings_value):
    properties = (
        "termination_criterion",
        "num_of_triangles_percentage",
        "num_of_vert_percentage",
        "max_num_of_triangles",
        "max_num_of_verts",
        "max_num_of_triangles_percentage",
        "max_num_of_verts_percentage",
        "max_deviation_percentage",
        "reduction_method",
        "silhouette_importance",
        "texture_importance",
        "shading_importance",
        "skinning_importance",
        "max_bones_per_vertex",
        "volume_importance",
        "base_lod",
    )
    return {name: value_text(settings_value, name) for name in properties}


def bone_filter_data(filters):
    return [
        {
            "bone_name": value_text(item, "bone_name"),
            "exclude_self": value_text(item, "exclude_self"),
        }
        for item in filters
    ]


settings = unreal.load_asset(LOD_SETTINGS_PATH)
mesh = unreal.load_asset(ENEMY_MESH_PATH)

report = {
    "lod_settings_asset": {
        "path": object_path(settings),
        "class": settings.get_class().get_name() if settings else "None",
        "groups": [],
    },
    "enemy_mesh": {
        "path": object_path(mesh),
        "class": mesh.get_class().get_name() if mesh else "None",
    },
}

if settings:
    groups = settings.get_editor_property("lod_groups")
    report["lod_settings_asset"]["group_count"] = len(groups)
    for index, group in enumerate(groups):
        reduction = group.get_editor_property("reduction_settings")
        bone_list = group.get_editor_property("bone_list")
        report["lod_settings_asset"]["groups"].append(
            {
                "index": index,
                "screen_size": value_text(group, "screen_size"),
                "lod_hysteresis": value_text(group, "lod_hysteresis"),
                "bone_filter_action_option": value_text(group, "bone_filter_action_option"),
                "bone_list": bone_filter_data(bone_list),
                "bones_to_prioritize": value_text(group, "bones_to_prioritize"),
                "sections_to_prioritize": value_text(group, "sections_to_prioritize"),
                "weight_of_prioritization": value_text(group, "weight_of_prioritization"),
                "bake_pose": value_text(group, "bake_pose"),
                "allow_mesh_deformer": value_text(group, "allow_mesh_deformer"),
                "reduction_settings": reduction_data(reduction),
            }
        )

if mesh:
    report["enemy_mesh"]["lod_settings"] = object_path(
        mesh.get_editor_property("lod_settings")
    )
    lod_info = mesh.get_editor_property("lod_info")
    report["enemy_mesh"]["lod_count"] = len(lod_info)
    report["enemy_mesh"]["lod_info"] = []
    for index, info in enumerate(lod_info):
        reduction = info.get_editor_property("reduction_settings")
        report["enemy_mesh"]["lod_info"].append(
            {
                "index": index,
                "screen_size": value_text(info, "screen_size"),
                "lod_hysteresis": value_text(info, "lod_hysteresis"),
                "reduction_settings": reduction_data(reduction),
            }
        )

output_dir = os.path.join(unreal.Paths.project_saved_dir(), "Profiling")
os.makedirs(output_dir, exist_ok=True)
output_path = os.path.join(output_dir, "EnemyLODSettingsAudit.json")
with open(output_path, "w", encoding="utf-8") as output_file:
    json.dump(report, output_file, ensure_ascii=False, indent=2)

unreal.log("LOD settings audit written to {}".format(output_path))
