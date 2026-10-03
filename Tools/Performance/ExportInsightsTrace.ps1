param(
    [Parameter(Mandatory = $true)]
    [string]$TraceFile,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$Region = "AutomatedBenchmarkCapture"
)

$ErrorActionPreference = "Stop"

$Insights = "E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealInsights.exe"
$TraceFile = (Resolve-Path -LiteralPath $TraceFile).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$Exports = @(
    [PSCustomObject]@{
        Name = "Threads"
        File = Join-Path $OutputDirectory "Threads.csv"
        Command = "TimingInsights.ExportThreads"
    },
    [PSCustomObject]@{
        Name = "Timers"
        File = Join-Path $OutputDirectory "Timers.csv"
        Command = "TimingInsights.ExportTimers"
    },
    [PSCustomObject]@{
        Name = "GameThreadEvents"
        File = Join-Path $OutputDirectory "GameThreadEvents.csv"
        Command = "TimingInsights.ExportTimingEvents"
        Arguments = "-columns=ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth -threads=GameThread -timers=* -region=$Region"
    },
    [PSCustomObject]@{
        Name = "WorkerEvents"
        File = Join-Path $OutputDirectory "WorkerEvents.csv"
        Command = "TimingInsights.ExportTimingEvents"
        Arguments = "-columns=ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth -threads=Worker*,Foreground* -timers=* -region=$Region"
    },
    [PSCustomObject]@{
        Name = "TimerStatistics"
        File = Join-Path $OutputDirectory "TimerStatistics.csv"
        Command = "TimingInsights.ExportTimerStatistics"
        Arguments = "-columns=* -threads=GameThread,Worker*,Foreground* -timers=* -region=$Region"
    }
)

foreach ($Export in $Exports) {
    $Log = Join-Path $OutputDirectory "$($Export.Name).log"
    $Command = "$($Export.Command) $($Export.File)"
    if ($Export.Arguments) {
        $Command += " $($Export.Arguments)"
    }

    $ArgumentLine = "-OpenTraceFile=`"$TraceFile`" -ABSLOG=`"$Log`" -AutoQuit -NoUI -ExecOnAnalysisCompleteCmd=`"$Command`" -log"
    $Process = Start-Process -FilePath $Insights -ArgumentList $ArgumentLine -WindowStyle Hidden -Wait -PassThru
    if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Export.File)) {
        throw "Insights export failed: $($Export.Name). See $Log"
    }
}

Write-Output "Insights exports written to $OutputDirectory"
