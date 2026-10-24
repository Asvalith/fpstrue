"""Run the scoped native asset migration, then let the Python commandlet exit.

Default is a read-only node audit. Add -FPMultiplayerMigrationApply to back up,
migrate and compile the four reviewed maps and two HUD layouts.
"""

import unreal


apply = "-FPMultiplayerMigrationApply" in unreal.SystemLibrary.get_command_line().split()
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
unreal.SystemLibrary.execute_console_command(world, "fp.MigrateMultiplayerOnly" + (" Apply" if apply else ""))
