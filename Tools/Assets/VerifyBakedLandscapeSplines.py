"""Read-only verification of a saved spline candidate; run in the UE editor."""
import argparse
import json
from collections import Counter
from pathlib import Path
import unreal


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    if not args.tag.isalnum():
        raise ValueError('Invalid tag')
    root = Path(unreal.Paths.project_saved_dir()) / 'Profiling' / ('SplineBake_' + args.tag)
    report = json.loads((root / 'bake_manifest.json').read_text(encoding='utf-8'))
    unreal.EditorLoadingAndSavingUtils.load_map(report['candidate_map'])
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    splines = {c.get_name(): c for a in actors for c in a.get_components_by_class(unreal.SplineMeshComponent)}
    labels = {a.get_actor_label(): a for a in actors}
    mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    errors = []
    checked_lods = 0
    max_bounds_error = 0.0
    for row in report['rows']:
        source = splines[row['source_name']]
        dest = labels[row['actor_label']].static_mesh_component
        if source.get_world_transform().export_text() != dest.get_world_transform().export_text():
            errors.append(row['source_name'] + ': transform mismatch')
        if list(source.get_materials()) != list(dest.get_materials()):
            errors.append(row['source_name'] + ': material mismatch')
        if mesh_editor.get_lod_screen_sizes(source.static_mesh) != mesh_editor.get_lod_screen_sizes(dest.static_mesh):
            errors.append(row['source_name'] + ': LOD threshold mismatch')
        for lod, expected_triangles in enumerate(row['lod_triangles']):
            options = unreal.GeometryScriptCopyMeshFromComponentOptions()
            options.requested_lod = unreal.GeometryScriptMeshReadLOD(
                lod_type=unreal.GeometryScriptLODType.RENDER_DATA, lod_index=lod)
            meshes = []
            for component in (source, dest):
                dm = unreal.DynamicMesh()
                _, _, outcome = unreal.GeometryScript_SceneUtils.copy_mesh_from_component(component, dm, options, False)
                if outcome != unreal.GeometryScriptOutcomePins.SUCCESS or dm.get_triangle_count() != expected_triangles:
                    errors.append(row['source_name'] + f': LOD {lod} triangle mismatch')
                meshes.append(dm)
            boxes = [unreal.GeometryScript_MeshQueries.get_mesh_bounding_box(dm) for dm in meshes]
            error = max(abs(getattr(getattr(boxes[0], corner), axis) - getattr(getattr(boxes[1], corner), axis))
                        for corner in ('min', 'max') for axis in ('x', 'y', 'z'))
            max_bounds_error = max(max_bounds_error, error)
            if error > 0.02:
                errors.append(row['source_name'] + f': LOD {lod} geometry bounds differ by {error} cm')
            checked_lods += 1
    light_owners = [a for a in actors if a.get_components_by_class(unreal.LightComponent)]
    audit = {
        'map': report['candidate_map'], 'replacements': len(report['rows']), 'checked_lods': checked_lods,
        'max_geometry_bounds_error_cm': max_bounds_error, 'errors': errors,
        'light_owner_classes': dict(Counter(a.get_class().get_name() for a in light_owners)),
        'tick_enabled_light_owners': [a.get_actor_label() for a in light_owners if a.is_actor_tick_enabled()],
        'scope': 'saved transforms, materials, LOD thresholds/triangle counts and deformed geometry bounds; not motion-image acceptance',
    }
    (root / 'verification.json').write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding='utf-8')
    if errors:
        raise RuntimeError(f'Baked geometry verification failed: {len(errors)} differences; see verification.json')
    unreal.log('SPLINE_BAKE_VERIFIED ' + json.dumps(audit, ensure_ascii=False))


if __name__ == '__main__':
    main()
