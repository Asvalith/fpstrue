"""Build a reversible hard-surface Nanite candidate; never save shared source assets.

Run in the UE 5.5 editor with -ExecutePythonScript="... --tag HardSurface20260929".
The whitelist is intentionally small. This does not merge actors, change lights,
reduce fallback geometry, or modify the accepted map. --verify only reloads and
checks the saved candidate. Performance capture uses RunRenderCostMatrix.ps1.
"""
import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

import unreal

SOURCE_MAP = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked'
MESH_NAMES = {
    'Pallet', 'ConcreteBlock', 'Oil_Drum', 'Vent_A_End', 'Vent_A_Long',
    'Silo_mdl', 'TrainFlat_mdl', 'ABuilding_TypeA_C_Frame',
}
# Mesh references are the only intended component change. Include collision,
# navigation and rendering state so save/reload cannot silently broaden scope.
COMPONENT_PROPERTIES = (
    'mobility', 'hidden_in_game', 'cast_shadow', 'cast_dynamic_shadow',
    'cast_static_shadow', 'cast_hidden_shadow', 'visible_in_ray_tracing',
    'render_in_main_pass', 'render_in_depth_pass', 'render_custom_depth',
    'affect_dynamic_indirect_lighting', 'affect_distance_field_lighting',
    'can_ever_affect_navigation', 'generate_overlap_events', 'use_default_collision',
    'bounds_scale', 'ld_max_draw_distance', 'min_draw_distance',
    'allow_cull_distance_volume', 'forced_lod_model', 'override_min_lod', 'min_lod',
    'evaluate_world_position_offset', 'evaluate_world_position_offset_in_ray_tracing',
)


def require(value, message):
    if not value:
        raise RuntimeError(message)


def encoded(value):
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if hasattr(value, 'export_text'):
        return value.export_text()
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def write_report(path, report):
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


def component_snapshot(component):
    return {
        'transform': component.get_world_transform().export_text(),
        'visible': component.is_visible(),
        'collision': str(component.get_collision_enabled()),
        'collision_profile': str(component.get_collision_profile_name()),
        'materials': [m.get_path_name() if m else None for m in component.get_materials()],
        'properties': {p: encoded(component.get_editor_property(p)) for p in COMPONENT_PROPERTIES},
    }


def mesh_snapshot(mesh, mesh_editor):
    body = mesh.get_editor_property('body_setup')
    bounds = mesh.get_bounding_box()
    return {
        'lod_triangles': [mesh.get_num_triangles(i) for i in range(mesh_editor.get_lod_count(mesh))],
        'lod_sections': [mesh.get_num_sections(i) for i in range(mesh_editor.get_lod_count(mesh))],
        'bounds': [bounds.min.x, bounds.min.y, bounds.min.z,
                   bounds.max.x, bounds.max.y, bounds.max.z],
        'materials': [encoded(m.material_interface) for m in mesh.get_editor_property('static_materials')],
        'lod_for_collision': mesh.get_editor_property('lod_for_collision'),
        'support_ray_tracing': mesh.get_editor_property('support_ray_tracing'),
        'complex_collision_mesh': encoded(mesh.get_editor_property('complex_collision_mesh')),
        'body': {p: encoded(body.get_editor_property(p)) for p in (
            'collision_trace_flag', 'agg_geom', 'phys_material',
            'double_sided_geometry', 'walkable_slope_override')} if body else None,
    }


def verify_mesh_snapshot(before, after, label):
    # Rebuilding float vertex bounds through UE's double-precision FBox can
    # shift the last bits (e.g. 0.000031 cm). Do not confuse that with lost LODs
    # or changed collision. Only bounds get a tolerance; all other fields match.
    changed = [key for key in before if key != 'bounds' and before[key] != after[key]]
    bounds_error = max(abs(a - b) for a, b in zip(before['bounds'], after['bounds']))
    require(not changed and bounds_error <= 0.001,
            f'Geometry/collision changed: {label}; fields={changed}, bounds_error_cm={bounds_error}')
    return bounds_error


def material_base(material):
    visited = set()
    while isinstance(material, unreal.MaterialInstance):
        path = material.get_path_name()
        require(path not in visited, 'Material parent cycle: ' + path)
        visited.add(path)
        material = material.get_editor_property('parent')
    return material


def candidates(actor_editor, mesh_editor):
    selected, excluded = [], Counter()
    for actor in actor_editor.get_all_level_actors():
        if actor.get_class() != unreal.StaticMeshActor.static_class():
            continue
        component = actor.static_mesh_component
        mesh = component.static_mesh
        if not mesh or mesh.get_name() not in MESH_NAMES:
            continue
        if mesh_editor.get_nanite_settings(mesh).enabled:
            excluded['already_nanite'] += 1
            continue
        if component.mobility != unreal.ComponentMobility.STATIC or component.is_simulating_physics():
            excluded['not_static'] += 1
            continue
        if actor.is_actor_tick_enabled() or actor.get_editor_property('replicates') or actor.get_attach_parent_actor():
            excluded['gameplay_or_attachment'] += 1
            continue
        if not component.is_visible() or component.get_editor_property('hidden_in_game'):
            excluded['hidden'] += 1
            continue
        if mesh_editor.has_instance_vertex_colors(component):
            excluded['instance_vertex_colors'] += 1
            continue
        # Check both asset slots and component overrides. Masked/transparent meshes
        # are deliberately outside this batch, not assumed unsupported by Nanite.
        materials = list(component.get_materials()) + [m.material_interface for m in mesh.static_materials]
        if any(not isinstance(material_base(m), unreal.Material) or
               material_base(m).get_editor_property('blend_mode') != unreal.BlendMode.BLEND_OPAQUE
               for m in materials):
            excluded['not_all_opaque'] += 1
            continue
        selected.append((actor, component))
    return selected, dict(excluded)


def verify(report, actor_editor, mesh_editor):
    require(unreal.EditorLoadingAndSavingUtils.load_map(report['candidate_map']), 'Cannot reload candidate.')
    actors = actor_editor.get_all_level_actors()
    by_name = {a.get_name(): a for a in actors}
    require(len(actors) == report['actor_count'], 'Actor count changed.')
    for row in report['components']:
        component = by_name[row['actor']].static_mesh_component
        require(component.static_mesh.get_path_name() == row['replacement'], 'Mesh reference changed.')
        require(component_snapshot(component) == row['before'], 'Component changed: ' + row['actor'])
    bounds_errors = []
    for row in report['assets']:
        mesh = unreal.load_asset(row['replacement'])
        settings = mesh_editor.get_nanite_settings(mesh)
        require(settings.enabled and settings.fallback_percent_triangles == 1.0 and
                settings.fallback_relative_error == 0.0, 'Nanite/fallback settings not retained.')
        bounds_errors.append(verify_mesh_snapshot(row['before'], mesh_snapshot(mesh, mesh_editor), row['source']))
    for filename, expected in report['source_sha256'].items():
        require(hashlib.sha256(Path(filename).read_bytes()).hexdigest() == expected, 'Source file changed: ' + filename)
    return {'candidate_map': report['candidate_map'], 'assets': len(report['assets']),
            'components': len(report['components']), 'reload_verified': True,
            'max_bounds_error_cm': max(bounds_errors, default=0),
            'scope': 'component state, fallback LOD triangles/sections, bounds, collision settings and source hashes; not motion-image acceptance'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', required=True)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--inspect', action='store_true', help='Read-only comparison of existing candidate meshes.')
    args = parser.parse_args()
    require(args.tag.isalnum(), 'Tag must be alphanumeric.')
    target_root = '/Game/PerformanceCandidates/Nanite_' + args.tag
    output = Path(unreal.Paths.project_saved_dir()) / 'Profiling' / ('Nanite_' + args.tag)
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / 'build.json'
    actor_editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    if args.inspect:
        comparison = {}
        for name in sorted(MESH_NAMES):
            target = target_root + '/Meshes/' + name
            if unreal.EditorAssetLibrary.does_asset_exist(target):
                before = mesh_snapshot(unreal.load_asset('/Game/FactoryDistrict/Meshes/' + name), mesh_editor)
                after = mesh_snapshot(unreal.load_asset(target), mesh_editor)
                comparison[name] = {'before': before, 'after': after,
                                    'changed_fields': [k for k in before if before[k] != after[k]]}
        write_report(output / 'inspection.json', comparison)
        unreal.log('SCENE_NANITE_INSPECTION ' + json.dumps({k: v['changed_fields'] for k, v in comparison.items()}))
        return
    if args.verify:
        result = verify(json.loads(manifest.read_text(encoding='utf-8')), actor_editor, mesh_editor)
        write_report(output / 'verification.json', result)
        unreal.log('SCENE_NANITE_VERIFIED ' + json.dumps(result))
        return

    require(not manifest.exists() and not unreal.EditorAssetLibrary.does_directory_exist(target_root),
            'Candidate already exists; use --verify or a new tag.')
    world = unreal.EditorLoadingAndSavingUtils.new_map_from_template(SOURCE_MAP, False)
    require(world, 'Cannot open source template.')
    selected, excluded = candidates(actor_editor, mesh_editor)
    require(selected, 'No eligible hard-surface meshes.')
    report = {'source_map': SOURCE_MAP, 'candidate_map': target_root + '/Demonstration_Nanite',
              'actor_count': len(actor_editor.get_all_level_actors()), 'excluded': excluded,
              'whitelist': sorted(MESH_NAMES), 'assets': [], 'components': [], 'source_sha256': {},
              'status': 'building', 'fallback_policy': '100% triangles, zero relative error; no intentional collision/RT simplification'}
    project_content = Path(unreal.Paths.project_content_dir()).resolve()

    def record_source(package, suffix):
        filename = project_content / (package.removeprefix('/Game/').split('.')[0] + suffix)
        require(filename.is_file(), 'Missing source package: ' + str(filename))
        report['source_sha256'][str(filename)] = hashlib.sha256(filename.read_bytes()).hexdigest()

    record_source(SOURCE_MAP, '.umap')
    replacements = {}
    for actor, component in selected:
        source = component.static_mesh
        source_path = source.get_path_name()
        if source_path not in replacements:
            record_source(source_path, '.uasset')
            before = mesh_snapshot(source, mesh_editor)
            target = target_root + '/Meshes/' + source.get_name()
            duplicate = unreal.EditorAssetLibrary.duplicate_asset(source_path, target)
            require(duplicate, 'Mesh duplication failed: ' + source_path)
            settings = mesh_editor.get_nanite_settings(duplicate)
            settings.enabled = True
            settings.fallback_target = unreal.NaniteFallbackTarget.PERCENT_TRIANGLES
            settings.fallback_percent_triangles = 1.0
            settings.fallback_relative_error = 0.0
            mesh_editor.set_nanite_settings(duplicate, settings, True)
            require(unreal.EditorAssetLibrary.save_loaded_asset(duplicate, False), 'Mesh save failed.')
            verify_mesh_snapshot(before, mesh_snapshot(duplicate, mesh_editor), source_path)
            replacements[source_path] = duplicate
            report['assets'].append({'source': source_path, 'replacement': duplicate.get_path_name(), 'before': before})
            unreal.log('SCENE_NANITE_ASSET ' + source_path)
        duplicate = replacements[source_path]
        snapshot = component_snapshot(component)
        require(component.set_static_mesh(duplicate), 'Cannot replace mesh: ' + actor.get_name())
        report['components'].append({'actor': actor.get_name(), 'source': source_path,
                                     'replacement': duplicate.get_path_name(), 'before': snapshot})
    require(unreal.EditorLoadingAndSavingUtils.save_map(world, report['candidate_map']), 'Cannot save candidate map.')
    report['status'] = 'built_pending_validation'
    write_report(manifest, report)
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    result = verify(report, actor_editor, mesh_editor)
    write_report(output / 'verification.json', result)
    report['status'] = 'reload_verified_pending_performance'
    write_report(manifest, report)
    unreal.log('SCENE_NANITE_SUCCESS ' + json.dumps(result))


if __name__ == '__main__':
    main()
