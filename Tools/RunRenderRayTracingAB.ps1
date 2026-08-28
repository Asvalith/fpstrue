param(
    [int]$EnemyCount = 20,
    [int]$RunsPerGroup = 2,
    [double]$WarmupSeconds = 5,
    [double]$DurationSeconds = 5,
    [int]$BenchmarkSeed = 1337,
    [ValidateSet("RayTracingEffects", "DynamicShadows")]
    [string]$TestMode = "RayTracingEffects",
    [string]$RunName = "RenderRayTracingAB_20260826"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "E:\ueprojrct\fpstrue_safe2"
$Editor = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = Join-Path $ProjectRoot "fpstrue.uproject"
$Map = "/Game/FactoryDistrict/Maps/Demonstration"
$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"

if (Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue) {
    throw "Close the running Unreal Editor before the A/B test. A second editor process would invalidate the result."
}

if ($TestMode -eq "DynamicShadows") {
    $Groups = @(
        [PSCustomObject]@{
            Name = "Baseline"
            ExecCmds = "showflag.DynamicShadows 1"
        },
        [PSCustomObject]@{
            Name = "DynamicShadowsOff"
            ExecCmds = "showflag.DynamicShadows 0"
        }
    )
}
else {
    $Groups = @(
        [PSCustomObject]@{
            Name = "Baseline"
            ExecCmds = "r.RayTracing.ForceAllRayTracingEffects -1"
        },
        [PSCustomObject]@{
            Name = "RayTracingEffectsOff"
            ExecCmds = "r.RayTracing.ForceAllRayTracingEffects 0"
        }
    )
}

function Get-NumericValues {
    param(
        [object[]]$Rows,
        [string]$Column
    )

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
    param(
        [double[]]$Values,
        [double]$Percentile
    )

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
    $RunGroups = @($Groups | Sort-Object { Get-Random })

    foreach ($Group in $RunGroups) {
        $RunId = "$($Group.Name)_Run$Run"
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
            "-ExecCmds=`"$($Group.ExecCmds)`"",
            "-log=$LogName"
        )

        Write-Output "Starting $RunId with $EnemyCount enemies"
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
            Group = $Group.Name
            Run = $Run
            EnemyCount = $EnemyCount
            BenchmarkSeed = $BenchmarkSeed
            ExecCmds = $Group.ExecCmds
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
    "GPUTime",
    "Exclusive/GameThread/Input",
    "Exclusive/RenderThread/EventWait/Visibility",
    "Exclusive/RenderThread/RenderOther",
    "GPU/RayTracingDynamicGeometry",
    "GPU/RayTracingScene",
    "RHI/DrawCalls"
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
            Average = ($Values | Measure-Object -Average).Average
            P95 = Get-Percentile -Values $Values -Percentile 0.95
            P99 = Get-Percentile -Values $Values -Percentile 0.99
            Maximum = ($Values | Measure-Object -Maximum).Maximum
        }
    }
}

$SummaryPath = Join-Path $EvidenceRoot "summary.csv"
$SummaryRows | Export-Csv -LiteralPath $SummaryPath -NoTypeInformation -Encoding UTF8
Write-Output "Render A/B evidence is ready in $EvidenceRoot"
Write-Output "Summary: $SummaryPath"
