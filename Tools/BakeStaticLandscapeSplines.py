"""UE 5.5 editor-only, reversible rendering candidate (original map is never saved).

Run with -ExecutePythonScript=".../BakeStaticLandscapeSplines.py --tag Pilot --limit 2"
and -EnablePlugins=GeometryScripting, with a normal D3D12 editor (not -NullRHI).
Omit --limit to bake all static tracks/gravel.
The original spline components remain collision-only. Generated render meshes retain
deformation, material slots, all rendered LODs, shadows and ray-tracing participation.
Editing landscape splines after baking requires regeneration; this is not a runtime tool.
"""
import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

import unreal

SOURCE_MAP = '/Game/FactoryDistrict/Maps/Demonstration'
MESH_NAMES = {'Tracks_mdl', 'Track_Gravel_mdl'}
RENDER_PROPERTIES = (
    'cast_shadow', 'cast_dynamic_shadow', 'cast_static_shadow', 'cast_contact_shadow',
    'cast_hidden_shadow', 'visible_in_ray_tracing', 'visible_in_reflection_captures',
    'visible_in_real_time_sky_captures', 'affect_dynamic_indirect_lighting',
    'affect_distance_field_lighting', 'render_in_main_pass', 'render_in_depth_pass',
    'render_custom_depth', 'custom_depth_stencil_value', 'lighting_channels',
    'bounds_scale', 'ld_max_draw_distance', 'min_draw_distance', 'allow_cull_distance_volume',
    'forced_lod_model', 'override_min_lod', 'min_lod', 'reverse_culling',
    'evaluate_world_position_offset', 'evaluate_world_position_offset_in_ray_tracing',
    'world_position_offset_disable_distance',
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def components(actor_subsystem):
    return [c for a in actor_subsystem.get_all_level_actors()
            for c in a.get_components_by_class(unreal.SplineMeshComponent)
            if c.static_mesh and c.static_mesh.get_name() in MESH_NAMES]


def bake_key(component):
    # Exact local spline parameters, never world placement or rounded curve values.
    fields = ['spline_params', 'forward_axis', 'spline_up_dir',
              'spline_boundary_min', 'spline_boundary_max', 'smooth_interp_roll_scale']
    values = {'mesh': component.static_mesh.get_path_name()}
    for name in fields:
        value = component.get_editor_property(name)
        values[name] = value.export_text() if hasattr(value, 'export_text') else str(value)
    return hashlib.sha256(json.dumps(values, sort_keys=True).encode()).hexdigest()[:20]


def extract(component, lod):
    options = unreal.GeometryScriptCopyMeshFromComponentOptions()
    options.want_instance_colors = True
    options.requested_lod = unreal.GeometryScriptMeshReadLOD(
        lod_type=unreal.GeometryScriptLODType.RENDER_DATA, lod_index=lod)
    mesh = unreal.DynamicMesh()
    _, _, outcome = unreal.GeometryScript_SceneUtils.copy_mesh_from_component(
        component, mesh, options, False)
    require(outcome == unreal.GeometryScriptOutcomePins.SUCCESS and mesh.get_triangle_count() > 0,
            f'Cannot extract deformed LOD {lod}: {component.get_path_name()}')
    return mesh


def preserve_imported_lods(mesh_editor, mesh, lod_count):
    # SetNumSourceModels assigns automatic reductions to newly added LOD slots.
    # Our source data is ALREADY each deformed render LOD: never reduce it again.
    for lod in range(1, lod_count):
        settings = unreal.MeshReductionSettings(percent_triangles=1.0, percent_vertices=1.0,
                                                max_deviation=0.0, base_lod_model=lod)
        mesh_editor.set_lod_reduction_settings(mesh, lod, settings)


def replace_render_component(actor_editor, source, baked):
    actor = actor_editor.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector())
    require(actor is not None, 'Cannot create replacement actor.')
    actor.set_actor_label('Baked_' + source.get_name())
    actor.set_folder_path('PerformanceCandidates/BakedTracks')
    destination = actor.static_mesh_component
    destination.set_mobility(unreal.ComponentMobility.MOVABLE)
    actor.set_actor_transform(source.get_world_transform(), False, True)
    destination.set_static_mesh(baked)
    for prop in RENDER_PROPERTIES:
        destination.set_editor_property(prop, source.get_editor_property(prop))
    for slot, material in enumerate(source.get_materials()):
        destination.set_material(slot, material)
    # Persist the profile, not just CollisionEnabled: BlockAll resets it on reload.
    destination.set_editor_property('use_default_collision', False)
    destination.set_collision_profile_name('NoCollision')
    destination.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    destination.set_editor_property('can_ever_affect_navigation', False)
    destination.set_mobility(unreal.ComponentMobility.STATIC)
    collision_before = str(source.get_collision_enabled())
    profile_before = str(source.get_collision_profile_name())
    # Preserve the authored collision/navigation; only replace rendering.
    source.set_visibility(False)
    source.set_hidden_in_game(True)
    source.set_editor_property('visible_in_ray_tracing', False)
    source.set_cast_shadow(False)
    source.set_editor_property('affect_dynamic_indirect_lighting', False)
    source.set_editor_property('affect_distance_field_lighting', False)
    require(str(source.get_collision_enabled()) == collision_before, 'Original collision changed.')
    return actor, destination, collision_before, profile_before


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', required=True)
    parser.add_argument('--limit', type=int, default=0)
    parser.add_argument('--repair-lods', action='store_true', help='Repair only generated assets listed in this tag manifest.')
    parser.add_argument('--publish', action='store_true', help='Apply verified candidate to source map; requires a matching source backup.')
    args = parser.parse_args()
    require(args.tag.isalnum() and args.limit >= 0, 'Use an alphanumeric tag and non-negative limit.')
    target_root = '/Game/PerformanceCandidates/SplineBake_' + args.tag
    target_map = target_root + '/Demonstration_Baked'
    if args.publish:
        import csv
        saved_root = Path(unreal.Paths.project_saved_dir()) / 'Profiling' / ('SplineBake_' + args.tag)
        verification = json.loads((saved_root / 'verification.json').read_text(encoding='utf-8'))
        require(verification['map'] == target_map and not verification['errors'], 'Asset verification has not passed.')
        acceptance = saved_root / 'acceptance.json'
        approval = json.loads(acceptance.read_text(encoding='utf-8'))
        require(approval['candidate_map'] == target_map and approval['apply_candidate'], 'No completed acceptance decision.')
        with Path(approval['manifest']).open(encoding='utf-8-sig', newline='') as stream:
            rows = list(csv.DictReader(stream))
        require({int(row['EnemyCount']) for row in rows} == {0, 160} and
                all(row['Valid'] == 'True' for row in rows), 'Incomplete zero/160 acceptance.')
        original_file = Path(unreal.Paths.project_content_dir()) / 'FactoryDistrict/Maps/Demonstration.umap'
        backup_file = saved_root / 'Backup/Demonstration.umap'
        source_hash = hashlib.sha256(original_file.read_bytes()).hexdigest()
        require(backup_file.is_file() and hashlib.sha256(backup_file.read_bytes()).hexdigest() == source_hash,
                'Original map must have an exact backup before publication.')
        require(source_hash == approval['source_sha256'].lower(), 'Source map changed since the acceptance decision.')
        # Apply the same narrow delta to the original world. Saving the duplicate
        # over it can fail when UE still holds references to the original package.
        world = unreal.EditorLoadingAndSavingUtils.load_map(SOURCE_MAP)
        require(world is not None, 'Cannot load source map.')
        actor_editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        sources = {c.get_name(): c for c in components(actor_editor)}
        manifest = json.loads((saved_root / 'bake_manifest.json').read_text(encoding='utf-8'))
        require(not any(a.get_actor_label().startswith('Baked_') for a in actor_editor.get_all_level_actors()),
                'Source already contains replacements; refusing duplicate application.')
        for row in manifest['rows']:
            require(row['mesh'].startswith(target_root + '/Meshes/'), 'Unexpected replacement asset.')
            source = sources[row['source_name']]
            baked = unreal.load_asset(row['mesh'])
            require(baked is not None, 'Replacement mesh missing.')
            replace_render_component(actor_editor, source, baked)
        require(unreal.EditorLoadingAndSavingUtils.save_map(world, SOURCE_MAP), 'Cannot publish map.')
        unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
        unreal.EditorLoadingAndSavingUtils.load_map(SOURCE_MAP)
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
        require(sum(a.get_actor_label().startswith('Baked_') for a in actors) == verification['replacements'],
                'Published map lost baked actors.')
        approval.update({'published_map': SOURCE_MAP, 'backup': str(backup_file),
                         'published_sha256': hashlib.sha256(original_file.read_bytes()).hexdigest()})
        acceptance.write_text(json.dumps(approval, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('SPLINE_BAKE_PUBLISHED ' + SOURCE_MAP)
        return
    if args.repair_lods:
        saved_root = Path(unreal.Paths.project_saved_dir()) / 'Profiling' / ('SplineBake_' + args.tag)
        manifest = json.loads((saved_root / 'bake_manifest.json').read_text(encoding='utf-8'))
        require(manifest['candidate_map'] == target_map, 'Candidate path mismatch.')
        mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        for row in manifest['rows']:
            require(row['mesh'].startswith(target_root + '/Meshes/'), 'Refusing to change a source asset.')
            if len(row['lod_triangles']) > 1:
                mesh = unreal.load_asset(row['mesh'])
                preserve_imported_lods(mesh_editor, mesh, len(row['lod_triangles']))
                require(unreal.EditorAssetLibrary.save_loaded_asset(mesh, False), 'Cannot save repaired asset.')
        unreal.log('SPLINE_BAKE_LODS_REPAIRED ' + target_map)
        return
    require(not unreal.EditorAssetLibrary.does_directory_exist(target_root),
            f'Refusing to overwrite existing candidate: {target_root}')
    actor_editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    require(mesh_editor is not None, 'Use full editor -ExecutePythonScript, not a commandlet.')
    # Template loading protects the source package even if an exception interrupts baking.
    world = unreal.EditorLoadingAndSavingUtils.new_map_from_template(SOURCE_MAP, False)
    require(world is not None, 'Cannot create candidate map from template.')
    require(not any(a.get_actor_label().startswith('Baked_') for a in actor_editor.get_all_level_actors()),
            'Source is already baked. Restore the recorded original backup before generating another candidate.')
    candidates = sorted(components(actor_editor), key=lambda c: (c.static_mesh.get_name(), c.get_name()))
    if args.limit:
        # A small pilot must include both asset types, not just two adjacent gravel sections.
        first_per_type = [next(c for c in candidates if c.static_mesh.get_name() == name)
                          for name in sorted(MESH_NAMES)]
        candidates = (first_per_type + [c for c in candidates if c not in first_per_type])[:args.limit]
    require(candidates, 'No matching track/gravel components.')
    # Preflight every source before any asset is created.
    for component in candidates:
        require(component.mobility == unreal.ComponentMobility.STATIC, 'Runtime spline cannot be baked.')
        require(component.get_owner().get_class().get_name() == 'Landscape', 'Unexpected source owner.')
        require(not component.static_mesh.get_editor_property('nanite_settings').enabled, 'Nanite conversion is outside this experiment.')
        for prop in RENDER_PROPERTIES:
            component.get_editor_property(prop)
        bake_key(component)
    report = {'source_map': SOURCE_MAP, 'candidate_map': target_map,
              'collision_policy': 'unchanged original spline, generated render component NoCollision',
              'sources': dict(Counter(c.static_mesh.get_name() for c in candidates)), 'rows': []}
    saved_root = Path(unreal.Paths.project_saved_dir()) / 'Profiling' / ('SplineBake_' + args.tag)
    saved_root.mkdir(parents=True, exist_ok=True)
    cache = {}
    for index, source in enumerate(candidates):
        original = source.static_mesh
        key = bake_key(source)
        if key not in cache:
            lod_count = mesh_editor.get_lod_count(original)
            lod_meshes = [extract(source, lod) for lod in range(lod_count)]
            triangle_counts = [dm.get_triangle_count() for dm in lod_meshes]
            asset_path = target_root + '/Meshes/SM_' + original.get_name() + '_' + key
            options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions(enable_collision=False)
            baked, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh_lo_ds(
                lod_meshes, asset_path, options)
            require(outcome == unreal.GeometryScriptOutcomePins.SUCCESS and baked, 'Static mesh creation failed.')
            baked.set_editor_property('static_materials', original.get_editor_property('static_materials'))
            baked.set_editor_property('support_ray_tracing', original.get_editor_property('support_ray_tracing'))
            baked.set_editor_property('light_map_coordinate_index', original.get_editor_property('light_map_coordinate_index'))
            preserve_imported_lods(mesh_editor, baked, lod_count)
            require(mesh_editor.set_lod_screen_sizes(baked, mesh_editor.get_lod_screen_sizes(original)), 'LOD thresholds not copied.')
            require(mesh_editor.get_lod_count(baked) == lod_count, 'Lost a LOD during baking.')
            require(unreal.EditorAssetLibrary.save_loaded_asset(baked, False), 'Cannot save generated asset.')
            cache[key] = (baked, triangle_counts)
        baked, triangle_counts = cache[key]
        unreal.log(f'SPLINE_BAKE_REPLACE {index + 1}/{len(candidates)} {source.get_name()}')
        actor, destination, collision_before, profile_before = replace_render_component(actor_editor, source, baked)
        report['rows'].append({'source_name': source.get_name(), 'actor_label': actor.get_actor_label(),
                               'mesh': baked.get_path_name(), 'lod_triangles': triangle_counts,
                               'collision': collision_before, 'profile': profile_before,
                               'ray_tracing': bool(destination.get_editor_property('visible_in_ray_tracing')),
                               'cast_shadow': bool(destination.get_editor_property('cast_shadow'))})
        if (index + 1) % 25 == 0:
            unreal.log(f'SPLINE_BAKE_PROGRESS {index + 1}/{len(candidates)}')
    require(unreal.EditorLoadingAndSavingUtils.save_map(world, target_map), 'Cannot save candidate map.')
    report.update({'generated_assets': len(cache), 'replacements': len(candidates), 'reload_verified': False})
    (saved_root / 'bake_manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    # Round-trip validation catches landscape reconstruction undoing component visibility.
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    unreal.EditorLoadingAndSavingUtils.load_map(target_map)
    reloaded = {c.get_name(): c for c in components(actor_editor)}
    baked_actors = {a.get_actor_label(): a for a in actor_editor.get_all_level_actors()
                    if a.get_actor_label().startswith('Baked_')}
    for row in report['rows']:
        source = reloaded[row['source_name']]
        dest = baked_actors[row['actor_label']].static_mesh_component
        require(not source.is_visible() and not source.get_editor_property('visible_in_ray_tracing'), 'Landscape restored original rendering.')
        require(str(source.get_collision_enabled()) == row['collision'] and
                str(source.get_collision_profile_name()) == row['profile'], 'Collision round-trip mismatch.')
        require(dest.is_visible() and dest.get_editor_property('visible_in_ray_tracing') == row['ray_tracing'] and
                dest.get_editor_property('cast_shadow') == row['cast_shadow'], 'Replacement render flags mismatch.')
        require(dest.get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION, 'Duplicate collision.')
    report.update({'generated_assets': len(cache), 'replacements': len(candidates), 'reload_verified': True})
    (saved_root / 'bake_manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    unreal.log(f'SPLINE_BAKE_SUCCESS map={target_map} replacements={len(candidates)} assets={len(cache)}')


if __name__ == '__main__':
    main()
