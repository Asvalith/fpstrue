"""Run a project's explicit, non-saving Blueprint/C++ contract audit and exit.

Use the Python commandlet with PythonScriptPlugin and EditorScriptingUtilities.
Co-op requires -ContractAuditReport=<new absolute JSON path>; the native audit
refuses overwriting reports. This script does not edit or save Content.
"""

import re

import unreal


project = unreal.Paths.get_project_file_path().replace("\\", "/").rsplit("/", 1)[-1].lower()
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
if project == "fpstrue.uproject":
    unreal.SystemLibrary.execute_console_command(world, "fp.AuditGameplayContracts")
elif project == "multiplayer.uproject":
    match = re.search(r'-ContractAuditReport=(?:"([^"]+)"|(\S+))', unreal.SystemLibrary.get_command_line())
    if not match:
        raise RuntimeError("Co-op contract audit requires a new -ContractAuditReport path")
    report = match.group(1) or match.group(2)
    unreal.SystemLibrary.execute_console_command(world, 'coop.AuditGameplayContracts "' + report + '"')
else:
    raise RuntimeError("Unsupported project: " + project)
