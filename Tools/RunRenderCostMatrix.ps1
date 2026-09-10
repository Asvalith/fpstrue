param(
    [int[]]$Counts = @(20, 50, 100, 160),
    [int]$RunsPerCase = 3,
    [double]$WarmupSeconds = 15,
    [double]$DurationSeconds = 30,
    [double]$PlayerHealth = 1000000,
    [int]$BenchmarkSeed = 1337,
    [ValidateSet("Baseline", "EnemyRayTracingOff", "EnemyShadowsOff", "OcclusionQueriesOn", "OcclusionQueriesOff", "HardwareQueries", "HZBOcclusion", "BufferedQueries2")]
    [string[]]$VariantNames = @("Baseline", "EnemyRayTracingOff", "EnemyShadowsOff"),
    [ValidateSet("Any", "RVO", "DetourCrowd")]
    [string]$ExpectedAvoidanceMode = "Any",
    [switch]$CaptureTaskTrace,
    [switch]$BalancedVariantOrder,
    [double]$StartTrimSeconds = 0,
    [double]$EndTrimSeconds = 0,
    [string]$RunName = "",
    [string]$ProjectRoot = "",
    [string]$EditorPath = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe",
    [string]$Map = "/Game/FactoryDistrict/Maps/Demonstration",
    [string]$DdcPath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path $PSScriptRoot -Parent
}
if ([string]::IsNullOrWhiteSpace($DdcPath)) {
    $DdcPath = Join-Path $ProjectRoot "Saved\DerivedDataCache"
}
if ([string]::IsNullOrWhiteSpace($RunName)) {
    $RunName = "RenderCostMatrix_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss")
}

$Counts = @($Counts | Sort-Object -Unique)
if ($Counts.Count -eq 0 -or @($Counts | Where-Object { $_ -lt 0 }).Count -gt 0) {
    throw "Counts must contain at least one non-negative enemy count. Zero keeps the normal world running without spawning enemies."
}
if ($RunsPerCase -lt 1) {
    throw "RunsPerCase must be at least 1."
}
if ($WarmupSeconds -lt 0 -or $DurationSeconds -lt 1 -or $PlayerHealth -lt 1) {
    throw "Warmup, duration or player health is outside the valid range."
}
if ($StartTrimSeconds -lt 0 -or $EndTrimSeconds -lt 0 -or
    $StartTrimSeconds + $EndTrimSeconds -ge $DurationSeconds) {
    throw "Summary edge trimming must leave a positive capture interval."
}

$Project = Join-Path $ProjectRoot "fpstrue.uproject"
if (-not (Test-Path -LiteralPath $EditorPath)) {
    throw "Unreal Editor was not found: $EditorPath"
}
if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project was not found: $Project"
}
if (Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue) {
    throw "Close the running Unreal Editor before the matrix test; another editor process would invalidate the result."
}

$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "Choose a new RunName; existing experiment evidence must not be overwritten: $EvidenceRoot"
}
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"
$ExecCmds = "r.RayTracing.ForceAllRayTracingEffects -1,r.RayTracing.Geometry.SkeletalMeshes 1"
$VariantCatalog = @(
    [PSCustomObject]@{ Name = "Baseline"; Switch = ""; Exec = "" },
    [PSCustomObject]@{ Name = "EnemyRayTracingOff"; Switch = "-BenchmarkEnemyRayTracingOff"; Exec = "" },
    [PSCustomObject]@{ Name = "EnemyShadowsOff"; Switch = "-BenchmarkEnemyShadowsOff"; Exec = "" },
    # Process-local diagnosis only. Disabling culling may INCREASE draw/GPU costs;
    # this isolates the query dependency, not an approved production optimization.
    [PSCustomObject]@{ Name = "OcclusionQueriesOn"; Switch = ""; Exec = "r.AllowOcclusionQueries 1" },
    [PSCustomObject]@{ Name = "OcclusionQueriesOff"; Switch = ""; Exec = "r.AllowOcclusionQueries 0" },
    # Keep occlusion enabled in all three candidates. Each differs from the explicit
    # hardware control by ONE policy value; do not combine HZB and buffering changes.
    [PSCustomObject]@{ Name = "HardwareQueries"; Switch = ""; Exec = "r.AllowOcclusionQueries 1,r.HZBOcclusion 0,r.NumBufferedOcclusionQueries 1" },
    [PSCustomObject]@{ Name = "HZBOcclusion"; Switch = ""; Exec = "r.AllowOcclusionQueries 1,r.HZBOcclusion 1,r.NumBufferedOcclusionQueries 1" },
    [PSCustomObject]@{ Name = "BufferedQueries2"; Switch = ""; Exec = "r.AllowOcclusionQueries 1,r.HZBOcclusion 0,r.NumBufferedOcclusionQueries 2" }
)
$Variants = @($VariantCatalog | Where-Object { $VariantNames -contains $_.Name })
if ($Variants.Count -eq 0) {
    throw "VariantNames did not select any supported test variant."
}

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null

# Record immutable experiment inputs, rather than treating the branch name as the
# build identity. These hashes cover the tested binary, map and default config;
# they do not imply every referenced asset or machine condition was frozen.
$FingerprintPaths = @(
    (Join-Path $ProjectRoot "Binaries\Win64\UnrealEditor-fpstrue.dll"),
    (Join-Path $ProjectRoot "Content\FactoryDistrict\Maps\Demonstration.umap"),
    (Join-Path $ProjectRoot "Config\DefaultEngine.ini"),
    (Join-Path $ProjectRoot "Config\DefaultGame.ini"),
    (Join-Path $ProjectRoot "Config\DefaultScalability.ini")
) | Where-Object { Test-Path -LiteralPath $_ }
function Get-ExperimentFingerprint {
    @($FingerprintPaths | ForEach-Object {
        $Item = Get-Item -LiteralPath $_
        [PSCustomObject]@{ Path = $Item.FullName; Length = $Item.Length; SHA256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    })
}
function Get-CVarValue {
    param([string]$Text, [string]$Name)
    $Pattern = '(?im)' + [regex]::Escape($Name) + '\s*=\s*"?([^"\s]+)"?'
    $MatchesFound = [regex]::Matches($Text, $Pattern)
    if ($MatchesFound.Count -eq 0) { return $null }
    return $MatchesFound[$MatchesFound.Count - 1].Groups[1].Value
}
$InitialFingerprint = @(Get-ExperimentFingerprint)
$GpuCommand = Get-Command nvidia-smi -ErrorAction SilentlyContinue
function Get-GpuSnapshot {
    if ($null -eq $GpuCommand) { return "Unavailable" }
    return ((& $GpuCommand.Source --query-gpu=timestamp,name,driver_version,temperature.gpu,clocks.current.graphics,clocks.current.memory,pstate,power.draw,utilization.gpu,memory.used --format=csv,noheader,nounits 2>&1) -join "`n")
}
$EnvironmentRecord = [ordered]@{
    StartedAt = (Get-Date).ToString("o")
    EditorPath = $EditorPath
    Map = $Map
    Resolution = "1600x900"
    NoVSync = $true
    GitCommit = ((& git -C $ProjectRoot rev-parse HEAD) -join "`n")
    GitStatus = @(& git -C $ProjectRoot status --short)
    Inputs = $InitialFingerprint
    PowerScheme = ((& powercfg /getactivescheme) -join "`n")
    GpuSnapshot = Get-GpuSnapshot
    GpuSnapshotColumns = "timestamp,name,driver_version,temperature.gpu,clocks.current.graphics,clocks.current.memory,pstate,power.draw,utilization.gpu,memory.used"
    StartTrimSeconds = $StartTrimSeconds
    EndTrimSeconds = $EndTrimSeconds
    BalancedVariantOrder = [bool]$BalancedVariantOrder
    Note = "GPU snapshots are boundary observations, not continuous thermal stability proof. Default input hashes are checked around each run."
}
$EnvironmentRecord | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $EvidenceRoot "environment.json") -Encoding UTF8
$ManifestRows = @()
$OrderRandom = [System.Random]::new($BenchmarkSeed)
$BalancedVariants = @($Variants | Sort-Object { $OrderRandom.Next() })
$TotalCases = $Counts.Count * $Variants.Count * $RunsPerCase
$CompletedCases = 0

for ($Run = 1; $Run -le $RunsPerCase; ++$Run) {
    $RunCounts = @($Counts | Sort-Object { $OrderRandom.Next() })
    foreach ($Count in $RunCounts) {
        $RunVariants = if ($BalancedVariantOrder) {
            # Rotate one seeded permutation: with three variants and three rounds,
            # each policy occupies each within-round position once.
            @(for ($Index = 0; $Index -lt $BalancedVariants.Count; ++$Index) {
                $BalancedVariants[($Index + $Run - 1) % $BalancedVariants.Count]
            })
        } else { @($Variants | Sort-Object { $OrderRandom.Next() }) }
        foreach ($Variant in $RunVariants) {
            ++$CompletedCases
            $RunId = "N{0}_{1}_R{2}" -f $Count, $Variant.Name, $Run
            $RunRoot = Join-Path $EvidenceRoot $RunId
            New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null

            $StartedAt = Get-Date
            $LogName = "Benchmark_{0}_{1}.log" -f $RunName, $RunId
            $TraceDestination = if ($CaptureTaskTrace) { Join-Path $RunRoot "$RunId.utrace" } else { "" }
            $RunExecCmds = $ExecCmds
            if (-not [string]::IsNullOrWhiteSpace($Variant.Exec)) {
                $RunExecCmds += ",$($Variant.Exec)"
            }
            # Read actual runtime values after applying overrides. The plain
            # Baseline variant only queries these values, allowing a default probe.
            $RunExecCmds += ",r.AllowOcclusionQueries,r.HZBOcclusion,r.NumBufferedOcclusionQueries"
            $Arguments = @(
                $Project,
                $Map,
                "-game",
                "-windowed",
                "-ResX=1600",
                "-ResY=900",
                "-NoVSync",
                "-NoSplash",
                "-NoLiveCoding",
                "-Unattended",
                "-AutoBenchmark",
                "-BenchmarkEnemies=$Count",
                "-BenchmarkPlayerHealth=$PlayerHealth",
                "-BenchmarkWarmup=$WarmupSeconds",
                "-BenchmarkDuration=$DurationSeconds",
                "-BenchmarkSeed=$BenchmarkSeed",
                "-BenchmarkScreenshot",
                "-BenchmarkAutoQuit",
                "-csvGpuStats",
                ('-ExecCmds="{0}"' -f $RunExecCmds),
                "-log=$LogName",
                "-ddc=NoZenLocalFallback",
                "-LocalDataCachePath=$DdcPath",
                "-ShaderWorkingDir=Saved/ShaderWorkingDir"
            )
            if (-not [string]::IsNullOrWhiteSpace($Variant.Switch)) {
                $Arguments += $Variant.Switch
            }
            if ($CaptureTaskTrace) {
                # Enable task lifecycle recording before warmup so cross-boundary dependencies have IDs.
                # The existing Runner starts the file and capture region after warmup. Named events
                # expose renderer wait/dispatch scopes; traced timings are diagnostic, not baseline replacements.
                $Arguments += "-trace=cpu,gpu,frame,bookmark,task,stats,region,counters"
                $Arguments += "-statnamedevents"
                $Arguments += "-BenchmarkTraceFile=$TraceDestination"
            }

            Write-Output "[$CompletedCases/$TotalCases] Starting $RunId"
            $BeforeFingerprint = @(Get-ExperimentFingerprint)
            if (($BeforeFingerprint | ConvertTo-Json -Compress) -ne ($InitialFingerprint | ConvertTo-Json -Compress)) {
                throw "Tested binary/map/default configuration changed before $RunId."
            }
            $GpuBefore = Get-GpuSnapshot
            [PSCustomObject]@{ Editor = $EditorPath; Arguments = $Arguments; StartedAt = $StartedAt.ToString("o"); GpuBefore = $GpuBefore } |
                ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunRoot "launch.json") -Encoding UTF8
            $Process = Start-Process -FilePath $EditorPath -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
            $GpuAfter = Get-GpuSnapshot
            $AfterFingerprint = @(Get-ExperimentFingerprint)
            $InputIdentityValid = ($AfterFingerprint | ConvertTo-Json -Compress) -eq ($InitialFingerprint | ConvertTo-Json -Compress)
            if ($Process.ExitCode -ne 0) {
                throw "$RunId failed with exit code $($Process.ExitCode)."
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
                throw "$RunId did not produce a complete CSV, screenshot and log set."
            }

            $CsvDestination = Join-Path $RunRoot "$RunId.csv"
            $ScreenshotDestination = Join-Path $RunRoot "$RunId.png"
            $LogDestination = Join-Path $RunRoot $LogName
            Copy-Item -LiteralPath $Csv.FullName -Destination $CsvDestination -Force
            Copy-Item -LiteralPath $Screenshot.FullName -Destination $ScreenshotDestination -Force
            Copy-Item -LiteralPath $Log.FullName -Destination $LogDestination -Force

            $LogLines = @(Get-Content -LiteralPath $Log.FullName)
            $LogText = $LogLines -join [Environment]::NewLine
            $HealthMatch = [regex]::Match(
                $LogText,
                "Automated benchmark player health override: max=([0-9.]+) current=([0-9.]+)"
            )
            $SnapshotMatch = [regex]::Match(
                $LogText,
                "Benchmark enemy snapshot:.*castingShadow=(\d+) rayTracingVisible=(\d+)"
            )
            $AvoidanceMatch = [regex]::Match(
                $LogText,
                "Benchmark avoidance snapshot: enemies=(\d+) rvoEnabled=(\d+) crowdFollowing=(\d+) crowdValid=(\d+)"
            )
            $EndMatch = [regex]::Match(
                $LogText,
                "Automated benchmark validation: phase=capture-end requested=(\d+) alive=(\d+) playerHealth=([0-9.]+)"
            )

            $Ready = $LogText.Contains("Automated benchmark ready: requested=$Count alive=$Count")
            $CaptureCompleted = $LogText.Contains("Automated benchmark completed successfully.")
            $TraceValid = -not $CaptureTaskTrace
            if ($CaptureTaskTrace) {
                $TraceValid = (Test-Path -LiteralPath $TraceDestination) -and
                    (Get-Item -LiteralPath $TraceDestination).Length -gt 0 -and
                    $LogText.Contains("Automated benchmark Insights trace started:") -and
                    $LogText.Contains("Automated benchmark Insights trace stopped.")
            }
            $HealthApplied = $HealthMatch.Success -and [double]$HealthMatch.Groups[1].Value -ge $PlayerHealth
            $AliveAtEnd = if ($EndMatch.Success) { [int]$EndMatch.Groups[2].Value } else { -1 }
            $PlayerHealthAtEnd = if ($EndMatch.Success) { [double]$EndMatch.Groups[3].Value } else { 0.0 }
            $ShadowCastersAtStart = if ($SnapshotMatch.Success) { [int]$SnapshotMatch.Groups[1].Value } else { -1 }
            $RayTracingVisibleAtStart = if ($SnapshotMatch.Success) { [int]$SnapshotMatch.Groups[2].Value } else { -1 }
            $ConsumerOverrideValid =
                ($Variant.Name -ne "EnemyRayTracingOff" -or $RayTracingVisibleAtStart -eq 0) -and
                ($Variant.Name -ne "EnemyShadowsOff" -or $ShadowCastersAtStart -eq 0)
            $AllowOcclusionValue = Get-CVarValue $LogText "r.AllowOcclusionQueries"
            $HZBValue = Get-CVarValue $LogText "r.HZBOcclusion"
            $BufferedQueryValue = Get-CVarValue $LogText "r.NumBufferedOcclusionQueries"
            # Validate and record the same final echoed value; an earlier matching
            # value must not make a later conflicting override appear valid.
            $OcclusionOverrideValid =
                ($Variant.Name -ne "OcclusionQueriesOn" -or $AllowOcclusionValue -in @("1", "true")) -and
                ($Variant.Name -ne "OcclusionQueriesOff" -or $AllowOcclusionValue -in @("0", "false"))
            $CandidateConfigurationValid = $true
            if ($Variant.Name -in @("HardwareQueries", "HZBOcclusion", "BufferedQueries2")) {
                $ExpectedHZB = if ($Variant.Name -eq "HZBOcclusion") { "1" } else { "0" }
                $ExpectedBuffer = if ($Variant.Name -eq "BufferedQueries2") { "2" } else { "1" }
                $CandidateConfigurationValid = $AllowOcclusionValue -in @("1", "true") -and
                    $HZBValue -eq $ExpectedHZB -and $BufferedQueryValue -eq $ExpectedBuffer
            }
            $AvoidanceEnemies = if ($AvoidanceMatch.Success) { [int]$AvoidanceMatch.Groups[1].Value } else { -1 }
            $RVOEnabled = if ($AvoidanceMatch.Success) { [int]$AvoidanceMatch.Groups[2].Value } else { -1 }
            $CrowdFollowing = if ($AvoidanceMatch.Success) { [int]$AvoidanceMatch.Groups[3].Value } else { -1 }
            $CrowdValid = if ($AvoidanceMatch.Success) { [int]$AvoidanceMatch.Groups[4].Value } else { -1 }
            $ConsistentRVO =
                $AvoidanceMatch.Success -and
                $AvoidanceEnemies -eq $Count -and
                $RVOEnabled -eq $Count -and
                $CrowdFollowing -eq 0 -and
                $CrowdValid -eq 0
            $ConsistentDetourCrowd =
                $AvoidanceMatch.Success -and
                $AvoidanceEnemies -eq $Count -and
                $RVOEnabled -eq 0 -and
                $CrowdFollowing -eq $Count -and
                $CrowdValid -eq $Count
            $AvoidanceValid = switch ($ExpectedAvoidanceMode) {
                "RVO" { $ConsistentRVO }
                "DetourCrowd" { $ConsistentDetourCrowd }
                default { $ConsistentRVO -or $ConsistentDetourCrowd }
            }
            $Valid = $Ready -and $CaptureCompleted -and $HealthApplied -and
                $AliveAtEnd -eq $Count -and $PlayerHealthAtEnd -gt 0 -and
                $SnapshotMatch.Success -and $ConsumerOverrideValid -and $AvoidanceValid -and $TraceValid -and $OcclusionOverrideValid -and
                $CandidateConfigurationValid -and $InputIdentityValid

            $ManifestRows += [PSCustomObject]@{
                RunId = $RunId
                Variant = $Variant.Name
                ExecCmds = $RunExecCmds
                OcclusionOverrideValid = $OcclusionOverrideValid
                AllowOcclusionQueries = $AllowOcclusionValue
                HZBOcclusion = $HZBValue
                NumBufferedOcclusionQueries = $BufferedQueryValue
                CandidateConfigurationValid = $CandidateConfigurationValid
                InputIdentityValid = $InputIdentityValid
                GpuBefore = $GpuBefore
                GpuAfter = $GpuAfter
                StartedAt = $StartedAt.ToString("o")
                CompletedAt = (Get-Date).ToString("o")
                StartTrimSeconds = $StartTrimSeconds
                EndTrimSeconds = $EndTrimSeconds
                Run = $Run
                EnemyCount = $Count
                BenchmarkSeed = $BenchmarkSeed
                WarmupSeconds = $WarmupSeconds
                DurationSeconds = $DurationSeconds
                PlayerHealth = $PlayerHealth
                Ready = $Ready
                CaptureCompleted = $CaptureCompleted
                HealthApplied = $HealthApplied
                AliveAtEnd = $AliveAtEnd
                PlayerHealthAtEnd = $PlayerHealthAtEnd
                ShadowCastersAtStart = $ShadowCastersAtStart
                RayTracingVisibleAtStart = $RayTracingVisibleAtStart
                ConsumerOverrideValid = $ConsumerOverrideValid
                ExpectedAvoidanceMode = $ExpectedAvoidanceMode
                AvoidanceEnemies = $AvoidanceEnemies
                RVOEnabled = $RVOEnabled
                CrowdFollowing = $CrowdFollowing
                CrowdValid = $CrowdValid
                AvoidanceValid = $AvoidanceValid
                TaskTraceRequested = [bool]$CaptureTaskTrace
                TraceValid = $TraceValid
                Trace = $TraceDestination
                Valid = $Valid
                SpawnFailures = ($LogLines | Select-String -SimpleMatch "SpawnActor failed for enemy class").Count
                VSMQueueOverflows = ($LogLines | Select-String -SimpleMatch "Non-Nanite Marking Job Queue overflow").Count
                TexturePoolWarnings = ($LogLines | Select-String -Pattern "Texture streaming pool.*over budget").Count
                Csv = $CsvDestination
                Screenshot = $ScreenshotDestination
                Log = $LogDestination
            }
            $ManifestRows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv") -NoTypeInformation -Encoding UTF8

            if (-not $Valid) {
                throw "$RunId failed benchmark validation. Inspect $LogDestination."
            }
            Write-Output "[$CompletedCases/$TotalCases] Completed $RunId"
        }
    }
}

$SummaryScript = Join-Path $ProjectRoot "Tools\SummarizeRenderCostMatrix.ps1"
& $SummaryScript -EvidenceRoot $EvidenceRoot -StartTrimSeconds $StartTrimSeconds -EndTrimSeconds $EndTrimSeconds
Write-Output "Performance matrix evidence: $EvidenceRoot"
