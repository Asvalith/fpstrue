param(
    [int]$RunsPerGroup = 3,
    [double]$WarmupSeconds = 10,
    [double]$DurationSeconds = 30,
    [int]$BenchmarkSeed = 1337,
    [string]$RunName = "EnemyContributionAB_20260828_CurrentSignificance"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "E:\ueprojrct\fpstrue_safe2"
$Editor = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = Join-Path $ProjectRoot "fpstrue.uproject"
$Map = "/Game/FactoryDistrict/Maps/Demonstration"
$DdcPath = "E:\ueprojrct\ddc"
$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"
$EnemyCounts = @(0, 20)
$ExecCmds = "r.RayTracing.ForceAllRayTracingEffects -1,r.RayTracing.Geometry.SkeletalMeshes 1"

if (Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue) {
    throw "Close the running Unreal Editor before the A/B test. A second editor process would invalidate the result."
}

function Get-NumericValues {
    param([object[]]$Rows, [string]$Column)

    $Values = foreach ($Row in $Rows) {
        $Value = 0.0
        if ([double]::TryParse(
                [string]$Row.$Column,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$Value)) {
            $Value
        }
    }
    return @($Values)
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)

    if ($Values.Count -eq 0) {
        return 0.0
    }
    $Sorted = @($Values | Sort-Object)
    $Index = [Math]::Floor(($Sorted.Count - 1) * $Percentile)
    return $Sorted[$Index]
}

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$ManifestRows = @()

for ($Run = 1; $Run -le $RunsPerGroup; ++$Run) {
    $RunCounts = @($EnemyCounts | Sort-Object { Get-Random })

    foreach ($EnemyCount in $RunCounts) {
        $GroupName = if ($EnemyCount -eq 0) { "EnvironmentOnly" } else { "Enemies20" }
        $RunId = "${GroupName}_Run${Run}"
        $RunRoot = Join-Path $EvidenceRoot $RunId
        New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null

        $StartedAt = Get-Date
        $LogName = "Benchmark_${RunName}_${RunId}.log"
        $Arguments = @(
            $Project,
            $Map,
            "-game",
            "-windowed",
            "-ResX=1600",
            "-ResY=900",
            "-NoVSync",
            "-NoSplash",
            "-NoSound",
            "-NoLiveCoding",
            "-Unattended",
            "-RenderOffscreen",
            "-AutoBenchmark",
            "-BenchmarkEnemies=$EnemyCount",
            "-BenchmarkWarmup=$WarmupSeconds",
            "-BenchmarkDuration=$DurationSeconds",
            "-BenchmarkSeed=$BenchmarkSeed",
            "-BenchmarkScreenshot",
            "-BenchmarkAutoQuit",
            "-csvGpuStats",
            "-ExecCmds=`"$ExecCmds`"",
            "-log=$LogName",
            "-ddc=InstalledNoZenLocalFallback",
            "-LocalDataCachePath=$DdcPath"
        )

        Write-Output "Starting $RunId"
        $Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
        if ($Process.ExitCode -ne 0) {
            throw "$RunId failed with exit code $($Process.ExitCode)"
        }

        $Csv = Get-ChildItem -LiteralPath $SourceCsv -File -Filter "*.csv" |
            Where-Object { $_.LastWriteTime -ge $StartedAt } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        $Screenshot = Get-ChildItem -LiteralPath $SourceScreenshots -File -Filter "*.png" |
            Where-Object { $_.LastWriteTime -ge $StartedAt } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        $Log = Get-Item -LiteralPath (Join-Path $SourceLogs $LogName)

        if (-not $Csv -or -not $Screenshot -or -not $Log) {
            throw "$RunId evidence is incomplete"
        }

        $CsvDestination = Join-Path $RunRoot "$RunId.csv"
        $ScreenshotDestination = Join-Path $RunRoot "$RunId.png"
        $LogDestination = Join-Path $RunRoot $LogName
        Copy-Item -LiteralPath $Csv.FullName -Destination $CsvDestination -Force
        Copy-Item -LiteralPath $Screenshot.FullName -Destination $ScreenshotDestination -Force
        Copy-Item -LiteralPath $Log.FullName -Destination $LogDestination -Force

        $LogLines = Get-Content -LiteralPath $Log.FullName
        $Ready = [bool]($LogLines | Select-String -SimpleMatch "Automated benchmark ready: requested=$EnemyCount alive=$EnemyCount")
        $Stopped = [bool]($LogLines | Select-String -SimpleMatch "Automated benchmark capture stopped.")
        if (-not $Ready -or -not $Stopped) {
            throw "$RunId did not reach a valid capture state"
        }

        $ManifestRows += [PSCustomObject]@{
            Group = $GroupName
            Run = $Run
            EnemyCount = $EnemyCount
            BenchmarkSeed = $BenchmarkSeed
            WarmupSeconds = $WarmupSeconds
            DurationSeconds = $DurationSeconds
            ExecCmds = $ExecCmds
            Ready = $Ready
            CaptureStopped = $Stopped
            SpawnFailures = ($LogLines | Select-String -SimpleMatch "SpawnActor failed for enemy class").Count
            VSMQueueOverflows = ($LogLines | Select-String -SimpleMatch "Non-Nanite Marking Job Queue overflow").Count
            TexturePoolWarnings = ($LogLines | Select-String -Pattern "Texture streaming pool.*over budget").Count
            Csv = $CsvDestination
            Screenshot = $ScreenshotDestination
            Log = $LogDestination
        }
        $ManifestRows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv") -NoTypeInformation -Encoding UTF8
        Write-Output "Completed $RunId"
    }
}

$MetricNames = @(
    "FrameTime",
    "GameThreadTime",
    "RenderThreadTime",
    "RHIThreadTime",
    "GPUTime",
    "Exclusive/RenderThread/EventWait/Visibility",
    "Exclusive/RenderThread/RenderOther",
    "Exclusive/RenderThread/RenderLighting",
    "Exclusive/RenderThread/RenderShadows",
    "GPU/RayTracingDynamicGeometry",
    "GPU/RayTracingScene",
    "GPU/ShadowDepths",
    "GPU/ShadowProjection",
    "DrawCall/ShadowDepths",
    "DrawCall/Basepass",
    "DrawCall/Prepass",
    "DrawCall/Lights",
    "RHI/PrimitivesDrawn",
    "RHI/DrawCalls",
    "Exclusive/GameThread/CharacterMovement",
    "Exclusive/GameThread/Animation",
    "Exclusive/GameThread/EventWait/EndPhysics",
    "Exclusive/GameThread/SyncBodies",
    "fpstrueAI/GameThread/DecisionTime",
    "fpstrueSignificance/GameThread/UpdateTime",
    "fpstrueSignificance/GameplayFull",
    "fpstrueSignificance/GameplayReduced",
    "fpstrueSignificance/GameplayBackground",
    "fpstrueSignificance/RenderFull",
    "fpstrueSignificance/RenderReduced",
    "fpstrueSignificance/RenderBackground",
    "fpstrueSignificance/LOD0",
    "fpstrueSignificance/LOD1",
    "fpstrueSignificance/LOD2Plus",
    "fpstrueSignificance/ShadowCasters",
    "fpstrueSignificance/RayTracingVisible",
    "fpstrueSignificance/AnimationSharingFollowers",
    "fpstrueSignificance/GameplayAnimationProtection",
    "fpstrueSignificance/FullBudgetDowngrades",
    "fpstrueSignificance/ShadowBudgetRejected",
    "fpstrueSignificance/RayTracingBudgetRejected",
    "fpstrueSignificance/MeanScore",
    "fpstrueSignificance/MeanFrustumFactor",
    "fpstrueSignificance/MeanScreenFactor",
    "fpstrueSignificance/MeanRecentFactor",
    "fpstrueSignificance/MeanDistanceFactor",
    "ActorCount/fpstrueEnemyCharacter",
    "ActorCount/fpstrueEnemyAIController",
    "MemoryFreeMB"
)

$SummaryRows = @()
foreach ($ManifestRow in $ManifestRows) {
    $Rows = @(Import-Csv -LiteralPath $ManifestRow.Csv)
    foreach ($MetricName in $MetricNames) {
        $Values = @(Get-NumericValues -Rows $Rows -Column $MetricName)
        if ($Values.Count -eq 0) {
            continue
        }
        $SummaryRows += [PSCustomObject]@{
            Group = $ManifestRow.Group
            Run = $ManifestRow.Run
            Metric = $MetricName
            Samples = $Values.Count
            Average = ($Values | Measure-Object -Average).Average
            P95 = Get-Percentile -Values $Values -Percentile 0.95
            P99 = Get-Percentile -Values $Values -Percentile 0.99
            Maximum = ($Values | Measure-Object -Maximum).Maximum
        }
    }
}

$SummaryRows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "summary.csv") -NoTypeInformation -Encoding UTF8
Write-Output "Enemy contribution A/B evidence is ready in $EvidenceRoot"
