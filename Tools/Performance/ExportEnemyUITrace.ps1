param(
    [Parameter(Mandatory=$true)][string]$TraceFile,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$EngineRoot = 'E:/program/ue554/UE_5.5',
    [switch]$GameThreadEvents
)
$ErrorActionPreference = 'Stop'
$TraceFile = (Resolve-Path -LiteralPath $TraceFile).Path
$Insights = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealInsights.exe'
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new trace-export directory; existing evidence is retained.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
if ($OutputDirectory -match '\s') { throw 'Use an export path without spaces for engine console arguments.' }
$Exports = @(
    @{Name='Threads'; Command='TimingInsights.ExportThreads'; Options=''},
    @{Name='Timers'; Command='TimingInsights.ExportTimers'; Options=''},
    @{Name='CpuAndGpuTimerStatistics'; Command='TimingInsights.ExportTimerStatistics'; Options='-columns=* -threads=* -timers=*'},
    @{Name='GameThreadTimerStatistics'; Command='TimingInsights.ExportTimerStatistics'; Options='-columns=* -threads=GameThread -timers=*'},
    @{Name='EnemyUITimingEvents'; Command='TimingInsights.ExportTimingEvents'; Options='-columns=ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth -threads=GameThread -timers=FpstrueEnemyUI_*'}
)
if ($GameThreadEvents) {
    # Optional full GT events locate long frames; unlike aggregated cross-thread
    # timers these retain the exact nesting/time relationship. They can be large.
    $Exports += @{Name='GameThreadTimingEvents'; Command='TimingInsights.ExportTimingEvents'; Options='-columns=ThreadId,ThreadName,TimerId,TimerName,StartTime,EndTime,Duration,Depth -threads=GameThread -timers=*'}
}
# The profile starts its file trace immediately before capture and stops it
# before post-capture GC. No unrelated old AutomatedBenchmarkCapture region is
# assumed. Trace startup/flush edges remain visible rather than silently cropped.
foreach ($Export in $Exports) {
    $File = Join-Path $OutputDirectory ($Export.Name+'.csv')
    $Log = Join-Path $OutputDirectory ($Export.Name+'.log')
    $Command = $Export.Command+' '+$File+' '+$Export.Options
    $Arguments = @(('-OpenTraceFile="'+$TraceFile+'"'),('-ABSLOG="'+$Log+'"'),'-AutoQuit','-NoUI',('-ExecOnAnalysisCompleteCmd="'+$Command+'"'))
    $Process = Start-Process -FilePath $Insights -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(60000)) { Stop-Process -Id $Process.Id -Force; throw ('Owned trace exporter timed out: '+$Export.Name) }
    $Process.Refresh()
    if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $File) -or (Get-Item -LiteralPath $File).Length -eq 0) { throw ('Trace export failed: '+$Export.Name) }
    $Process.Dispose()
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'provenance.json'),([ordered]@{
    Trace=$TraceFile; SHA256=(Get-FileHash -LiteralPath $TraceFile -Algorithm SHA256).Hash;
    Boundary='Diagnostic trace only; full traced window, not baseline frametime. Nested CPU timers and parallel CPU/GPU timelines must not be added.'
} | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Write-Output ('Trace evidence exported: '+$OutputDirectory)
