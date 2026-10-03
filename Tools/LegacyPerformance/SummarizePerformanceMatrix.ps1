param(
    [string]$EvidenceRoot = "E:\ueprojrct\fpstrue_safe2\Saved\Profiling\FPS_FinalLOD_20260816"
)

$ErrorActionPreference = "Stop"

function Get-NumericValues {
    param(
        [object[]]$Rows,
        [string]$Column
    )

    $Values = New-Object System.Collections.Generic.List[double]
    foreach ($Row in $Rows) {
        $Value = 0.0
        $Text = $Row.$Column
        if ([double]::TryParse(
            $Text,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$Value
        )) {
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
    return Get-Average @($Values | Where-Object { $_ -gt 0.0 })
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )
    if ($Values.Count -eq 0) { return 0.0 }
    $Sorted = $Values | Sort-Object
    $Index = [Math]::Ceiling($Percentile * $Sorted.Count) - 1
    $Index = [Math]::Max(0, [Math]::Min($Index, $Sorted.Count - 1))
    return $Sorted[$Index]
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

$CsvRoot = Join-Path $EvidenceRoot "CSV"
$ManifestPath = Join-Path $EvidenceRoot "manifest.csv"
$Counts = @(Import-Csv -LiteralPath $ManifestPath | ForEach-Object { [int]$_.EnemyCount } | Sort-Object -Unique)
$Summary = @()

foreach ($Count in $Counts) {
    $CsvPath = Join-Path $CsvRoot "FinalLOD_${Count}.csv"
    $Rows = @(Import-CsvWithUniqueHeaders -Path $CsvPath | Where-Object {
        $FrameValue = 0.0
        [double]::TryParse(
            $_.FrameTime,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$FrameValue
        ) -and $FrameValue -gt 0.0
    })

    $Frame = Get-NumericValues $Rows "FrameTime"
    $Game = Get-NumericValues $Rows "GameThreadTime"
    $Render = Get-NumericValues $Rows "RenderThreadTime"
    $Rhi = Get-NumericValues $Rows "RHIThreadTime"
    $Gpu = Get-NumericValues $Rows "GPUTime"
    $VisibilityWait = Get-NumericValues $Rows "Exclusive/RenderThread/EventWait/Visibility"
    $RenderOther = Get-NumericValues $Rows "Exclusive/RenderThread/RenderOther"
    $RayTracingDynamicGeometry = Get-NumericValues $Rows "GPU/RayTracingDynamicGeometry"
    $RayTracingScene = Get-NumericValues $Rows "GPU/RayTracingScene"
    $ShadowDepths = Get-NumericValues $Rows "GPU/ShadowDepths"
    $Movement = Get-NumericValues $Rows "Exclusive/GameThread/CharacterMovement"
    $Animation = Get-NumericValues $Rows "Exclusive/GameThread/Animation"
    $TickActors = Get-NumericValues $Rows "Exclusive/GameThread/TickActors"
    $Decision = Get-NumericValues $Rows "fpstrueAI/GameThread/DecisionTime"
    $DecisionCount = Get-NumericValues $Rows "fpstrueAI/DecisionCount"
    $MoveRequestCount = Get-NumericValues $Rows "fpstrueAI/MoveRequestCount"
    $DrawCalls = Get-NumericValues $Rows "RHI/DrawCalls"
    $GpuMemory = Get-NumericValues $Rows "GPUMem/LocalUsedMB"
    $PhysicalMemory = Get-NumericValues $Rows "PhysicalUsedMB"
    $WantedMips = Get-NumericValues $Rows "TextureStreaming/WantedMips"
    $EnemyActors = Get-NumericValues $Rows "ActorCount/fpstrueEnemyCharacter"
    $SignificanceUpdate = Get-NumericValues $Rows "fpstrueSignificance/GameThread/UpdateTime"
    $RenderFull = Get-NumericValues $Rows "fpstrueSignificance/RenderFull"
    $RenderReduced = Get-NumericValues $Rows "fpstrueSignificance/RenderReduced"
    $RenderBackground = Get-NumericValues $Rows "fpstrueSignificance/RenderBackground"
    $Lod0 = Get-NumericValues $Rows "fpstrueSignificance/LOD0"
    $Lod1 = Get-NumericValues $Rows "fpstrueSignificance/LOD1"
    $Lod2Plus = Get-NumericValues $Rows "fpstrueSignificance/LOD2Plus"
    $ShadowCasters = Get-NumericValues $Rows "fpstrueSignificance/ShadowCasters"
    $RayTracingVisible = Get-NumericValues $Rows "fpstrueSignificance/RayTracingVisible"
    $AnimationSharingFollowers = Get-NumericValues $Rows "fpstrueSignificance/AnimationSharingFollowers"

    $FrameAverage = Get-Average $Frame
    $Summary += [PSCustomObject]@{
        EnemyCount = $Count
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
        GameThreadMs = [Math]::Round((Get-Average $Game), 3)
        RenderThreadMs = [Math]::Round((Get-Average $Render), 3)
        RHIThreadMs = [Math]::Round((Get-Average $Rhi), 3)
        GpuMs = [Math]::Round((Get-Average $Gpu), 3)
        VisibilityWaitMs = [Math]::Round((Get-Average $VisibilityWait), 3)
        RenderOtherMs = [Math]::Round((Get-Average $RenderOther), 3)
        RayTracingDynamicGeometryMs = [Math]::Round((Get-Average $RayTracingDynamicGeometry), 3)
        RayTracingSceneMs = [Math]::Round((Get-Average $RayTracingScene), 3)
        ShadowDepthsMs = [Math]::Round((Get-Average $ShadowDepths), 3)
        CharacterMovementMs = [Math]::Round((Get-Average $Movement), 3)
        AnimationMs = [Math]::Round((Get-Average $Animation), 3)
        TickActorsMs = [Math]::Round((Get-Average $TickActors), 3)
        AIDecisionMs = [Math]::Round((Get-Average $Decision), 3)
        AIDecisionsPerFrame = [Math]::Round((Get-Average $DecisionCount), 3)
        MoveRequestsPerFrame = [Math]::Round((Get-Average $MoveRequestCount), 3)
        DrawCalls = [Math]::Round((Get-Average $DrawCalls), 0)
        GpuMemoryMB = [Math]::Round((Get-Average $GpuMemory), 0)
        PhysicalMemoryMB = [Math]::Round((Get-Average $PhysicalMemory), 0)
        WantedMipsMB = [Math]::Round((Get-Average $WantedMips), 1)
        AverageEnemyActors = [Math]::Round((Get-Average $EnemyActors), 1)
        SignificanceUpdateMs = [Math]::Round((Get-AveragePositive $SignificanceUpdate), 4)
        RenderFull = [Math]::Round((Get-AveragePositive $RenderFull), 1)
        RenderReduced = [Math]::Round((Get-AveragePositive $RenderReduced), 1)
        RenderBackground = [Math]::Round((Get-AveragePositive $RenderBackground), 1)
        LOD0 = [Math]::Round((Get-AveragePositive $Lod0), 1)
        LOD1 = [Math]::Round((Get-AveragePositive $Lod1), 1)
        LOD2Plus = [Math]::Round((Get-AveragePositive $Lod2Plus), 1)
        ShadowCasters = [Math]::Round((Get-AveragePositive $ShadowCasters), 1)
        RayTracingVisible = [Math]::Round((Get-AveragePositive $RayTracingVisible), 1)
        AnimationSharingFollowers = [Math]::Round((Get-AveragePositive $AnimationSharingFollowers), 1)
    }
}

$SummaryCsv = Join-Path $EvidenceRoot "summary.csv"
$SummaryMarkdown = Join-Path $EvidenceRoot "summary.md"
$Summary | Export-Csv -LiteralPath $SummaryCsv -NoTypeInformation -Encoding UTF8

$Lines = New-Object System.Collections.Generic.List[string]
$Lines.Add("# Current Multi-Enemy Performance Matrix")
$Lines.Add("")
$Lines.Add("| Enemies | Samples | FPS | Frame Avg | P95 | P99 | GT | RT | GPU | Movement | Animation | Draw Calls |")
$Lines.Add("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($Row in $Summary) {
    $Lines.Add("| $($Row.EnemyCount) | $($Row.Samples) | $($Row.AverageFps) | $($Row.FrameAverageMs) ms | $($Row.FrameP95Ms) ms | $($Row.FrameP99Ms) ms | $($Row.GameThreadMs) ms | $($Row.RenderThreadMs) ms | $($Row.GpuMs) ms | $($Row.CharacterMovementMs) ms | $($Row.AnimationMs) ms | $($Row.DrawCalls) |")
}
$Lines | Set-Content -LiteralPath $SummaryMarkdown -Encoding UTF8

$Summary | Format-Table -AutoSize
