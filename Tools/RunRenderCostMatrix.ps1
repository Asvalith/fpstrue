param(
    [int[]]$Counts = @(20, 50, 100, 160),
    [int]$RunsPerCase = 3,
    [double]$WarmupSeconds = 15,
    [double]$DurationSeconds = 30,
    [Nullable[double]]$ScreenPercentage = $null,
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
    [string]$DdcPath = "",
    [string]$ConfigFile = "",
    [Alias("DryRun")]
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"

# Resolve and validate before checking machine paths or creating any evidence.
# Explicit CLI arguments (including -CaptureTaskTrace:$false) win over JSON.
. (Join-Path $PSScriptRoot "ReadRenderCostConfig.ps1")
$InputConfig = if (-not [string]::IsNullOrWhiteSpace($ConfigFile)) { Read-RenderCostConfig $ConfigFile } else { $null }
$ParameterSources = [ordered]@{}
foreach ($Name in $RenderCostConfigParameters) {
    $ParameterSources[$Name] = "Default"
    if ($null -ne $InputConfig -and $null -ne $InputConfig.Settings.PSObject.Properties[$Name]) {
        if (-not $PSBoundParameters.ContainsKey($Name)) { Set-Variable -Name $Name -Value $InputConfig.Settings.$Name }
        $ParameterSources[$Name] = "ConfigFile"
    }
    if ($PSBoundParameters.ContainsKey($Name)) { $ParameterSources[$Name] = "CommandLine" }
}

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
foreach ($Name in @('WarmupSeconds', 'DurationSeconds', 'PlayerHealth', 'StartTrimSeconds', 'EndTrimSeconds', 'ScreenPercentage')) {
    $Value = (Get-Variable -Name $Name).Value
    if ($null -ne $Value -and ([double]::IsNaN($Value) -or [double]::IsInfinity($Value))) {
        throw "$Name must be finite."
    }
}
if ($WarmupSeconds -lt 0 -or $DurationSeconds -lt 1 -or $PlayerHealth -lt 1) {
    throw "Warmup, duration or player health is outside the valid range."
}
if ($null -ne $ScreenPercentage -and ($ScreenPercentage -le 0 -or $ScreenPercentage -gt 100)) {
    throw "ScreenPercentage must be finite, greater than zero and at most 100; omit it to preserve the existing policy."
}
if ($StartTrimSeconds -lt 0 -or $EndTrimSeconds -lt 0 -or
    $StartTrimSeconds + $EndTrimSeconds -ge $DurationSeconds) {
    throw "Summary edge trimming must leave a positive capture interval."
}

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

# CLI maps may include an object name or travel options; the fingerprint identifies the package file.
$MapMatch = [regex]::Match($Map, '\A/Game/(?<Package>(?:[A-Za-z0-9_]+/)*[A-Za-z0-9_]+)(?:\.[A-Za-z0-9_]+)?(?:\?[^\s"]*)?\z')
if (-not $MapMatch.Success) {
    throw 'Map must be a /Game/... package path, optionally followed by .object and ?travel options.'
}
$MapFile = Join-Path $ProjectRoot ('Content/{0}.umap' -f $MapMatch.Groups['Package'].Value)

$ResolvedSettings = [ordered]@{}
foreach ($Name in $RenderCostConfigParameters) { $ResolvedSettings[$Name] = (Get-Variable -Name $Name).Value }
$ResolvedSettings.CaptureTaskTrace = [bool]$CaptureTaskTrace
$ResolvedSettings.BalancedVariantOrder = [bool]$BalancedVariantOrder
$ExperimentConfiguration = [PSCustomObject]@{ SchemaVersion = 1; Settings = $ResolvedSettings; Sources = $ParameterSources; InputConfig = $InputConfig }
if ($ValidateOnly) {
    # A configuration report only: no UE/process queries or Saved/Profiling output.
    [PSCustomObject]@{ Mode = "ValidateOnly"; Configuration = $ExperimentConfiguration } | ConvertTo-Json -Depth 8
    return
}

$Project = Join-Path $ProjectRoot "fpstrue.uproject"
if (-not (Test-Path -LiteralPath $EditorPath)) {
    throw "Unreal Editor was not found: $EditorPath"
}
if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project was not found: $Project"
}
if (-not (Test-Path -LiteralPath $MapFile -PathType Leaf)) {
    throw "Selected map was not found: $MapFile"
}

# Observe only; even engine-owned shader workers invalidate a clean timing run.
# Never terminate another project, an Insights session, or a worker process.
$ConflictingProcessNames = @("UnrealEditor", "UnrealEditor-Cmd", "UnrealInsights", "Insights", "ShaderCompileWorker", "UnrealLightmass")
$IsolationPollMilliseconds = 750
$RunTimeoutSeconds = [Math]::Max(300.0, $WarmupSeconds + $DurationSeconds + 180.0)
function Get-IsolationObservation {
    param([int]$OwnedProcessId = 0, [string]$Phase)
    $Conflicts = @(Get-Process -Name $ConflictingProcessNames -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $OwnedProcessId } |
        ForEach-Object { [PSCustomObject]@{ ProcessId = $_.Id; Name = $_.ProcessName } })
    return [PSCustomObject]@{ ObservedAt = (Get-Date).ToString("o"); Phase = $Phase; Conflicts = $Conflicts }
}

$EvidenceRoot = Join-Path $ProjectRoot "Saved\Profiling\$RunName"
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "Choose a new RunName; existing experiment evidence must not be overwritten: $EvidenceRoot"
}
$SourceCsv = Join-Path $ProjectRoot "Saved\Profiling\CSV"
$SourceScreenshots = Join-Path $ProjectRoot "Saved\Screenshots\WindowsEditor"
$SourceLogs = Join-Path $ProjectRoot "Saved\Logs"
$ExecCmds = "r.RayTracing.ForceAllRayTracingEffects -1,r.RayTracing.Geometry.SkeletalMeshes 1"

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null

# Record immutable experiment inputs, rather than treating the branch name as the
# build identity. These hashes cover the tested binary, map and default config;
# they do not imply every referenced asset or machine condition was frozen.
$FingerprintPaths = @(
    (Join-Path $ProjectRoot "Binaries\Win64\UnrealEditor-fpstrue.dll"),
    $MapFile,
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
# Engine-owned user settings may be rewritten on exit. Keep the initial contents
# for audit without treating unrelated serialization changes as immutable inputs.
$GameUserSettingsPath = Join-Path $ProjectRoot "Saved\Config\WindowsEditor\GameUserSettings.ini"
$GameUserSettingsSnapshot = if (Test-Path -LiteralPath $GameUserSettingsPath) {
    [PSCustomObject]@{
        Path = $GameUserSettingsPath
        SHA256 = (Get-FileHash -LiteralPath $GameUserSettingsPath -Algorithm SHA256).Hash
        Content = Get-Content -LiteralPath $GameUserSettingsPath -Raw
    }
} else { $null }
$EnvironmentRecord = [ordered]@{
    StartedAt = (Get-Date).ToString("o")
    ExperimentConfiguration = $ExperimentConfiguration
    EditorPath = $EditorPath
    Map = $Map
    Resolution = "1600x900"
    NoVSync = $true
    RequestedScreenPercentage = $ScreenPercentage
    GameUserSettings = $GameUserSettingsSnapshot
    GitCommit = ((& git -C $ProjectRoot rev-parse HEAD) -join "`n")
    GitStatus = @(& git -C $ProjectRoot status --short)
    Inputs = $InitialFingerprint
    PowerScheme = ((& powercfg /getactivescheme) -join "`n")
    GpuSnapshot = Get-GpuSnapshot
    GpuSnapshotColumns = "timestamp,name,driver_version,temperature.gpu,clocks.current.graphics,clocks.current.memory,pstate,power.draw,utilization.gpu,memory.used"
    StartTrimSeconds = $StartTrimSeconds
    EndTrimSeconds = $EndTrimSeconds
    BalancedVariantOrder = [bool]$BalancedVariantOrder
    IsolationProcessNames = $ConflictingProcessNames
    IsolationPollMilliseconds = $IsolationPollMilliseconds
    RunTimeoutSeconds = $RunTimeoutSeconds
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
            if ($null -ne $ScreenPercentage) {
                $RunExecCmds += ",r.ScreenPercentage " + ([double]$ScreenPercentage).ToString("R", [Globalization.CultureInfo]::InvariantCulture)
            }
            # Read actual runtime values after applying overrides. The plain
            # Baseline variant only queries these values, allowing a default probe.
            $RunExecCmds += ",r.AllowOcclusionQueries,r.HZBOcclusion,r.NumBufferedOcclusionQueries"
            $RunExecCmds += ",r.ScreenPercentage,r.DynamicRes.OperationMode,r.DynamicRes.TestScreenPercentage,t.MaxFPS,r.VSync,sg.ResolutionQuality"
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
            $IsolationAuditPath = Join-Path $RunRoot "isolation.json"
            $IsolationObservations = New-Object System.Collections.Generic.List[object]
            $IsolationValid = $true
            $Process = $null
            $ProcessExitCode = $null
            $TimedOut = $false
            $RunError = ""
            $RunClock = [Diagnostics.Stopwatch]::StartNew()
            try {
                $Observation = Get-IsolationObservation -Phase "before-launch"
                $IsolationObservations.Add($Observation)
                if ($Observation.Conflicts.Count -gt 0) {
                    $IsolationValid = $false
                    $RunError = "Conflicting processes were present before launch; the game was not started."
                } else {
                    $Process = Start-Process -FilePath $EditorPath -ArgumentList $Arguments -WindowStyle Hidden -PassThru
                    while (-not $Process.HasExited) {
                        $Observation = Get-IsolationObservation -OwnedProcessId $Process.Id -Phase "running"
                        $IsolationObservations.Add($Observation)
                        if ($Observation.Conflicts.Count -gt 0) { $IsolationValid = $false }
                        # Finish a contaminated capture for audit. Only a timeout or
                        # failure below may reclaim this exact process we launched.
                        if ($RunClock.Elapsed.TotalSeconds -ge $RunTimeoutSeconds) {
                            $TimedOut = $true
                            $RunError = "The owned game process exceeded the $RunTimeoutSeconds second timeout."
                            break
                        }
                        $null = $Process.WaitForExit($IsolationPollMilliseconds)
                        $Process.Refresh()
                    }
                }
            } catch {
                $RunError = $_.Exception.Message
            } finally {
                if ($null -ne $Process) {
                    if (-not $Process.HasExited) {
                        # Kill uses the retained process handle, never a process-name
                        # search or a process tree that could include another task.
                        $Process.Kill()
                        $null = $Process.WaitForExit(10000)
                    }
                    if ($Process.HasExited) { $ProcessExitCode = $Process.ExitCode }
                }
                # The exited owned process can remain briefly enumerable while
                # its retained handle is alive. It is not a competing UE process.
                $Observation = Get-IsolationObservation -OwnedProcessId $(if ($null -ne $Process) { $Process.Id } else { 0 }) -Phase "after-exit"
                $IsolationObservations.Add($Observation)
                if ($Observation.Conflicts.Count -gt 0) { $IsolationValid = $false }
                $RunClock.Stop()
                [PSCustomObject]@{
                    RunId = $RunId
                    OwnedProcessId = if ($null -ne $Process) { $Process.Id } else { $null }
                    ProcessExitCode = $ProcessExitCode
                    IsolationValid = $IsolationValid
                    PollMilliseconds = $IsolationPollMilliseconds
                    TimeoutSeconds = $RunTimeoutSeconds
                    ElapsedSeconds = $RunClock.Elapsed.TotalSeconds
                    TimedOut = $TimedOut
                    RunError = $RunError
                    Observations = $IsolationObservations.ToArray()
                    Note = "750 ms process snapshots can miss shorter overlaps; shader workers are conservatively treated as contamination."
                } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $IsolationAuditPath -Encoding UTF8
            }
            $GpuAfter = Get-GpuSnapshot
            $AfterFingerprint = @(Get-ExperimentFingerprint)
            $InputIdentityValid = ($AfterFingerprint | ConvertTo-Json -Compress) -eq ($InitialFingerprint | ConvertTo-Json -Compress)
            $ProcessCompleted = $null -ne $ProcessExitCode -and $ProcessExitCode -eq 0 -and -not $TimedOut -and
                [string]::IsNullOrWhiteSpace($RunError)

            $Csv = Get-ChildItem -LiteralPath $SourceCsv -File -Filter "*.csv" -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $StartedAt } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            $Screenshot = Get-ChildItem -LiteralPath $SourceScreenshots -File -Filter "*.png" -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $StartedAt } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            $Log = Get-Item -LiteralPath (Join-Path $SourceLogs $LogName) -ErrorAction SilentlyContinue
            $ArtifactsComplete = $null -ne $Csv -and $null -ne $Screenshot -and $null -ne $Log

            $CsvDestination = Join-Path $RunRoot "$RunId.csv"
            $ScreenshotDestination = Join-Path $RunRoot "$RunId.png"
            $LogDestination = Join-Path $RunRoot $LogName
            # Keep whatever was produced even on timeout, concurrent occupancy,
            # or a missing artifact; the manifest below records the rejected run.
            if ($null -ne $Csv) { Copy-Item -LiteralPath $Csv.FullName -Destination $CsvDestination -Force }
            if ($null -ne $Screenshot) { Copy-Item -LiteralPath $Screenshot.FullName -Destination $ScreenshotDestination -Force }
            if ($null -ne $Log) { Copy-Item -LiteralPath $Log.FullName -Destination $LogDestination -Force }

            $LogLines = @(if ($null -ne $Log) { Get-Content -LiteralPath $Log.FullName })
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
            $InputLocked = $LogText.Contains("Automated benchmark input locked:")
            $DiagnosticsMatch = [regex]::Match($LogText,
                "Benchmark diagnostics applied: enemies=(\d+) attackSweepOff=0 pawnCollisionOff=0 pathFollowingTickOff=0 characterMovementTickOff=0 skeletalMeshTickOff=0 significanceOff=0(?:\s|$)")
            $DefaultDiagnosticsValid = $DiagnosticsMatch.Success -and [int]$DiagnosticsMatch.Groups[1].Value -eq $Count
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
            $StrictCandidate = $Count -gt 0 -and $Variant.Name -in @("HardwareQueries", "BufferedQueries2")
            $CandidateConsumersValid = -not $StrictCandidate -or
                ($ShadowCastersAtStart -eq 5 -and $RayTracingVisibleAtStart -eq 12)
            $SpawnFailures = ($LogLines | Select-String -SimpleMatch "SpawnActor failed for enemy class").Count
            $VSMQueueOverflows = ($LogLines | Select-String -SimpleMatch "Non-Nanite Marking Job Queue overflow").Count
            $TexturePoolWarnings = ($LogLines | Select-String -Pattern "Texture streaming pool.*over budget").Count
            $RenderWarningsValid = $SpawnFailures -eq 0 -and $VSMQueueOverflows -eq 0 -and $TexturePoolWarnings -eq 0
            $ConsumerOverrideValid =
                ($Variant.Name -ne "EnemyRayTracingOff" -or $RayTracingVisibleAtStart -eq 0) -and
                ($Variant.Name -ne "EnemyShadowsOff" -or $ShadowCastersAtStart -eq 0)
            $AllowOcclusionValue = Get-CVarValue $LogText "r.AllowOcclusionQueries"
            $HZBValue = Get-CVarValue $LogText "r.HZBOcclusion"
            $BufferedQueryValue = Get-CVarValue $LogText "r.NumBufferedOcclusionQueries"
            $ScreenPercentageValue = Get-CVarValue $LogText "r.ScreenPercentage"
            $DynamicResolutionValue = Get-CVarValue $LogText "r.DynamicRes.OperationMode"
            $DynamicResolutionTestValue = Get-CVarValue $LogText "r.DynamicRes.TestScreenPercentage"
            $MaxFPSValue = Get-CVarValue $LogText "t.MaxFPS"
            $VSyncValue = Get-CVarValue $LogText "r.VSync"
            $ResolutionQualityValue = Get-CVarValue $LogText "sg.ResolutionQuality"
            $ScreenPercentageOverrideValid = $true
            if ($null -ne $ScreenPercentage) {
                $ActualScreenPercentage = 0.0
                $ActualMaxFPS = 0.0
                $ActualResolutionQuality = 0.0
                $ActualDynamicResolutionTest = 0.0
                $NumberStyle = [Globalization.NumberStyles]::Float
                $NumberCulture = [Globalization.CultureInfo]::InvariantCulture
                # Dynamic resolution (including its debug override) must already
                # be disabled. Never change it to make a requested percentage work.
                $ScreenPercentageOverrideValid =
                    [double]::TryParse($ScreenPercentageValue, $NumberStyle, $NumberCulture, [ref]$ActualScreenPercentage) -and
                    [Math]::Abs($ActualScreenPercentage - [double]$ScreenPercentage) -le 0.0001 -and
                    $DynamicResolutionValue -eq "0" -and
                    [double]::TryParse($DynamicResolutionTestValue, $NumberStyle, $NumberCulture, [ref]$ActualDynamicResolutionTest) -and
                    $ActualDynamicResolutionTest -eq 0 -and
                    [double]::TryParse($MaxFPSValue, $NumberStyle, $NumberCulture, [ref]$ActualMaxFPS) -and
                    $ActualMaxFPS -eq 0 -and $VSyncValue -in @("0", "false") -and
                    [double]::TryParse($ResolutionQualityValue, $NumberStyle, $NumberCulture, [ref]$ActualResolutionQuality) -and
                    -not [double]::IsNaN($ActualResolutionQuality) -and -not [double]::IsInfinity($ActualResolutionQuality)
            }
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
                $CandidateConfigurationValid -and $InputIdentityValid -and $IsolationValid -and $InputLocked -and
                $DefaultDiagnosticsValid -and $CandidateConsumersValid -and (-not $StrictCandidate -or $RenderWarningsValid) -and
                $ProcessCompleted -and $ArtifactsComplete -and $ScreenPercentageOverrideValid

            $ManifestRows += [PSCustomObject]@{
                RunId = $RunId
                Variant = $Variant.Name
                ExecCmds = $RunExecCmds
                RequestedScreenPercentage = $ScreenPercentage
                ScreenPercentage = $ScreenPercentageValue
                DynamicResolutionOperationMode = $DynamicResolutionValue
                DynamicResolutionTestScreenPercentage = $DynamicResolutionTestValue
                MaxFPS = $MaxFPSValue
                VSync = $VSyncValue
                ResolutionQuality = $ResolutionQualityValue
                ScreenPercentageOverrideValid = $ScreenPercentageOverrideValid
                OcclusionOverrideValid = $OcclusionOverrideValid
                AllowOcclusionQueries = $AllowOcclusionValue
                HZBOcclusion = $HZBValue
                NumBufferedOcclusionQueries = $BufferedQueryValue
                CandidateConfigurationValid = $CandidateConfigurationValid
                InputIdentityValid = $InputIdentityValid
                IsolationValid = $IsolationValid
                IsolationAudit = $IsolationAuditPath
                InputLocked = $InputLocked
                DefaultDiagnosticsValid = $DefaultDiagnosticsValid
                CandidateConsumersValid = $CandidateConsumersValid
                RenderWarningsValid = $RenderWarningsValid
                OwnedProcessId = if ($null -ne $Process) { $Process.Id } else { $null }
                ProcessExitCode = $ProcessExitCode
                ProcessCompleted = $ProcessCompleted
                TimedOut = $TimedOut
                RunError = $RunError
                ArtifactsComplete = $ArtifactsComplete
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
                SpawnFailures = $SpawnFailures
                VSMQueueOverflows = $VSMQueueOverflows
                TexturePoolWarnings = $TexturePoolWarnings
                Csv = if ($null -ne $Csv) { $CsvDestination } else { "" }
                Screenshot = if ($null -ne $Screenshot) { $ScreenshotDestination } else { "" }
                Log = if ($null -ne $Log) { $LogDestination } else { "" }
            }
            $ManifestRows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv") -NoTypeInformation -Encoding UTF8

            if (-not $Valid) {
                throw "$RunId failed benchmark validation. Inspect manifest.csv, $IsolationAuditPath and $LogDestination."
            }
            Write-Output "[$CompletedCases/$TotalCases] Completed $RunId"
        }
    }
}

$SummaryScript = Join-Path $ProjectRoot "Tools\SummarizeRenderCostMatrix.ps1"
& $SummaryScript -EvidenceRoot $EvidenceRoot -StartTrimSeconds $StartTrimSeconds -EndTrimSeconds $EndTrimSeconds
Write-Output "Performance matrix evidence: $EvidenceRoot"
