param(
    [Parameter(Mandatory = $true)][string]$TraceFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Region = "AutomatedBenchmarkCapture",
    [switch]$IncludeWaitEvents
)

$ErrorActionPreference = "Stop"
$Insights = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealInsights.exe"
$TraceFile = (Resolve-Path -LiteralPath $TraceFile).Path
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Choose a new output directory; prior evidence must not be overwritten: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$Columns = "ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth"
$Exports = @(
    @{Name="GpuEvents"; Command="TimingInsights.ExportTimingEvents"; Options="-columns=$Columns -threads=GPU* -timers=* -region=$Region"},
    @{Name="GpuStatistics"; Command="TimingInsights.ExportTimerStatistics"; Options="-columns=* -threads=GPU* -timers=* -region=$Region"}
)
if ($IncludeWaitEvents) {
    $Exports += @{Name="WaitEvents"; Command="TimingInsights.ExportTimingEvents"; Options="-columns=$Columns -threads=* -timers=WaitForGatherDynamicMeshElements,OcclusionCullPipe,SyncPoint_Wait -region=$Region"}
}

# Export offline only. Never run Insights concurrently with a measured game.
# GPU event durations are timestamp scopes, not independent hardware busy intervals.
foreach ($Export in $Exports) {
    $Destination = Join-Path $OutputDirectory "$($Export.Name).csv"
    $Log = Join-Path $OutputDirectory "$($Export.Name).log"
    # Keep the engine console argument simple; reject paths requiring nested quoting.
    if ($Destination -match '\s') { throw "Use an export path without spaces: $Destination" }
    $Command = "$($Export.Command) $Destination $($Export.Options)"
    $Arguments = "-OpenTraceFile=`"$TraceFile`" -ABSLOG=`"$Log`" -AutoQuit -NoUI -ExecOnAnalysisCompleteCmd=`"$Command`" -log"
    Write-Output "Exporting $($Export.Name) from $TraceFile"
    $Process = Start-Process -FilePath $Insights -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(60000)) {
        $Process.Kill()
        throw "Owned Insights exporter timed out. Source trace is preserved. See $Log"
    }
    $Process.Refresh()
    if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Destination)) {
        throw "Export failed: $($Export.Name). See $Log"
    }
    $RegionCounts = @([regex]::Matches((Get-Content -LiteralPath $Log -Raw), 'Detected (\d+) regions'))
    if ($RegionCounts.Count -ne 1 -or $RegionCounts[0].Groups[1].Value -ne '1') {
        throw "Expected exactly one capture region; reject potentially overwritten export. See $Log"
    }
}
[ordered]@{
    TraceFile=$TraceFile; TraceSHA256=(Get-FileHash -LiteralPath $TraceFile -Algorithm SHA256).Hash
    Region=$Region; TimestampUnits="seconds"; IncludeWaitEvents=[bool]$IncludeWaitEvents
    StatisticsNote="Engine statistics may aggregate GPU timelines; separated event analysis is authoritative."
    Engine="UE 5.5.4"; ExportedAt=(Get-Date).ToString("o")
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory "provenance.json") -Encoding UTF8
Write-Output "GPU timing exports: $OutputDirectory"
