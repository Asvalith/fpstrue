import json
import os

import unreal


ASSET_PATH = "/Game/FPS/Demo/Characters/Mannequins/Meshes/Mannequin_LODSettingsnew"

FINGER_BONES = [
    "thumb_01_l",
    "index_01_l",
    "middle_01_l",
    "ring_01_l",
    "pinky_01_l",
    "thumb_01_r",
    "index_01_r",
    "middle_01_r",
    "ring_01_r",
    "pinky_01_r",
]

TWIST_BONES = [
    "upperarm_twist_01_l",
    "lowerarm_twist_01_l",
    "upperarm_twist_01_r",
    "lowerarm_twist_01_r",
    "thigh_twist_01_l",
    "calf_twist_01_l",
    "thigh_twist_01_r",
    "calf_twist_01_r",
]


def make_filter(bone_name):
    value = unreal.BoneFilter()
    value.set_editor_property("bone_name", unreal.Name(bone_name))
    value.set_editor_property("exclude_self", False)
    return value


def names_from_group(group):
    return [
        str(item.get_editor_property("bone_name"))
        for item in group.get_editor_property("bone_list")
    ]


asset = unreal.load_asset(ASSET_PATH)
if not asset:
    raise RuntimeError("LOD settings asset was not found: {}".format(ASSET_PATH))

groups = list(asset.get_editor_property("lod_groups"))
if len(groups) != 4:
    raise RuntimeError("Expected 4 LOD groups, found {}".format(len(groups)))

before = [names_from_group(group) for group in groups]
target_bones = [[], [], FINGER_BONES, FINGER_BONES + TWIST_BONES]

for index, bone_names in enumerate(target_bones):
    group = groups[index]
    group.set_editor_property(
        "bone_filter_action_option", unreal.BoneFilterActionOption.REMOVE
    )
    group.set_editor_property(
        "bone_list", [make_filter(name) for name in bone_names]
    )
    groups[index] = group

asset.set_editor_property("lod_groups", groups)
saved = unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)
if not saved:
    raise RuntimeError("Failed to save {}".format(ASSET_PATH))

after = [names_from_group(group) for group in asset.get_editor_property("lod_groups")]
report = {
    "asset": ASSET_PATH,
    "before_counts": [len(names) for names in before],
    "after_counts": [len(names) for names in after],
    "after_bones": after,
}

output_dir = os.path.join(unreal.Paths.project_saved_dir(), "Profiling")
os.makedirs(output_dir, exist_ok=True)
output_path = os.path.join(output_dir, "EnemyLODBoneConfiguration.json")
with open(output_path, "w", encoding="utf-8") as output_file:
    json.dump(report, output_file, ensure_ascii=False, indent=2)

unreal.log("Enemy LOD bone configuration written to {}".format(output_path))
