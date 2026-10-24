"""Read migration references and the four maps/two HUDs. Never edits or saves assets.

Registry results and HUD parent tags describe saved assets, not runtime reachability.
HUD graphs and compilation are audited by the existing fp.AuditGameplayContracts
command; this script does not read protected Blueprint fields or compile assets.
"""

import json

import unreal


RETIREMENT_CANDIDATES = (
    "/Game/FirstPerson/Blueprints/gamemode/fpstruegamemode",
    "/Game/FirstPerson/Blueprints/BP_FirstPersonGameMode",
    "/Game/Config/DA_FPMatchWaves",
)
LEGACY_UI_PACKAGES = (
    "/Game/FirstPerson/UI/mainmenu",
    "/Game/FirstPerson/UI/mainmenushake",
    "/Game/FirstPerson/UI/winorfail",
)
MAP_PACKAGES = (
    "/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked",
    "/Game/PerformanceCandidates/Nanite_HardSurface20260929/Demonstration_Nanite",
    "/Game/FactoryDistrict/Maps/Demonstration",
    "/Game/FirstPerson/Maps/FirstPersonMap",
)
HUD_PACKAGES = (
    "/Game/FirstPerson/UI/ingame_network",
    "/Game/FirstPerson/UI/ingame",
)
PVP_MODE = "/Script/fpstrue.fpstruePvpGameMode"
PVP_HUD = "/Script/fpstrue.fpstruePvpIngameWidget"


def report(kind, **values):
    unreal.log("FP_MP_MIGRATION_{} {}".format(
        kind, json.dumps(values, ensure_ascii=False, sort_keys=True)))


def path(value):
    return value.get_path_name() if value is not None else "None"


registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.search_all_assets(synchronous_search=True)
hard_options = unreal.AssetRegistryDependencyOptions(
    include_hard_package_references=True, include_soft_package_references=False,
    include_searchable_names=False, include_soft_management_references=False,
    include_hard_management_references=False)
soft_options = unreal.AssetRegistryDependencyOptions(
    include_hard_package_references=False, include_soft_package_references=True,
    include_searchable_names=False, include_soft_management_references=False,
    include_hard_management_references=False)

report("AUDIT_BEGIN", read_only=True, source="SerializedRegistryAndLoadedObjects",
       graph_and_compile_audit="fp.AuditGameplayContracts",
       note="No compile, map switch, migration, save or deletion. Zero references is not deletion authorization.")
# Query metadata before loading maps. Never load retired GameMode blueprints or
# HUDs. ingame/mainmenu/pause may serve other content and are not deletion targets.
ui_packages = {str(asset.package_name) for asset in
               registry.get_assets_by_path("/Game/FirstPerson/UI", recursive=True)}
for package in sorted(set(RETIREMENT_CANDIDATES + LEGACY_UI_PACKAGES + HUD_PACKAGES) | ui_packages):
    report("REFERENCERS", package=package,
           present=bool(registry.get_assets_by_package_name(package)),
           retirement_candidate=package in RETIREMENT_CANDIDATES,
           hard=sorted(str(value) for value in (registry.get_referencers(package, hard_options) or [])),
           soft=sorted(str(value) for value in (registry.get_referencers(package, soft_options) or [])))

failures = 0
for package in HUD_PACKAGES:
    try:
        assets = registry.get_assets_by_package_name(package, include_only_on_disk_assets=True)
        if not assets:
            raise RuntimeError("HUD has no saved asset registry record")
        for asset in assets:
            parent = asset.get_tag_value("ParentClass")
            native_parent = asset.get_tag_value("NativeParentClass")
            report("HUD_SAVED_TAGS", package=package, asset=str(asset.asset_name),
                   parent_class=parent, native_parent_class=native_parent,
                   expected_parent=PVP_HUD,
                   graph_and_compile_audit="fp.AuditGameplayContracts")
            if parent is None or native_parent is None:
                raise RuntimeError("Saved HUD is missing ParentClass/NativeParentClass tags")
            if PVP_HUD not in str(parent) or PVP_HUD not in str(native_parent):
                raise RuntimeError("Saved HUD parent does not match the multiplayer event bridge")
    except Exception as error:
        failures += 1
        report("READ_FAILED", package=package, error=str(error))

for package in MAP_PACKAGES:
    try:
        world = unreal.load_asset(package)
        if world is None:
            raise RuntimeError("Map could not be loaded")
        settings = world.get_world_settings()
        if settings is None:
            raise RuntimeError("Loaded map has no WorldSettings")
        mode = path(settings.get_editor_property("DefaultGameMode"))
        report("MAP", package=package, world_settings=path(settings),
               default_game_mode=mode, expected_game_mode=PVP_MODE,
               mode_matches=mode == PVP_MODE)
        if mode != PVP_MODE:
            raise RuntimeError("Saved map does not use the multiplayer GameMode")
    except Exception as error:
        failures += 1
        report("READ_FAILED", package=package, error=str(error))

report("AUDIT_COMPLETE", read_failures=failures, no_assets_saved=True,
       no_assets_deleted=True, static_semantics_verified=False)
if failures:
    raise RuntimeError("Multiplayer migration audit failed: {} reads/contracts".format(failures))
