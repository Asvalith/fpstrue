"""Copy enemy combat tuning into its Data Asset; multiplayer rules use GameMode config.

Run with UnrealEditor-Cmd, PythonScriptPlugin and EditorScriptingUtilities.
This tool intentionally never recreates the retired single-player wave configuration.
"""

import unreal


enemy = unreal.load_asset("/Game/FirstPerson/Blueprints/enemy/enemy_BP")
defaults = unreal.get_default_object(enemy.generated_class())
combat_class = unreal.load_class(None, "/Script/fpstrue.fpstrueEnemyCombatComponent")
combat = defaults.get_component_by_class(combat_class)
if combat.get_editor_property("combat_configuration") is not None:
    unreal.log("FP_GAMEPLAY_CONFIG_READY PreservingAssignedEnemyCombat=true")
else:
    path = "/Game/Config/DA_FPEnemyCombat"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        raise RuntimeError("Existing unbound combat asset requires inspection: " + path)
    fields = (
        "attack_range", "attack_damage", "attack_interval", "attack_animation_duration",
        "attack_fail_safe_duration", "attack_completion_grace_period",
        "weapon_trace_start_socket_name", "weapon_trace_end_socket_name",
        "weapon_trace_radius", "weapon_trace_sample_count",
    )
    values = {field: combat.get_editor_property(field) for field in fields}
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.load_class(None, "/Script/fpstrue.fpstrueEnemyCombatConfig"))
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset("DA_FPEnemyCombat", "/Game/Config", None, factory)
    for field, value in values.items():
        asset.set_editor_property(field, value)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError("Cannot save enemy combat configuration")
    combat.set_editor_property("combat_configuration", asset)
    unreal.BlueprintEditorLibrary.compile_blueprint(enemy)
    if not unreal.EditorAssetLibrary.save_loaded_asset(enemy, only_if_is_dirty=False):
        raise RuntimeError("Cannot save enemy Blueprint configuration binding")
    unreal.log("FP_GAMEPLAY_CONFIG_READY Combat=" + asset.get_path_name())
