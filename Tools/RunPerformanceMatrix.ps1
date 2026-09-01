param(
    [int[]]$Counts = @(20, 80, 160),
    [double]$WarmupSeconds = 10,
    [double]$DurationSeconds = 30,
    [int]$BenchmarkSeed = 1337,
    [string]$RunName = "CurrentScaleMatrix_20260830"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "E:\ueprojrct\fpstrue_safe2"
$Editor = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = Join-Path $ProjectRoot "fpstrue.uproject"
$Map = "/Game/FactoryDistrict/Maps/Demonstration"
$DdcPath = Join-Path $ProjectRoot "Saved\DerivedDataCache"
$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
$CsvOutput = Join-Path $EvidenceRoot "CSV"
$ScreenshotOutput = Join-Path $EvidenceRoot "Screenshots"
$LogOutput = Join-Path $EvidenceRoot "Logs"
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"

New-Item -ItemType Directory -Path $CsvOutput -Force | Out-Null
New-Item -ItemType Directory -Path $ScreenshotOutput -Force | Out-Null
New-Item -ItemType Directory -Path $LogOutput -Force | Out-Null

$Results = @()

foreach ($Count in $Counts) {
    Write-Output "Starting benchmark: $Count enemies"
    $StartedAt = Get-Date
    $LogName = "Benchmark_${RunName}_$Count.log"
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
        "-BenchmarkEnemies=$Count",
        "-BenchmarkWarmup=$WarmupSeconds",
        "-BenchmarkDuration=$DurationSeconds",
        "-BenchmarkSeed=$BenchmarkSeed",
        "-BenchmarkScreenshot",
        "-BenchmarkAutoQuit",
        "-csvGpuStats",
        '-ExecCmds="stat unit,stat streaming"',
        "-log=$LogName",
        "-ddc=NoZenLocalFallback",
        "-LocalDataCachePath=$DdcPath",
        "-ShaderWorkingDir=Saved/ShaderWorkingDir"
    )

    $Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    if ($Process.ExitCode -ne 0) {
        throw "Benchmark process failed for $Count enemies with exit code $($Process.ExitCode)"
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
        throw "Benchmark evidence is incomplete for $Count enemies"
    }

    $NamedCsv = Join-Path $CsvOutput "FinalLOD_${Count}.csv"
    $NamedScreenshot = Join-Path $ScreenshotOutput "FinalLOD_${Count}.png"
    $NamedLog = Join-Path $LogOutput $LogName
    Copy-Item -LiteralPath $Csv.FullName -Destination $NamedCsv -Force
    Copy-Item -LiteralPath $Screenshot.FullName -Destination $NamedScreenshot -Force
    Copy-Item -LiteralPath $Log.FullName -Destination $NamedLog -Force

    $LogLines = Get-Content -LiteralPath $Log.FullName
    $ReadyPattern = "Automated benchmark ready: requested=$Count alive=$Count"
    $Ready = [bool]($LogLines | Select-String -SimpleMatch $ReadyPattern)
    $Stopped = [bool]($LogLines | Select-String -SimpleMatch "Automated benchmark capture stopped.")
    if (-not $Ready -or -not $Stopped) {
        throw "Benchmark did not reach a valid capture state for $Count enemies"
    }

    $Results += [PSCustomObject]@{
        EnemyCount = $Count
        Ready = $Ready
        CaptureStopped = $Stopped
        SpawnFailures = ($LogLines | Select-String -SimpleMatch "SpawnActor failed for enemy class").Count
        VSMQueueOverflows = ($LogLines | Select-String -SimpleMatch "Non-Nanite Marking Job Queue overflow").Count
        TexturePoolWarnings = ($LogLines | Select-String -Pattern "Texture streaming pool.*over budget").Count
        Csv = $NamedCsv
        Screenshot = $NamedScreenshot
        Log = $NamedLog
    }

    Write-Output "Completed benchmark: $Count enemies"
}

$Manifest = Join-Path $EvidenceRoot "manifest.csv"
$Results | Export-Csv -LiteralPath $Manifest -NoTypeInformation -Encoding UTF8
$Results | Format-Table -AutoSize
& (Join-Path $ProjectRoot "Tools\SummarizePerformanceMatrix.ps1") -EvidenceRoot $EvidenceRoot
