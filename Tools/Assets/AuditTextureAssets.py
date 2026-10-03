import csv
import json
import os

import unreal


ASSET_ROOTS = (
    "/Game/FactoryDistrict/Textures",
    "/Game/EnemyWarriorAnimPack/Textures",
    "/Game/EnemyWarriorAnimPack/Materials/MaterialLayers",
)


def property_text(asset, name):
    try:
        return str(asset.get_editor_property(name))
    except Exception:
        return "<unavailable>"


def texture_size(texture):
    try:
        return texture.blueprint_get_size_x(), texture.blueprint_get_size_y()
    except Exception:
        return 0, 0


def package_size(asset_path):
    package_name = asset_path.split(".", 1)[0]
    relative_path = package_name.removeprefix("/Game/") + ".uasset"
    filename = os.path.join(unreal.Paths.project_content_dir(), relative_path)
    try:
        return os.path.getsize(filename)
    except OSError:
        return 0


def audit_texture(asset_path):
    texture = unreal.load_asset(asset_path)
    if not isinstance(texture, unreal.Texture2D):
        return None

    width, height = texture_size(texture)
    max_texture_size = texture.get_editor_property("max_texture_size")
    max_dimension = max(width, height)
    needs_review = max_dimension > 2048 and max_texture_size == 0

    return {
        "asset": texture.get_path_name(),
        "width": width,
        "height": height,
        "max_texture_size": max_texture_size,
        "lod_group": property_text(texture, "lod_group"),
        "compression": property_text(texture, "compression_settings"),
        "mip_gen": property_text(texture, "mip_gen_settings"),
        "never_stream": property_text(texture, "never_stream"),
        "virtual_texture_streaming": property_text(
            texture, "virtual_texture_streaming"
        ),
        "srgb": property_text(texture, "srgb"),
        "package_bytes": package_size(texture.get_path_name()),
        "review_uncapped_over_2k": needs_review,
    }


rows = []
for root in ASSET_ROOTS:
    for asset_path in unreal.EditorAssetLibrary.list_assets(
        root, recursive=True, include_folder=False
    ):
        row = audit_texture(asset_path)
        if row:
            rows.append(row)

rows.sort(key=lambda item: (item["package_bytes"], item["width"] * item["height"]), reverse=True)

output_dir = os.path.join(unreal.Paths.project_saved_dir(), "Profiling")
os.makedirs(output_dir, exist_ok=True)
json_path = os.path.join(output_dir, "TextureAssetAudit.json")
csv_path = os.path.join(output_dir, "TextureAssetAudit.csv")

with open(json_path, "w", encoding="utf-8") as output_file:
    json.dump(rows, output_file, ensure_ascii=False, indent=2)

with open(csv_path, "w", newline="", encoding="utf-8-sig") as output_file:
    writer = csv.DictWriter(output_file, fieldnames=rows[0].keys() if rows else [])
    if rows:
        writer.writeheader()
        writer.writerows(rows)

uncapped_count = sum(1 for row in rows if row["review_uncapped_over_2k"])
unstreamed_count = sum(1 for row in rows if row["never_stream"] == "True")
unreal.log(
    "Texture asset audit written: textures={} uncapped_over_2k={} never_stream={}".format(
        len(rows), uncapped_count, unstreamed_count
    )
)
unreal.log("Texture asset audit JSON: {}".format(json_path))
unreal.log("Texture asset audit CSV: {}".format(csv_path))
