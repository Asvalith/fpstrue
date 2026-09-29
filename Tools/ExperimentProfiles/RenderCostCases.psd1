# Shared configuration for RunRenderCostMatrix.ps1. Data only: no UE calls here.
# Normal runs select a JSON preset in this directory. Defaults retain the legacy CLI contract.
@{
    Defaults = @{
        Counts = @(20, 50, 100, 160)
        RunsPerCase = 3
        WarmupSeconds = 15
        DurationSeconds = 30
        ScreenPercentage = $null
        PlayerHealth = 1000000
        BenchmarkSeed = 1337
        VariantNames = @('Baseline', 'EnemyRayTracingOff', 'EnemyShadowsOff')
        ExpectedAvoidanceMode = 'Any'
        CaptureTaskTrace = $false
        BalancedVariantOrder = $false
        StartTrimSeconds = 0
        EndTrimSeconds = 0
        Map = '/Game/FactoryDistrict/Maps/Demonstration'
    }
    Capture = @{
        EditorPath = 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe'
        Width = 1600
        Height = 900
        TraceChannels = 'cpu,gpu,frame,bookmark,task,stats,region,counters'
        IsolationPollMilliseconds = 750
        ConflictingProcessNames = @('UnrealEditor', 'UnrealEditor-Cmd', 'UnrealInsights', 'Insights', 'ShaderCompileWorker', 'UnrealLightmass')
        MinimumTimeoutSeconds = 300
        TimeoutSlackSeconds = 180
        # All variants retain normal hardware RT / skeletal geometry unless explicitly disabled.
        CommonCVars = @{
            'r.RayTracing.ForceAllRayTracingEffects' = -1
            'r.RayTracing.Geometry.SkeletalMeshes' = 1
        }
        ReadbackCVars = @(
            'r.AllowOcclusionQueries', 'r.HZBOcclusion', 'r.NumBufferedOcclusionQueries'
            'r.Lumen.Reflections.DownsampleFactor', 'r.Lumen.ScreenProbeGather.DownsampleFactor'
            'r.TSR.History.ScreenPercentage', 'r.RayTracing.Geometry.SplineMeshes'
            'r.ScreenPercentage', 'r.DynamicRes.OperationMode', 'r.DynamicRes.TestScreenPercentage'
            't.MaxFPS', 'r.VSync', 'sg.ResolutionQuality'
        )
    }
    # CVars generates BOTH launch overrides and final-value checks. No second expected-value table.
    # Additional read-only requirements live beside the variant, never in the runner.
    Variants = @(
        @{ Name = 'Baseline'; CVars = @{} }
        @{ Name = 'EnemyRayTracingOff'; Switch = '-BenchmarkEnemyRayTracingOff'; CVars = @{}; DisabledConsumer = 'RayTracing' }
        @{ Name = 'EnemyShadowsOff'; Switch = '-BenchmarkEnemyShadowsOff'; CVars = @{}; DisabledConsumer = 'Shadows' }

        # Process-local diagnosis only. Disabling culling may INCREASE draw/GPU costs;
        # this isolates the query dependency, not an approved production optimization.
        @{ Name = 'OcclusionQueriesOn'; CVars = @{ 'r.AllowOcclusionQueries' = 1 } }
        @{ Name = 'OcclusionQueriesOff'; CVars = @{ 'r.AllowOcclusionQueries' = 0 } }
        # Keep occlusion enabled in all three candidates. Each differs from the explicit
        # hardware control by ONE policy value; do not combine HZB and buffering changes.
        @{
            Name = 'HardwareQueries'
            CVars = @{ 'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 1 }
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        @{
            Name = 'HZBOcclusion'
            CVars = @{ 'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 1; 'r.NumBufferedOcclusionQueries' = 1 }
        }
        @{
            Name = 'BufferedQueries2'
            CVars = @{ 'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 2 }
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        # GPU screening candidates. Each changes one sampling density and is
        # validated from the final echoed CVar before the run can be accepted.
        @{ Name = 'LumenReflectionsDS2'; CVars = @{ 'r.Lumen.Reflections.DownsampleFactor' = 2 } }
        @{ Name = 'LumenScreenProbeDS32'; CVars = @{ 'r.Lumen.ScreenProbeGather.DownsampleFactor' = 32 } }
        # Final interaction check: explicit values make this A/B independent of
        # project defaults before the winning pair is committed to DefaultEngine.ini.
        @{
            Name = 'OriginalRenderPolicy'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0
                'r.NumBufferedOcclusionQueries' = 1; 'r.Lumen.Reflections.DownsampleFactor' = 1
            }
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        @{
            Name = 'OptimizedRenderPolicy'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0
                'r.NumBufferedOcclusionQueries' = 2; 'r.Lumen.Reflections.DownsampleFactor' = 2
            }
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        # Isolate the TSR history resolution while keeping the current query/Lumen policy fixed.
        @{
            Name = 'TSRHistory200'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 2
                'r.Lumen.Reflections.DownsampleFactor' = 2; 'r.Lumen.ScreenProbeGather.DownsampleFactor' = 16
                'r.TSR.History.ScreenPercentage' = 200
            }
            RequireDefaultResolution = $true
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        @{
            Name = 'TSRHistory150'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 2
                'r.Lumen.Reflections.DownsampleFactor' = 2; 'r.Lumen.ScreenProbeGather.DownsampleFactor' = 16
                'r.TSR.History.ScreenPercentage' = 150
            }
            RequireDefaultResolution = $true
            ExpectedConsumers = @{ Shadows = 5; RayTracing = 12 }
        }
        # Diagnostic only: removing spline geometry from ray tracing is not the final fix.
        # A separate process restores the explicit On control; no defaults/assets change.
        @{
            Name = 'SplineRayTracingOn'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 2
                'r.Lumen.Reflections.DownsampleFactor' = 2; 'r.Lumen.ScreenProbeGather.DownsampleFactor' = 16
                'r.TSR.History.ScreenPercentage' = 200; 'r.RayTracing.Geometry.SplineMeshes' = 1
            }
            RequireDefaultResolution = $true
        }
        @{
            Name = 'SplineRayTracingOff'
            CVars = @{
                'r.AllowOcclusionQueries' = 1; 'r.HZBOcclusion' = 0; 'r.NumBufferedOcclusionQueries' = 2
                'r.Lumen.Reflections.DownsampleFactor' = 2; 'r.Lumen.ScreenProbeGather.DownsampleFactor' = 16
                'r.TSR.History.ScreenPercentage' = 200; 'r.RayTracing.Geometry.SplineMeshes' = 0
            }
            RequireDefaultResolution = $true
        }
    )
}
