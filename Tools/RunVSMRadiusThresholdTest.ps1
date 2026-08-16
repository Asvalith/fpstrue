param(
    [int]$EnemyCount = 80
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "E:\ueprojrct\fpstrue_safe2"
$Editor = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = Join-Path $ProjectRoot "fpstrue.uproject"
$Map = "/Game/FactoryDistrict/Maps/Demonstration"
$DdcPath = "E:\ueprojrct\ddc"
$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\VSM_RadiusThreshold_20260816"
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"
$Thresholds = @(0.02, 0.03)

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$Results = @()

foreach ($Threshold in $Thresholds) {
    $Label = $Threshold.ToString("0.00", [System.Globalization.CultureInfo]::InvariantCulture).Replace(".", "_")
    $Name = "RadiusThreshold$Label"
    Write-Output "Starting VSM test: $Name"
    $StartedAt = Get-Date
    $LogName = "VSM_$Name.log"
    $CVarValue = $Threshold.ToString([System.Globalization.CultureInfo]::InvariantCulture)
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
        "-BenchmarkWarmup=10",
        "-BenchmarkDuration=30",
        "-BenchmarkScreenshot",
        "-BenchmarkAutoQuit",
        "-csvGpuStats",
        "-ExecCmds=`"r.Shadow.RadiusThreshold=$CVarValue,stat unit,stat streaming`"",
        "-log=$LogName",
        "-ddc=InstalledNoZenLocalFallback",
        "-LocalDataCachePath=$DdcPath"
    )

    $Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    if ($Process.ExitCode -ne 0) {
        throw "VSM test failed for $Name with exit code $($Process.ExitCode)"
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
        throw "VSM evidence is incomplete for $Name"
    }

    $NamedCsv = Join-Path $EvidenceRoot "$Name.csv"
    $NamedScreenshot = Join-Path $EvidenceRoot "$Name.png"
    $NamedLog = Join-Path $EvidenceRoot $LogName
    Copy-Item -LiteralPath $Csv.FullName -Destination $NamedCsv -Force
    Copy-Item -LiteralPath $Screenshot.FullName -Destination $NamedScreenshot -Force
    Copy-Item -LiteralPath $Log.FullName -Destination $NamedLog -Force

    $LogLines = Get-Content -LiteralPath $Log.FullName
    $Ready = [bool]($LogLines | Select-String -SimpleMatch "Automated benchmark ready: requested=$EnemyCount alive=$EnemyCount")
    $Stopped = [bool]($LogLines | Select-String -SimpleMatch "Automated benchmark capture stopped.")
    if (-not $Ready -or -not $Stopped) {
        throw "VSM test did not reach a valid capture state for $Name"
    }

    $Results += [PSCustomObject]@{
        RadiusThreshold = $CVarValue
        EnemyCount = $EnemyCount
        VSMQueueOverflows = ($LogLines | Select-String -SimpleMatch "Non-Nanite Marking Job Queue overflow").Count
        TexturePoolWarnings = ($LogLines | Select-String -Pattern "Texture streaming pool.*over budget").Count
        MovementStepWarnings = ($LogLines | Select-String -SimpleMatch "Max iterations 8 hit").Count
        Csv = $NamedCsv
        Screenshot = $NamedScreenshot
        Log = $NamedLog
    }

    Write-Output "Completed VSM test: $Name"
}

$Results | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv") -NoTypeInformation -Encoding UTF8
$Results | Format-Table -AutoSize
