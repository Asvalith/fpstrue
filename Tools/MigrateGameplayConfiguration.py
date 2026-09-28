"""Move the current Blueprint tuning into two reusable Data Assets.

Run in UnrealEditor-Cmd with PythonScriptPlugin and EditorScriptingUtilities.
Back up the two Blueprint packages before the first run. Existing configured
assets are preserved; an unbound existing asset is never silently overwritten.
"""

import unreal


ROOT = "/Game/Config"
GAME_MODE = "/Game/FirstPerson/Blueprints/gamemode/fpstruegamemode"
ENEMY = "/Game/FirstPerson/Blueprints/enemy/enemy_BP"
COMBAT_FIELDS = (
    "attack_range", "attack_damage", "attack_interval", "attack_animation_duration",
    "attack_fail_safe_duration", "attack_completion_grace_period",
    "weapon_trace_start_socket_name", "weapon_trace_end_socket_name",
    "weapon_trace_radius", "weapon_trace_sample_count",
)


def save(asset):
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError("Could not save " + asset.get_path_name())


def native_class(name):
    result = unreal.load_class(None, "/Script/fpstrue." + name)
    if result is None:
        raise RuntimeError("Build the current C++ module first: " + name)
    return result


def migrate(owner, property_name, name, config_class, values):
    path = ROOT + "/" + name
    configured = owner.get_editor_property(property_name)
    if configured is not None:
        if configured.get_path_name().split(".")[0] != path:
            raise RuntimeError("A different designer configuration is already assigned: " + property_name)
        unreal.log("Preserving existing configuration " + path)
        return configured
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        raise RuntimeError("Existing unbound configuration requires inspection: " + path)
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", config_class)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, ROOT, None, factory)
    if asset is None:
        raise RuntimeError("Could not create " + path)
    for key, value in values.items():
        asset.set_editor_property(key, value)
    # Verify before changing a Blueprint reference; migration must not retune it.
    for key, value in values.items():
        if asset.get_editor_property(key) != value:
            raise RuntimeError("Configuration copy did not preserve " + key)
    save(asset)
    owner.set_editor_property(property_name, asset)
    return asset


game_mode = unreal.load_asset(GAME_MODE)
enemy = unreal.load_asset(ENEMY)
if not isinstance(game_mode, unreal.Blueprint) or not isinstance(enemy, unreal.Blueprint):
    raise RuntimeError("Project GameMode and enemy Blueprints are required.")
gm_defaults = unreal.get_default_object(game_mode.generated_class())
enemy_defaults = unreal.get_default_object(enemy.generated_class())
combat = enemy_defaults.get_component_by_class(native_class("fpstrueEnemyCombatComponent"))
if combat is None:
    raise RuntimeError("Enemy has no native CombatComponent.")

# Read and validate every source before creating either asset.
gm_defaults.get_editor_property("wave_configuration")
combat.get_editor_property("combat_configuration")
combat_values = {key: combat.get_editor_property(key) for key in COMBAT_FIELDS}
waves = list(gm_defaults.get_editor_property("wave_configs"))
if not waves:
    count = gm_defaults.get_editor_property("total_waves")
    base = gm_defaults.get_editor_property("base_enemies_per_wave")
    added = gm_defaults.get_editor_property("enemies_added_per_wave")
    for index in range(count):
        wave = unreal.fpstrueWaveConfig()
        wave.set_editor_property("enemy_count", base + index * added)
        waves.append(wave)
default_enemy = gm_defaults.get_editor_property("enemy_class")
if not waves or any(wave.get_editor_property("enemy_count") < 1 or
                    (wave.get_editor_property("enemy_class") is None and default_enemy is None)
                    for wave in waves):
    raise RuntimeError("Legacy wave configuration is invalid; refusing a silent retune.")
wave_values = {
    "default_enemy_class": default_enemy,
    "waves": waves,
    "wave_interval": gm_defaults.get_editor_property("wave_interval"),
    "game_duration": gm_defaults.get_editor_property("game_duration"),
}

wave_asset = migrate(gm_defaults, "wave_configuration", "DA_FPMatchWaves",
                     native_class("fpstrueWaveConfiguration"), wave_values)
unreal.BlueprintEditorLibrary.compile_blueprint(game_mode)
save(game_mode)
combat_asset = migrate(combat, "combat_configuration", "DA_FPEnemyCombat",
                       native_class("fpstrueEnemyCombatConfig"), combat_values)
unreal.BlueprintEditorLibrary.compile_blueprint(enemy)
save(enemy)

# Compilation may reinstate CDOs: verify the saved reference targets again.
gm_defaults = unreal.get_default_object(game_mode.generated_class())
enemy_defaults = unreal.get_default_object(enemy.generated_class())
combat = enemy_defaults.get_component_by_class(native_class("fpstrueEnemyCombatComponent"))
if gm_defaults.get_editor_property("wave_configuration") != wave_asset or \
        combat.get_editor_property("combat_configuration") != combat_asset:
    raise RuntimeError("Blueprint recompilation did not preserve configuration bindings.")
unreal.log("FP_GAMEPLAY_CONFIG_READY waves={} duration={} combat={}".format(
    [wave.get_editor_property("enemy_count") for wave in wave_asset.get_editor_property("waves")],
    wave_asset.get_editor_property("game_duration"), combat_asset.get_path_name()))
