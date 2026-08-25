param(
    [int]$EnemyCount = 160,
    [double]$WarmupSeconds = 10,
    [double]$TraceSeconds = 15,
    [int]$BenchmarkSeed = 1337,
    [string]$RunName = "InsightsFirstDiagnosis_20260824"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "E:\ueprojrct\fpstrue_safe2"
$Editor = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = Join-Path $ProjectRoot "fpstrue.uproject"
$Map = "/Game/FactoryDistrict/Maps/Demonstration"
$DdcPath = "E:\ueprojrct\ddc"
$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
$TraceFile = Join-Path $EvidenceRoot "Baseline.utrace"
$LogName = "Benchmark_${RunName}.log"
$LogFile = Join-Path $ProjectRoot "Saved\Logs\$LogName"
$ExportDirectory = Join-Path $EvidenceRoot "InsightsExport"

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$StartedAt = Get-Date

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
    "-BenchmarkDuration=$TraceSeconds",
    "-BenchmarkSeed=$BenchmarkSeed",
    "-BenchmarkTraceFile=$TraceFile",
    "-BenchmarkScreenshot",
    "-BenchmarkAutoQuit",
    "-csvGpuStats",
    '-ExecCmds="stat unit"',
    "-log=$LogName",
    "-ddc=InstalledNoZenLocalFallback",
    "-LocalDataCachePath=$DdcPath"
)

Write-Output "Recording $TraceSeconds second Insights baseline with $EnemyCount enemies"
$Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
if ($Process.ExitCode -ne 0) {
    throw "Benchmark failed with exit code $($Process.ExitCode)"
}

if (-not (Test-Path -LiteralPath $TraceFile) -or -not (Test-Path -LiteralPath $LogFile)) {
    throw "Trace or benchmark log is missing"
}

$LogLines = Get-Content -LiteralPath $LogFile
$RequiredMarkers = @(
    "Automated benchmark ready: requested=$EnemyCount alive=$EnemyCount",
    "Automated benchmark Insights trace started:",
    "Automated benchmark Insights trace stopped.",
    "Automated benchmark capture stopped."
)
foreach ($Marker in $RequiredMarkers) {
    if (-not ($LogLines | Select-String -SimpleMatch $Marker)) {
        throw "Benchmark did not reach a valid capture state: missing '$Marker'"
    }
}

$Csv = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "Saved\Profiling\CSV") -File -Filter "*.csv" |
    Where-Object { $_.LastWriteTime -ge $StartedAt } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
$Screenshot = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor") -File -Filter "*.png" |
    Where-Object { $_.LastWriteTime -ge $StartedAt } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($Csv) {
    Copy-Item -LiteralPath $Csv.FullName -Destination (Join-Path $EvidenceRoot "Baseline.csv") -Force
}
if ($Screenshot) {
    Copy-Item -LiteralPath $Screenshot.FullName -Destination (Join-Path $EvidenceRoot "Baseline.png") -Force
}
Copy-Item -LiteralPath $LogFile -Destination (Join-Path $EvidenceRoot $LogName) -Force

& (Join-Path $ProjectRoot "Tools\ExportInsightsTrace.ps1") -TraceFile $TraceFile -OutputDirectory $ExportDirectory
Write-Output "Baseline trace and exports are ready in $EvidenceRoot"
