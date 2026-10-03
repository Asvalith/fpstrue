"""Export with the old module, migrate with the rebuilt module, then verify in a fresh process.

UnrealEditor-Cmd <project> -run=pythonscript -script=<this file>
  -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities -WeaponConfigMode=Export|Migrate|Verify
  -unattended -NullRHI -NoSound -NoLiveCoding

Never replace an existing designer asset or re-export after removing the legacy fields.
The original Blueprint package and its actual values are kept under Saved/Backups.
"""

import hashlib
import json
import pathlib
import re
import shutil

import unreal

BLUEPRINT = "/Game/FirstPerson/Blueprints/weapon/BP_Weapon"
CONFIG = "/Game/Config/DA_FPWeapon_RU74"
FIELDS = (
    "grip_socket_name", "rounds_per_minute", "line_trace_range", "line_trace_impulse",
    "line_trace_damage", "line_trace_head_damage", "critical_hit_bones", "magazine_size",
    "starting_reserve_ammo", "hip_fire_spread_angle", "aim_fire_spread_angle",
    "continuous_fire_spread_step", "max_continuous_fire_spread_angle", "spread_reset_delay",
    "recoil_pitch", "recoil_yaw", "aim_recoil_multiplier", "recoil_recovery_delay",
    "recoil_recovery_speed", "max_accumulated_recoil_pitch", "max_accumulated_recoil_yaw",
    "reload_duration", "empty_reload_duration", "reload_fail_safe_duration",
    "reload_completion_grace_period",
)
PROJECT = pathlib.Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
BACKUP = PROJECT / "Saved/Backups/WeaponConfigurationMigration"
PACKAGE = PROJECT / "Content/FirstPerson/Blueprints/weapon/BP_Weapon.uasset"
MANIFEST = BACKUP / "original-values.json"


def weapon_template(blueprint):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    matches = []
    for handle in subsystem.k2_gather_subobject_data_for_blueprint(blueprint):
        data = library.get_data(handle)
        obj = library.get_object_for_blueprint(data, blueprint)
        if isinstance(obj, unreal.fpstrueWeaponComponent):
            matches.append(obj)
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one weapon template, got " + str(len(matches)))
    return matches[0]


def values_from(owner):
    values = {name: owner.get_editor_property(name) for name in FIELDS}
    values["grip_socket_name"] = str(values["grip_socket_name"])
    values["critical_hit_bones"] = [str(bone) for bone in values["critical_hit_bones"]]
    return values


def save(asset):
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError("Could not save " + asset.get_path_name())


match = re.search(r"-WeaponConfigMode=(Export|Migrate|Verify)\b", unreal.SystemLibrary.get_command_line())
if match is None:
    raise RuntimeError("Specify -WeaponConfigMode=Export, Migrate or Verify")
mode = match.group(1)
blueprint = unreal.load_asset(BLUEPRINT)
if not isinstance(blueprint, unreal.Blueprint):
    raise RuntimeError("Weapon Blueprint is required")
weapon = weapon_template(blueprint)

if mode == "Export":
    if MANIFEST.exists() or BACKUP.exists():
        raise RuntimeError("Original-value backup already exists; do not overwrite it")
    values = values_from(weapon)  # Fails closed when running against the new module.
    BACKUP.mkdir(parents=True)
    shutil.copy2(PACKAGE, BACKUP / PACKAGE.name)
    MANIFEST.write_text(json.dumps({
        "blueprint": BLUEPRINT, "component": weapon.get_name(),
        "source_sha256": hashlib.sha256(PACKAGE.read_bytes()).hexdigest(), "values": values,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log("FP_WEAPON_CONFIG_EXPORTED " + json.dumps(values, ensure_ascii=False))
else:
    original = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if original["blueprint"] != BLUEPRINT or original["component"] != weapon.get_name():
        raise RuntimeError("Blueprint/component does not match the export")
    values = original["values"]
    if set(values) != set(FIELDS):
        raise RuntimeError("Incomplete export")
    asset = unreal.load_asset(CONFIG) if unreal.EditorAssetLibrary.does_asset_exist(CONFIG) else None
    if mode == "Migrate":
        if asset is not None or weapon.get_editor_property("weapon_configuration") is not None:
            raise RuntimeError("Already configured; use Verify, not another migration")
        if hashlib.sha256(PACKAGE.read_bytes()).hexdigest() != original["source_sha256"]:
            raise RuntimeError("Blueprint changed since export; reconcile before migrating")
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.load_class(None, "/Script/fpstrue.fpstrueWeaponConfig"))
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset("DA_FPWeapon_RU74", "/Game/Config", None, factory)
        if asset is None:
            raise RuntimeError("Could not create weapon configuration")
        settings = asset.get_editor_property("settings")
        for name, value in values.items():
            settings.set_editor_property(name, value)
        asset.set_editor_property("settings", settings)
        if values_from(asset.get_editor_property("settings")) != values:
            raise RuntimeError("Configuration copy changed tuning")
        save(asset)
        weapon.set_editor_property("weapon_configuration", asset)
        unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
        save(blueprint)
        weapon = weapon_template(blueprint)
    if asset is None or weapon.get_editor_property("weapon_configuration") != asset:
        raise RuntimeError("Saved Blueprint binding is missing or incorrect")
    if values_from(asset.get_editor_property("settings")) != values:
        raise RuntimeError("Saved values differ from the original weapon")
    unreal.log("FP_WEAPON_CONFIG_VERIFIED fields={} asset={}".format(len(FIELDS), asset.get_path_name()))
