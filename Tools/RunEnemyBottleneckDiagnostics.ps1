param(
    [int]$EnemyCount = 160,
    [int]$RunsPerGroup = 1,
    [double]$WarmupSeconds = 10,
    [double]$DurationSeconds = 30,
    [int]$BenchmarkSeed = 1337,
    [string]$RunName = "EnemyBottleneckDiagnostics_20260824",
    [switch]$MovementFollowup
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

$Groups = @(
    [PSCustomObject]@{ Name = "Baseline"; Flag = "" },
    [PSCustomObject]@{ Name = "AttackSweepOff"; Flag = "-BenchmarkDisableAttackSweep" },
    [PSCustomObject]@{ Name = "EnemyPawnCollisionOff"; Flag = "-BenchmarkDisableEnemyPawnCollision" },
    [PSCustomObject]@{ Name = "PathFollowingTickOff"; Flag = "-BenchmarkDisablePathFollowingTick" },
    [PSCustomObject]@{ Name = "CharacterMovementTickOff"; Flag = "-BenchmarkDisableCharacterMovementTick" }
)

if ($MovementFollowup) {
    $Groups = @($Groups | Where-Object {
        $_.Name -in @("Baseline", "PathFollowingTickOff", "CharacterMovementTickOff")
    })
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
            '-ExecCmds="stat unit,stat streaming"',
            "-log=$LogName",
            "-ddc=InstalledNoZenLocalFallback",
            "-LocalDataCachePath=$DdcPath"
        )
        if ($Group.Flag) {
            $Arguments += $Group.Flag
        }

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
        $DiagnosticsApplied = [bool]($LogLines | Select-String -SimpleMatch "Benchmark diagnostics applied: enemies=$EnemyCount")
        if (-not $Ready -or -not $Stopped -or -not $DiagnosticsApplied) {
            throw "$RunId did not reach a valid diagnostic capture state"
        }

        $ManifestRows += [PSCustomObject]@{
            Group = $Group.Name
            Run = $Run
            EnemyCount = $EnemyCount
            BenchmarkSeed = $BenchmarkSeed
            DisabledFlag = $Group.Flag
            Ready = $Ready
            DiagnosticsApplied = $DiagnosticsApplied
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

& (Join-Path $ProjectRoot "Tools\SummarizeEnemyBottleneckDiagnostics.ps1") -EvidenceRoot $EvidenceRoot
