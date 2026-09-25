"""Create the editable enemy BT/Blackboard and bind a Blueprint controller.

Run through UnrealEditor-Cmd with PythonScriptPlugin and EditorScriptingUtilities.
Existing populated trees are never rebuilt; rerunning preserves designer edits.
"""

import unreal


ROOT = "/Game/AI"
ENEMY_PATH = "/Game/FirstPerson/Blueprints/enemy/enemy_BP"
TREE_PATH = ROOT + "/BT_FPEnemy"
BLACKBOARD_PATH = ROOT + "/BB_FPEnemy"
CONTROLLER_PATH = ROOT + "/BP_FPEnemyAIController"


def create_or_load(name, asset_class, factory):
    path = ROOT + "/" + name
    asset = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if asset is None:
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, ROOT, asset_class, factory
        )
    if not isinstance(asset, asset_class):
        raise RuntimeError("Unexpected or missing asset: " + path)
    return asset


tree_existed = unreal.EditorAssetLibrary.does_asset_exist(TREE_PATH)
blackboard_existed = unreal.EditorAssetLibrary.does_asset_exist(BLACKBOARD_PATH)
if tree_existed != blackboard_existed:
    raise RuntimeError("Partial BT/Blackboard assets already exist; inspect them before retrying.")

tree = create_or_load("BT_FPEnemy", unreal.BehaviorTree, unreal.BehaviorTreeFactory())
blackboard = create_or_load("BB_FPEnemy", unreal.BlackboardData, unreal.BlackboardDataFactory())
if not tree_existed:
    library_class = unreal.load_class(None, "/Script/fpstrue.fpstrueEnemyBehaviorTreeLibrary")
    library = unreal.get_default_object(library_class)
    if not library.populate_default_tree(tree, blackboard):
        raise RuntimeError("Native initializer rejected the empty assets.")
    for asset in (blackboard, tree):
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
            raise RuntimeError("Could not save " + asset.get_path_name())

enemy_class = unreal.EditorAssetLibrary.load_blueprint_class(ENEMY_PATH)
if enemy_class is None:
    raise RuntimeError("Missing enemy Blueprint; tree assets are available but not bound: " + ENEMY_PATH)
enemy_defaults = unreal.get_default_object(enemy_class)
previous_controller = enemy_defaults.get_editor_property("ai_controller_class")
controller = unreal.load_asset(CONTROLLER_PATH) if unreal.EditorAssetLibrary.does_asset_exist(CONTROLLER_PATH) else None
if controller is None:
    # Preserve any previous controller subclass rather than discarding its defaults.
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", previous_controller)
    controller = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "BP_FPEnemyAIController", ROOT, unreal.Blueprint, factory
    )
if controller is None:
    raise RuntimeError("Could not create the Blueprint controller.")
controller_class = controller.generated_class()
controller_defaults = unreal.get_default_object(controller_class)
configured_tree = controller_defaults.get_editor_property("behavior_tree_asset")
if configured_tree is not None and configured_tree != tree:
    raise RuntimeError("Controller has a different designer tree; refusing to overwrite it.")
controller_defaults.set_editor_property("behavior_tree_asset", tree)
unreal.BlueprintEditorLibrary.compile_blueprint(controller)
if not unreal.EditorAssetLibrary.save_loaded_asset(controller, only_if_is_dirty=False):
    raise RuntimeError("Could not save controller Blueprint.")

enemy_defaults.set_editor_property("ai_controller_class", controller.generated_class())
enemy = unreal.load_asset(ENEMY_PATH)
unreal.BlueprintEditorLibrary.compile_blueprint(enemy)
if not unreal.EditorAssetLibrary.save_loaded_asset(enemy, only_if_is_dirty=False):
    raise RuntimeError("Could not save enemy Blueprint binding.")
unreal.log("FP_ENEMY_BT_READY tree={} blackboard={} controller={} enemy={}".format(
    TREE_PATH, BLACKBOARD_PATH, CONTROLLER_PATH, ENEMY_PATH))
