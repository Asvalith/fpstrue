"""Check accessible bindings and save the graph opened by the full BT editor.

Protected BehaviorTree fields are verified by the C++ AssetGraphAndBindings
automation test, not by Python reflection. This script only reports operations
it actually completed.
"""

import unreal


try:
    tree = unreal.load_asset("/Game/AI/BT_FPEnemy")
    blackboard = unreal.load_asset("/Game/AI/BB_FPEnemy")
    controller_class = unreal.EditorAssetLibrary.load_blueprint_class("/Game/AI/BP_FPEnemyAIController")
    enemy_class = unreal.EditorAssetLibrary.load_blueprint_class("/Game/FirstPerson/Blueprints/enemy/enemy_BP")
    if None in (tree, blackboard, controller_class, enemy_class):
        raise RuntimeError("Missing enemy behavior assets.")
    if unreal.get_default_object(controller_class).get_editor_property("behavior_tree_asset") != tree:
        raise RuntimeError("Controller is not bound to the editable tree.")
    if unreal.get_default_object(enemy_class).get_editor_property("ai_controller_class") != controller_class:
        raise RuntimeError("Enemy is not bound to the Blueprint controller.")
    editor = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
    if not editor.open_editor_for_assets([tree]):
        raise RuntimeError("Cannot open the behavior tree editor.")
    if not unreal.EditorAssetLibrary.save_loaded_asset(tree, only_if_is_dirty=False):
        raise RuntimeError("Cannot save the editable graph.")
    unreal.log(
        "FP_ENEMY_BT_EDITOR_SAVE_COMPLETED: controller/enemy bindings checked; "
        "behavior-tree editor opened and asset saved. "
        "Run fpstrue.AI.BehaviorTree.AssetGraphAndBindings for serialized graph validation."
    )
finally:
    unreal.SystemLibrary.quit_editor()
