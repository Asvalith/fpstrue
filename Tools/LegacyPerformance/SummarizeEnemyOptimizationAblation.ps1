param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot
)

$ErrorActionPreference = "Stop"

function Get-NumericValues {
    param([object[]]$Rows, [string]$Column)
    $Values = New-Object System.Collections.Generic.List[double]
    foreach ($Row in $Rows) {
        $Value = 0.0
        if ([double]::TryParse($Row.$Column, [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture, [ref]$Value)) {
            $Values.Add($Value)
        }
    }
    return $Values.ToArray()
}

function Get-Average {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    return ($Values | Measure-Object -Average).Average
}

function Get-AveragePositive {
    param([double[]]$Values)
    $PositiveValues = @($Values | Where-Object { $_ -gt 0.0 })
    if ($PositiveValues.Count -eq 0) { return 0.0 }
    return ($PositiveValues | Measure-Object -Average).Average
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { return 0.0 }
    $Sorted = $Values | Sort-Object
    $Index = [Math]::Ceiling($Percentile * $Sorted.Count) - 1
    return $Sorted[[Math]::Max(0, [Math]::Min($Index, $Sorted.Count - 1))]
}

function Import-CsvWithUniqueHeaders {
    param([string]$Path)
    $Lines = @(Get-Content -LiteralPath $Path)
    if ($Lines.Count -lt 2) { return @() }
    $HeaderCounts = @{}
    $Headers = @($Lines[0] -split ',' | ForEach-Object {
        $Name = $_.Trim('"')
        if ($HeaderCounts.ContainsKey($Name)) {
            ++$HeaderCounts[$Name]
            return "${Name}__$($HeaderCounts[$Name])"
        }
        $HeaderCounts[$Name] = 1
        return $Name
    })
    return @($Lines | Select-Object -Skip 1 | ConvertFrom-Csv -Header $Headers)
}

$Manifest = Import-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv")
$RunSummary = @()

foreach ($Entry in $Manifest) {
    $Rows = @(Import-CsvWithUniqueHeaders $Entry.Csv | Where-Object {
        $Value = 0.0
        [double]::TryParse($_.FrameTime, [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture, [ref]$Value) -and $Value -gt 0.0
    })

    $Frame = Get-NumericValues $Rows "FrameTime"
    $FrameAverage = Get-Average $Frame
    $RunSummary += [PSCustomObject]@{
        Group = $Entry.Group
        Run = [int]$Entry.Run
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
        GameThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GameThreadTime")), 3)
        RenderThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "RenderThreadTime")), 3)
        RHIThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "RHIThreadTime")), 3)
        GpuMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPUTime")), 3)
        SkinnedGeometryBuildBLASMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPU/SkinnedGeometryBuildBLAS")), 3)
        RayTracingDynamicGeometryMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPU/RayTracingDynamicGeometry")), 3)
        RayTracingSceneMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPU/RayTracingScene")), 3)
        ShadowDepthsMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPU/ShadowDepths")), 3)
        ShadowProjectionMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPU/ShadowProjection")), 3)
        DrawCalls = [Math]::Round((Get-Average (Get-NumericValues $Rows "RHI/DrawCalls")), 1)
        ShadowDrawCalls = [Math]::Round((Get-Average (Get-NumericValues $Rows "DrawCall/ShadowDepths")), 1)
        CharacterMovementMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/CharacterMovement")), 3)
        CharacterMovementTicks = [Math]::Round((Get-Average (Get-NumericValues $Rows "Ticks/CharacterMovementComponent")), 1)
        AnimationMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/Animation")), 3)
        AIDecisionMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/GameThread/DecisionTime")), 3)
        AIDecisionsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/DecisionCount")), 3)
        MoveRequestsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/MoveRequestCount")), 3)
        SignificanceUpdateMs = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/GameThread/UpdateTime")), 4)
        LOD0Enemies = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/LOD0")), 1)
        LOD1Enemies = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/LOD1")), 1)
        LOD2PlusEnemies = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/LOD2Plus")), 1)
        ShadowCasters = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/ShadowCasters")), 1)
        RayTracingVisible = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/RayTracingVisible")), 1)
        FullBudgetDowngrades = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/FullBudgetDowngrades")), 1)
        ShadowBudgetRejected = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/ShadowBudgetRejected")), 1)
        RayTracingBudgetRejected = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/RayTracingBudgetRejected")), 1)
        AnimationSharingFollowers = [Math]::Round((Get-AveragePositive (Get-NumericValues $Rows "fpstrueSignificance/AnimationSharingFollowers")), 1)
        AverageEnemyActors = [Math]::Round((Get-Average (Get-NumericValues $Rows "ActorCount/fpstrueEnemyCharacter")), 1)
    }
}

$GroupSummary = @()
foreach ($Group in @($RunSummary.Group | Sort-Object -Unique)) {
    $Rows = @($RunSummary | Where-Object { $_.Group -eq $Group })
    $GroupSummary += [PSCustomObject]@{
        Group = $Group
        Runs = $Rows.Count
        AverageFps = [Math]::Round((Get-Average @($Rows.AverageFps)), 2)
        FrameAverageMs = [Math]::Round((Get-Average @($Rows.FrameAverageMs)), 3)
        FrameP95Ms = [Math]::Round((Get-Average @($Rows.FrameP95Ms)), 3)
        FrameP99Ms = [Math]::Round((Get-Average @($Rows.FrameP99Ms)), 3)
        GameThreadMs = [Math]::Round((Get-Average @($Rows.GameThreadMs)), 3)
        RenderThreadMs = [Math]::Round((Get-Average @($Rows.RenderThreadMs)), 3)
        RHIThreadMs = [Math]::Round((Get-Average @($Rows.RHIThreadMs)), 3)
        GpuMs = [Math]::Round((Get-Average @($Rows.GpuMs)), 3)
        SkinnedGeometryBuildBLASMs = [Math]::Round((Get-Average @($Rows.SkinnedGeometryBuildBLASMs)), 3)
        RayTracingDynamicGeometryMs = [Math]::Round((Get-Average @($Rows.RayTracingDynamicGeometryMs)), 3)
        RayTracingSceneMs = [Math]::Round((Get-Average @($Rows.RayTracingSceneMs)), 3)
        ShadowDepthsMs = [Math]::Round((Get-Average @($Rows.ShadowDepthsMs)), 3)
        ShadowProjectionMs = [Math]::Round((Get-Average @($Rows.ShadowProjectionMs)), 3)
        DrawCalls = [Math]::Round((Get-Average @($Rows.DrawCalls)), 1)
        ShadowDrawCalls = [Math]::Round((Get-Average @($Rows.ShadowDrawCalls)), 1)
        CharacterMovementMs = [Math]::Round((Get-Average @($Rows.CharacterMovementMs)), 3)
        CharacterMovementTicks = [Math]::Round((Get-Average @($Rows.CharacterMovementTicks)), 1)
        AnimationMs = [Math]::Round((Get-Average @($Rows.AnimationMs)), 3)
        AIDecisionMs = [Math]::Round((Get-Average @($Rows.AIDecisionMs)), 3)
        AIDecisionsPerFrame = [Math]::Round((Get-Average @($Rows.AIDecisionsPerFrame)), 3)
        MoveRequestsPerFrame = [Math]::Round((Get-Average @($Rows.MoveRequestsPerFrame)), 3)
        SignificanceUpdateMs = [Math]::Round((Get-Average @($Rows.SignificanceUpdateMs)), 4)
        LOD0Enemies = [Math]::Round((Get-Average @($Rows.LOD0Enemies)), 1)
        LOD1Enemies = [Math]::Round((Get-Average @($Rows.LOD1Enemies)), 1)
        LOD2PlusEnemies = [Math]::Round((Get-Average @($Rows.LOD2PlusEnemies)), 1)
        ShadowCasters = [Math]::Round((Get-Average @($Rows.ShadowCasters)), 1)
        RayTracingVisible = [Math]::Round((Get-Average @($Rows.RayTracingVisible)), 1)
        FullBudgetDowngrades = [Math]::Round((Get-Average @($Rows.FullBudgetDowngrades)), 1)
        ShadowBudgetRejected = [Math]::Round((Get-Average @($Rows.ShadowBudgetRejected)), 1)
        RayTracingBudgetRejected = [Math]::Round((Get-Average @($Rows.RayTracingBudgetRejected)), 1)
        AnimationSharingFollowers = [Math]::Round((Get-Average @($Rows.AnimationSharingFollowers)), 1)
        AverageEnemyActors = [Math]::Round((Get-Average @($Rows.AverageEnemyActors)), 1)
    }
}

$AllEnabled = $GroupSummary | Where-Object { $_.Group -eq "AllEnabled" }
$Comparison = @()
foreach ($Disabled in @($GroupSummary | Where-Object { $_.Group -ne "AllEnabled" })) {
    $Comparison += [PSCustomObject]@{
        DisabledGroup = $Disabled.Group
        OptimizationFpsBenefit = [Math]::Round($AllEnabled.AverageFps - $Disabled.AverageFps, 2)
        OptimizationFrameBenefitMs = [Math]::Round($Disabled.FrameAverageMs - $AllEnabled.FrameAverageMs, 3)
        OptimizationFrameP95BenefitMs = [Math]::Round($Disabled.FrameP95Ms - $AllEnabled.FrameP95Ms, 3)
        OptimizationFrameP99BenefitMs = [Math]::Round($Disabled.FrameP99Ms - $AllEnabled.FrameP99Ms, 3)
        OptimizationGameThreadBenefitMs = [Math]::Round($Disabled.GameThreadMs - $AllEnabled.GameThreadMs, 3)
        OptimizationRenderThreadBenefitMs = [Math]::Round($Disabled.RenderThreadMs - $AllEnabled.RenderThreadMs, 3)
        OptimizationRHIThreadBenefitMs = [Math]::Round($Disabled.RHIThreadMs - $AllEnabled.RHIThreadMs, 3)
        OptimizationGpuBenefitMs = [Math]::Round($Disabled.GpuMs - $AllEnabled.GpuMs, 3)
        OptimizationSkinnedGeometryBuildBLASBenefitMs = [Math]::Round($Disabled.SkinnedGeometryBuildBLASMs - $AllEnabled.SkinnedGeometryBuildBLASMs, 3)
        OptimizationRayTracingDynamicGeometryBenefitMs = [Math]::Round($Disabled.RayTracingDynamicGeometryMs - $AllEnabled.RayTracingDynamicGeometryMs, 3)
        OptimizationRayTracingSceneBenefitMs = [Math]::Round($Disabled.RayTracingSceneMs - $AllEnabled.RayTracingSceneMs, 3)
        OptimizationShadowDepthsBenefitMs = [Math]::Round($Disabled.ShadowDepthsMs - $AllEnabled.ShadowDepthsMs, 3)
        OptimizationShadowProjectionBenefitMs = [Math]::Round($Disabled.ShadowProjectionMs - $AllEnabled.ShadowProjectionMs, 3)
        OptimizationDrawCallBenefit = [Math]::Round($Disabled.DrawCalls - $AllEnabled.DrawCalls, 1)
        OptimizationShadowDrawCallBenefit = [Math]::Round($Disabled.ShadowDrawCalls - $AllEnabled.ShadowDrawCalls, 1)
        OptimizationMovementBenefitMs = [Math]::Round($Disabled.CharacterMovementMs - $AllEnabled.CharacterMovementMs, 3)
        OptimizationAnimationBenefitMs = [Math]::Round($Disabled.AnimationMs - $AllEnabled.AnimationMs, 3)
        OptimizationAIDecisionBenefitMs = [Math]::Round($Disabled.AIDecisionMs - $AllEnabled.AIDecisionMs, 3)
    }
}

$RunSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "run_summary.csv") -NoTypeInformation -Encoding UTF8
$GroupSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "group_summary.csv") -NoTypeInformation -Encoding UTF8
$Comparison | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "comparison_vs_all_enabled.csv") -NoTypeInformation -Encoding UTF8
$GroupSummary | Format-Table -AutoSize
$Comparison | Format-Table -AutoSize
