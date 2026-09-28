param(
    [Parameter(Mandatory = $true)][string]$TraceFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Region = "AutomatedBenchmarkCapture"
)

$ErrorActionPreference = "Stop"
$Insights = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealInsights.exe"
$TraceFile = (Resolve-Path -LiteralPath $TraceFile).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$EventColumns = "ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth"
# Retain every RenderThread scope for inclusive/exclusive attribution. The cross-thread
# export includes worker/RHI query and scheduler scopes needed to follow the wait producer.
$RelevantTimers = "*Wait*,*Visibility*,*Gather*,*Relevance*,*Occlusion*,*HZB*,*Query*,*Fence*,*SkinCache*,*SceneVisibility*,*ProcessTasks*,*Submission*,*Interrupt*,*Translate*,*Frame*"
$Exports = @(
    @{ Name = "Threads"; Command = "TimingInsights.ExportThreads"; Options = "" },
    @{ Name = "Timers"; Command = "TimingInsights.ExportTimers"; Options = "" },
    @{ Name = "RenderThreadEvents"; Command = "TimingInsights.ExportTimingEvents"; Options = "-columns=$EventColumns -threads=RenderThread* -timers=* -region=$Region" },
    @{ Name = "RelevantEvents"; Command = "TimingInsights.ExportTimingEvents"; Options = "-columns=$EventColumns -threads=* -timers=$RelevantTimers -region=$Region" },
    @{ Name = "TimerStatistics"; Command = "TimingInsights.ExportTimerStatistics"; Options = "-columns=* -threads=* -timers=* -region=$Region" }
)

foreach ($Export in $Exports) {
    $Destination = Join-Path $OutputDirectory "$($Export.Name).csv"
    $Log = Join-Path $OutputDirectory "$($Export.Name).log"
    $Command = "$($Export.Command) $Destination $($Export.Options)"
    $ArgumentLine = "-OpenTraceFile=`"$TraceFile`" -ABSLOG=`"$Log`" -AutoQuit -NoUI -ExecOnAnalysisCompleteCmd=`"$Command`" -log"
    Write-Output "Exporting $($Export.Name)"
    $Process = Start-Process -FilePath $Insights -ArgumentList $ArgumentLine -WindowStyle Hidden -Wait -PassThru
    if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Destination)) {
        throw "Trace export failed: $($Export.Name); inspect $Log"
    }
}
Write-Output "Render wait events exported to $OutputDirectory (time units: seconds)."
