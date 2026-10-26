param(
    [Parameter(Mandatory = $true)][string]$TraceFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Region = "AutomatedBenchmarkCapture",
    [double]$StartTime,
    [double]$EndTime,
    [string]$EngineRoot = 'E:/program/ue554/UE_5.5',
    [ValidateRange(5,600)][int]$TimeoutSeconds = 120,
    [switch]$IncludeGpuEvents,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$HasStart = $PSBoundParameters.ContainsKey('StartTime')
$HasEnd = $PSBoundParameters.ContainsKey('EndTime')
if ($HasStart -ne $HasEnd) { throw 'Provide both StartTime and EndTime, or neither.' }
$WindowMode = $HasStart -and $HasEnd
if ($WindowMode) {
    if ([double]::IsNaN($StartTime) -or [double]::IsInfinity($StartTime) -or
        [double]::IsNaN($EndTime) -or [double]::IsInfinity($EndTime) -or $StartTime -lt 0 -or $EndTime -le $StartTime) {
        throw 'A time window must be finite, nonnegative and have EndTime > StartTime.'
    }
    if ($PSBoundParameters.ContainsKey('Region')) { throw 'Explicit time windows and Region are mutually exclusive.' }
} elseif ($Region -notmatch '^[A-Za-z0-9_.:-]+$') { throw 'Region must be one unquoted region token.' }
$Insights = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealInsights.exe'
$TraceFile = (Resolve-Path -LiteralPath $TraceFile).Path
if (-not (Test-Path -LiteralPath $TraceFile -PathType Leaf)) { throw 'TraceFile must be a file.' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new export directory; existing evidence must not be overwritten.' }
if ($OutputDirectory -match '\s|["\r\n]') { throw 'Use an export directory without whitespace or quotes for engine console arguments.' }
$WindowOptions = if ($WindowMode) {
    '-startTime=' + $StartTime.ToString('R',[Globalization.CultureInfo]::InvariantCulture) +
        ' -endTime=' + $EndTime.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
} else { '-region=' + $Region }
$EventColumns = "ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth"
$EventHeader = $EventColumns.Split(',')
$StatsHeader = @('Name','Count','Incl','I.Min','I.Max','I.Avg','I.Med','Excl','E.Min','E.Max','E.Avg','E.Med')
# Keep the historical named-region exports available. For explicit short windows
# export EVERY scope on each selected CPU thread: timer-name filtering loses the
# nested ExecuteTask/work scopes needed to distinguish help from blocking waits.
$RelevantTimers = "*Wait*,*Visibility*,*Gather*,*Relevance*,*Occlusion*,*HZB*,*Query*,*Fence*,*SkinCache*,*SceneVisibility*,*ProcessTasks*,*Submission*,*Interrupt*,*Translate*,*Frame*"
$Exports = @(
    @{ Name = "Threads"; Command = "TimingInsights.ExportThreads"; Options = ""; Header = @('Id','Name','Group') },
    @{ Name = "Timers"; Command = "TimingInsights.ExportTimers"; Options = ""; Header = @('Id','Type','Name','File','Line') }
)
if ($WindowMode) {
    $Exports += @{ Name = 'CPUEvents'; Command = 'TimingInsights.ExportTimingEvents';
        Options = "-columns=$EventColumns -threads=GameThread,RenderThread*,RHI*,Foreground*,Background*,Worker*,TaskGraph* -timers=* $WindowOptions"; Header = $EventHeader }
} else {
    $Exports += @(
        @{ Name = 'RenderThreadEvents'; Command = 'TimingInsights.ExportTimingEvents'; Options = "-columns=$EventColumns -threads=RenderThread* -timers=* $WindowOptions"; Header = $EventHeader },
        @{ Name = 'RelevantEvents'; Command = 'TimingInsights.ExportTimingEvents'; Options = "-columns=$EventColumns -threads=* -timers=$RelevantTimers $WindowOptions"; Header = $EventHeader }
    )
}
$Exports += @{ Name = 'TimerStatistics'; Command = 'TimingInsights.ExportTimerStatistics'; Options = "-columns=* -threads=* -timers=* $WindowOptions"; Header = $StatsHeader }
if ($IncludeGpuEvents) {
    # CPU and GPU use the same source, time window, process ownership and CSV validation.
    $Exports += @(
        @{ Name = 'GpuEvents'; Command = 'TimingInsights.ExportTimingEvents'; Options = "-columns=$EventColumns -threads=GPU* -timers=* $WindowOptions"; Header = $EventHeader },
        @{ Name = 'GpuStatistics'; Command = 'TimingInsights.ExportTimerStatistics'; Options = "-columns=* -threads=GPU* -timers=* $WindowOptions"; Header = $StatsHeader }
    )
}

if ($DryRun) {
    [ordered]@{Mode=if($WindowMode){'TimeWindow'}else{'Region'};TraceFile=$TraceFile;OutputDirectory=$OutputDirectory;
        StartTime=if($WindowMode){$StartTime}else{$null};EndTime=if($WindowMode){$EndTime}else{$null};
        Region=if($WindowMode){$null}else{$Region};IncludeGpuEvents=[bool]$IncludeGpuEvents;Exports=$Exports;
        Boundary='No files written or applications launched. Explicit time bounds do not by themselves prove complete-frame alignment.'} | ConvertTo-Json -Depth 6
    return
}
if (-not (Test-Path -LiteralPath $Insights -PathType Leaf)) { throw 'UnrealInsights executable is missing.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$SourceInfo = Get-Item -LiteralPath $TraceFile
$Manifest = [ordered]@{SchemaVersion=2;Mode=if($WindowMode){'TimeWindow'}else{'Region'};
    TraceFile=$TraceFile;TraceSHA256=(Get-FileHash -LiteralPath $TraceFile -Algorithm SHA256).Hash;TraceBytes=$SourceInfo.Length;
    TraceLastWriteUtc=$SourceInfo.LastWriteTimeUtc.ToString('o');Engine=$Insights;TimestampUnits='seconds';
    StartTime=if($WindowMode){$StartTime}else{$null};EndTime=if($WindowMode){$EndTime}else{$null};
    Region=if($WindowMode){$null}else{$Region};IncludeGpuEvents=[bool]$IncludeGpuEvents;EventColumns=$EventHeader;TimeoutSeconds=$TimeoutSeconds;
    CPUHierarchy=if($WindowMode){'All timer scopes retained for selected CPU threads; no timer-name threshold.'}else{'Legacy render-only hierarchy plus filtered cross-thread candidates.'};
    CompleteFrameAlignment='Not implied by Region or explicit bounds. Supply independently validated complete-frame alignment to the analyzer.';
    SchedulingData='Not exported. Timing scope durations do not establish on-core, ready or OS-blocked time.';
    GpuTotalFrameTimeAvailable=$false;GpuBoundary='GPU exports are optional timestamp scopes, not hardware busy intervals. Complete GPU frame alignment still requires independent validation. Statistics can contain GPU rows even under a CPU filter.';
    Exports=@();Valid=$false;Failure=$null;StartedUtc=[datetime]::UtcNow.ToString('o')}
$ManifestPath = Join-Path $OutputDirectory 'export_manifest.json'
function Save-ExportManifest { [IO.File]::WriteAllText($ManifestPath,($Manifest | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false)) }
Save-ExportManifest

try {
foreach ($Export in $Exports) {
    $Destination = Join-Path $OutputDirectory "$($Export.Name).csv"
    $Log = Join-Path $OutputDirectory "$($Export.Name).log"
    $Command = "$($Export.Command) $Destination $($Export.Options)"
    $Arguments = @(('-OpenTraceFile="'+$TraceFile+'"'),('-ABSLOG="'+$Log+'"'),'-AutoQuit','-NoUI',('-ExecOnAnalysisCompleteCmd="'+$Command+'"'),'-log')
    Write-Output "Exporting $($Export.Name)"
    $Process = Start-Process -FilePath $Insights -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    try {
        if (-not $Process.WaitForExit($TimeoutSeconds*1000)) {
            $Process.Kill()
            $null = $Process.WaitForExit(10000)
            throw "Owned Insights export timed out: $($Export.Name); source Trace is retained."
        }
        $Process.Refresh()
        if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Destination -PathType Leaf) -or (Get-Item -LiteralPath $Destination).Length -eq 0) {
            throw "Trace export failed: $($Export.Name); inspect $Log"
        }
    } finally { $Process.Dispose() }
    $Reader = [IO.StreamReader]::new($Destination)
    try {
        $Header = $Reader.ReadLine()
        $FirstData = $Reader.ReadLine()
    } finally { $Reader.Dispose() }
    $ActualHeader = @($Header.Split(',') | ForEach-Object {$_.Trim().Trim('"')})
    if (($ActualHeader -join ',') -cne ($Export.Header -join ',') -or [string]::IsNullOrWhiteSpace($FirstData)) {
        throw "Invalid columns or header-only export: $($Export.Name)"
    }
    if (-not $WindowMode -and $Export.Options -match '-region=') {
        $Regions = @([regex]::Matches([IO.File]::ReadAllText($Log),'Detected (\d+) regions'))
        if ($Regions.Count -ne 1 -or $Regions[0].Groups[1].Value -ne '1') { throw 'Expected exactly one matching capture region.' }
    }
    $Manifest.Exports += [ordered]@{Name=$Export.Name;File=$Destination;Bytes=(Get-Item -LiteralPath $Destination).Length;
        Command=$Command;Columns=$ActualHeader;ExitCode=0;Log=$Log}
    Save-ExportManifest
}
if ((Get-Item -LiteralPath $TraceFile).Length -ne $SourceInfo.Length -or (Get-Item -LiteralPath $TraceFile).LastWriteTimeUtc -ne $SourceInfo.LastWriteTimeUtc) {
    throw 'Source Trace changed during offline export; discard this mixed analysis.'
}
$Manifest.Valid = $true
} catch {
    $Manifest.Failure = $_.Exception.Message
    throw
} finally {
    $Manifest.FinishedUtc = [datetime]::UtcNow.ToString('o')
    Save-ExportManifest
}
Write-Output "Performance events exported to $OutputDirectory (seconds; scope wall time, not OS scheduling time)."
