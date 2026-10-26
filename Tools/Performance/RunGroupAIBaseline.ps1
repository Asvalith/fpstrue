<#
Comparable, warmed Editor Game / packaged Shipping client baseline.
The same Editor dedicated server is used for both. One client renders; its peer
uses NullRHI. This is not Shipping Server capacity, WAN, cold-start or gameplay
input acceptance. Existing UI CSV profiles and historical evidence are retained.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:/ueprojrct/fpstrue_safe2',
    [string]$EngineRoot = 'E:/program/ue554/UE_5.5',
    [string]$Map = '/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192',
    [ValidateSet('Editor','Shipping')][string[]]$BuildKinds = @('Editor','Shipping'),
    [string]$ShippingClientExecutable = '',
    [ValidateSet('Client1','Client2')][string]$RenderRole = 'Client1',
    [ValidateSet('Player','Overview')][string]$View = 'Player',
    [ValidateRange(1,1024)][int]$EnemyCount = 160,
    [ValidateSet('Reference','Tiered')][string]$EnemyReplication = 'Tiered',
    [ValidateSet('Reference','Simple')][string]$EnemyHitProxies = 'Reference',
    [ValidateSet('NativeBox','SharedConvex')][string]$HitProxyGeometry = 'NativeBox',
    # Reference preserves the historical launch. Parallel changes only the DS
    # threading gate; it does not force CVars, animation flags or gameplay LOD.
    [ValidateSet('Reference','Parallel')][string[]]$ServerAnimationThreads = @('Reference'),
    # Omission preserves the historical NetDriver configuration. Zero is not
    # uncapped: UE5.5 clamps NetServerMaxTickRate into [1,1000] for a DS.
    [ValidateRange(1,1000)][Nullable[int]]$ServerTickRate = $null,
    # Independent native-renderer experiments, applied only to the rendered
    # client process. No production Config write and no combined overrides.
    [ValidateSet('Inherited','Buffer3','HZB')][string]$ClientOcclusion = 'Inherited',
    # Reverse ablation: restore continuous floor queries once, on the test DS
    # after all 160 enemies are ready. Default preserves authored CMC settings.
    [switch]$ContinuousFloorChecks,
    [ValidateRange(1,5)][int]$Repeat = 3,
    [ValidateRange(1024,65000)][int]$Port = 17871,
    [ValidateRange(180,900)][int]$TimeoutSeconds = 600,
    [string]$RunName = ('GroupAIBaseline_' + (Get-Date -Format 'yyyyMMdd_HHmmss')),
    [string]$OutputRoot = '',
    [switch]$PresentMon,
    [switch]$CaptureScreenshots,
    # Native CPU, pacing and unit observations; optional bounded named WPR.
    # No UE Trace, named events or native CSV profiler. Separate diagnostics.
    [switch]$CpuDiagnostic,
    [switch]$DiagnosticTrace,
    [switch]$DiagnosticTraceOnly,
    # Bounded server call-chain capture; client still records CPU/pacing, not a client Trace.
    [switch]$ServerTraceOnly,
    [switch]$DiagnosticScheduler,
    [string]$DiagnosticRoot = 'C:/UEBuild/FPSDiagnostics',
    [ValidateRange(100,50000)][int]$DiagnosticServerCsvFrames = 2500,
    [ValidateRange(100,50000)][int]$DiagnosticClientCsvFrames = 5000,
    [ValidateRange(30,500)][int]$DiagnosticFlushTimeoutSeconds = 300,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$EnemyReplication=if($EnemyReplication -ieq 'Reference'){'Reference'}else{'Tiered'}
$EnemyHitProxies=if($EnemyHitProxies -ieq 'Simple'){'Simple'}else{'Reference'}
$HitProxyGeometry=if($HitProxyGeometry -ieq 'SharedConvex'){'SharedConvex'}else{'NativeBox'}
$ServerAnimationThreads=@($ServerAnimationThreads | ForEach-Object {if($_ -ieq 'Parallel'){'Parallel'}else{'Reference'}})
$ClientOcclusion=switch($ClientOcclusion.ToLowerInvariant()){'buffer3'{'Buffer3'}'hzb'{'HZB'}default{'Inherited'}}
$CpuAccounting=[bool]($DiagnosticTrace -or $CpuDiagnostic)
$HasDiagnostics=[bool]($DiagnosticTrace -or $CpuDiagnostic)
$TraceRoles=@('Server')
if(-not $ServerTraceOnly){$TraceRoles+=$RenderRole}
if($ServerTraceOnly -and (-not $DiagnosticTrace -or -not $DiagnosticTraceOnly)){
    throw 'ServerTraceOnly requires DiagnosticTrace and DiagnosticTraceOnly.'
}
if($CpuDiagnostic -and ($DiagnosticTrace -or $DiagnosticTraceOnly -or $PresentMon)){
    throw 'CpuDiagnostic does not combine with UE Trace, native CSV profiler or PresentMon.'
}
if($ServerAnimationThreads.Count -eq 0 -or @($ServerAnimationThreads | Select-Object -Unique).Count -ne $ServerAnimationThreads.Count){throw 'Provide one or two distinct server animation-thread modes.'}
$PacingPolicy=[ordered]@{Mode=if($null -eq $ServerTickRate){'Inherited'}else{'ExplicitNetDriverTarget'};
    RequestedServerTickRateHz=$ServerTickRate;
    Boundary='Default launch does not change pacing. Explicit targets override only this disposable DS IpNetDriver ini; t.MaxFPS=0 does not uncap the DS. Targets with different pacing are independent experiments, never pooled with the historical baseline.';
    Readback='Live NetDriver target plus safe UGameEngine max-rate re-evaluation and FApp idle/overshoot. Getter result is not the captured internal pacing decision; independent diagnostic pacing.csv is paired by global frame.'}
if ($RunName -notmatch '^[A-Za-z0-9_-]+$' -or $Map -notmatch '^/Game/[A-Za-z0-9_/]+$') { throw 'Invalid run or map package name.' }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
if ($ShippingClientExecutable) { $ShippingClientExecutable = [IO.Path]::GetFullPath($ShippingClientExecutable) }
$Project = Join-Path $ProjectRoot 'fpstrue.uproject'
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Module = Join-Path $ProjectRoot 'Binaries/Win64/UnrealEditor-fpstrue.dll'
$MapFile = Join-Path $ProjectRoot ('Content/' + $Map.Substring(6) + '.umap')
$OutputRoot = [IO.Path]::GetFullPath($(if($OutputRoot){$OutputRoot}else{Join-Path $ProjectRoot 'Saved/Profiling'}))
if($OutputRoot.TrimEnd('\','/').Length -le 3){throw 'OutputRoot must be an explicit non-root directory.'}
$Run = Join-Path $OutputRoot $RunName
$DiagnosticRoot=[IO.Path]::GetFullPath($DiagnosticRoot)
$DiagnosticRun=Join-Path $DiagnosticRoot $RunName
if($DiagnosticTraceOnly -and -not $DiagnosticTrace){throw 'DiagnosticTraceOnly requires DiagnosticTrace.'}
if($DiagnosticScheduler -and ($PresentMon -or $Repeat -ne 1 -or $BuildKinds.Count -ne 1 -or $ServerAnimationThreads.Count -ne 1 -or
    (-not $CpuDiagnostic -and (-not $DiagnosticTrace -or -not $DiagnosticTraceOnly)))){
    throw 'DiagnosticScheduler requires one build, repeat and threading mode, with CpuDiagnostic or Editor DiagnosticTraceOnly, without PresentMon.'
}
if($DiagnosticTrace -and ($Repeat -ne 1 -or $BuildKinds.Count -ne 1 -or $BuildKinds[0] -ne 'Editor' -or $ServerAnimationThreads.Count -ne 1)){
    throw 'DiagnosticTrace is an independent Editor-only single repeat, never a Shipping or formal repeated baseline.'
}
if(($DiagnosticTrace -or $DiagnosticScheduler) -and ([IO.Path]::GetPathRoot($DiagnosticRoot) -ine 'C:\' -or $DiagnosticRoot.TrimEnd('\').Length -le 3)){
    throw 'DiagnosticRoot must be an explicit non-root C-drive directory for bounded trace storage.'
}
$PresentMonExecutable = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/PresentMon/Win64/PresentMon-1.8.0-x64.exe'
if ('Shipping' -in $BuildKinds -and -not $ShippingClientExecutable) { throw 'Provide the actual packaged Shipping executable, not an Editor or bootstrap.' }
$Cases = @()
for ($Round=1; $Round -le $Repeat; ++$Round) {
    $Order = @($BuildKinds)
    if ($Round % 2 -eq 0) { [array]::Reverse($Order) }
    $ThreadOrder=@($ServerAnimationThreads)
    if($Round % 2 -eq 0){[array]::Reverse($ThreadOrder)}
    foreach ($Kind in $Order) { foreach($Threads in $ThreadOrder){
        $Name=if($ServerAnimationThreads.Count -eq 1 -and $Threads -eq 'Reference'){('{0}_R{1}' -f $Kind,$Round)}else{('{0}_{1}_R{2}' -f $Kind,$Threads,$Round)}
        if($null -ne $ServerTickRate){$Name+=('_ServerTick'+$ServerTickRate)}
        if($ClientOcclusion -ne 'Inherited'){$Name+=('_Client'+$ClientOcclusion)}
        if($ContinuousFloorChecks){$Name+='_FloorReference'}
        if($CpuDiagnostic){$Name+='_CpuDiagnostic'}
        if($ServerTraceOnly){$Name+='_ServerTraceOnly'}
        $Cases += [pscustomobject]@{Kind=$Kind;ServerAnimationThreads=$Threads;ClientOcclusion=$ClientOcclusion;ContinuousFloorChecks=[bool]$ContinuousFloorChecks;CpuDiagnostic=[bool]$CpuDiagnostic;Round=$Round;Name=$Name}
    } }
}
if ($DryRun) {
    [pscustomobject]@{Run=$Run;Map=$Map;Cases=$Cases;Server='Frozen Editor -server';Clients=2;RenderedClients=1;
        Resolution='1600x900';Quality='High / scalability level 2; engine feature defaults retained';WarmupSeconds=20;CaptureSeconds=60;
        Population=$EnemyCount;Seed=1337;View=$View;EnemyReplication=$EnemyReplication;EnemyHitProxies=$EnemyHitProxies;HitProxyGeometry=$HitProxyGeometry;PresentMon=[bool]$PresentMon;ServerAnimationThreads=$ServerAnimationThreads;
        ServerPacing=$PacingPolicy;
        ContinuousFloorChecks=[bool]$ContinuousFloorChecks;FloorCheckBoundary='Reverse ablation only: Server applies bAlwaysCheckFloor=true once after the requested population is ready, before warmup. Default does not write the authored setting. Compare identical six-proxy geometry, build and renderer/pacing settings; never mix another geometry or old two-proxy results into the floor-cache control.';
        ClientOcclusion=$ClientOcclusion;ClientOcclusionBoundary='Rendered client only: Inherited changes nothing; Buffer3 overrides only r.NumBufferedOcclusionQueries=3; HZB overrides only r.HZBOcclusion=1. Native CVar readback must confirm the requested value. These are independent renderer experiments, not AI/network changes.';
        AnimationThreading='Reference adds no engine threading option; Parallel adds -useperfthreads to the Editor dedicated server only. Runtime gates are snapshots, not proof of Worker dispatch.';
        NetworkCounters='Per-role START/STOP parent-connection In/OutTotalBytes/Packets, including engine connection/PacketOverhead accounting; stable identities, no wrap/reset/reconnect concatenation; not NIC/per-Actor traffic.';
        Diagnostic=[ordered]@{Enabled=$HasDiagnostics;DiagnosticOnly=$HasDiagnostics;FormalBaseline=(-not $HasDiagnostics);TraceEnabled=[bool]$DiagnosticTrace;TraceRoles=@($TraceRoles | Where-Object {$DiagnosticTrace});ServerTraceOnly=[bool]$ServerTraceOnly;CpuOnly=([bool]$CpuDiagnostic -and -not $DiagnosticScheduler);TraceOnly=[bool]$DiagnosticTraceOnly;Output=if($CpuDiagnostic){$Run}else{$DiagnosticRun};ServerCsvFrames=if($CpuDiagnostic){$null}else{$DiagnosticServerCsvFrames};
            CpuAccounting=$CpuAccounting;Scheduler=[ordered]@{Requested=[bool]$DiagnosticScheduler;Recorder='One named WPR CPU.Verbose.File instance; UE contextswitch/stacksampling remain disabled';RequiresAdministrator=$true;
                Analysis='ETL existence is not proof of complete Running/Ready/Wait data; verify coverage, PID/TID mappings, lost events and QPC alignment offline.'};
            RenderedClientCsvFrames=if($CpuDiagnostic){$null}else{$DiagnosticClientCsvFrames};FlushTimeoutSeconds=$DiagnosticFlushTimeoutSeconds;
            Budget=if($DiagnosticScheduler){'Native CPU/unit observations plus one bounded WPR recording. Reserve at least 5 GiB on C before each capture; ETL coverage and symbols require offline validation. Never merge this diagnostic into the formal baseline.'}elseif($CpuDiagnostic){'Direct native GameThread CPU queries, bounded pacing.csv and engine unit observation add measurement overhead. No UE Trace, named events, native CSV profiler or ETL. Never merge this diagnostic into the formal baseline.'}else{'Trace/named events (and optional native CSV) add CPU/memory/disk overhead. Reserve at least 5 GiB on C; traces may consume several GiB. Never merge diagnostic timings into the formal baseline.'};
            Window=if($CpuDiagnostic){'Native OnEndFrame CPU deltas, selected-World CPU and engine unit callback samples; global Frame pairs callback coverage, not GPU execution identity. Missing/inapplicable unit counters remain null. TickFlush timer is requested on server and rendered client, but Shipping may ignore the client ini: readback controls availability, not zero-cost substitution. Server timer remains mandatory. Optional WPR needs independent QPC/PID/TID alignment before worker or OS Ready/Blocked attribution.'}elseif($DiagnosticTraceOnly){'Native CSV profiler intentionally disabled; engine unit observations remain separate. Trace window alignment requires offline validation, not an assumed FrameIndex.'}else{'Native CSV must finalize; exact baseline/native/Trace frame alignment is an offline validation, not an assumed FrameIndex.'}};
        ExitPolicy='Independent per-role controls; release clients and verify COMPLETE + natural exit 0 before releasing the server.';
        Boundary='Separate builds; no pooled timings; FrameEndInterval is not presentation latency.'} | ConvertTo-Json -Depth 5
    return
}
if($DiagnosticScheduler){
    $SchedulerIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $SchedulerPrincipal=[Security.Principal.WindowsPrincipal]::new($SchedulerIdentity)
    if(-not $SchedulerPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'DiagnosticScheduler requires an Administrator PowerShell. Do not enable UE contextswitch as a non-elevated substitute.'}
    $WprExecutable=(Get-Command wpr.exe -ErrorAction Stop).Source
    $XperfExecutable=(Get-Command xperf.exe -ErrorAction Stop).Source
}
foreach ($Path in @($Project,$Editor,$Module,$MapFile)) { if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing input: $Path" } }
if ('Shipping' -in $BuildKinds -and -not (Test-Path -LiteralPath $ShippingClientExecutable -PathType Leaf)) { throw 'Packaged Shipping executable missing.' }
if (Test-Path -LiteralPath $Run) { throw 'Do not overwrite earlier baseline evidence.' }
if($DiagnosticTrace -or $DiagnosticScheduler){
    if(Test-Path -LiteralPath $DiagnosticRun){throw 'Do not overwrite an existing diagnostic trace directory.'}
    if((Get-PSDrive -Name C -PSProvider FileSystem).Free -lt 5GB){throw 'Diagnostic disk guard: C must retain at least 5 GiB.'}
    $null=New-Item -ItemType Directory -Path $DiagnosticRun
}
$null = New-Item -ItemType Directory -Path $Run
function Write-BaselineJson($Value,[string]$Path) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false)) }

# One named WPR owner records all game PIDs. UE5.5's per-process ETW tracer
# stops the global Kernel Logger, so never add contextswitch/stacksampling to
# the individual clients or combine them with this recorder.
function Invoke-SchedulerWpr($Scheduler,[string[]]$Arguments,[string]$Operation) {
    $LogPrefix=Join-Path $Scheduler.Directory $Operation
    $ArgumentLine=(@($Arguments | ForEach-Object {'"'+($_ -replace '"','')+'"'}) -join ' ')
    $Cli=Start-Process -FilePath $WprExecutable -ArgumentList $ArgumentLine -WindowStyle Hidden -PassThru -RedirectStandardOutput ($LogPrefix+'.stdout.txt') -RedirectStandardError ($LogPrefix+'.stderr.txt')
    $Scheduler.CliProcess=$Cli
    $Clock=[Diagnostics.Stopwatch]::StartNew()
    while(-not $Cli.WaitForExit(500)){
        if($Clock.Elapsed.TotalSeconds -gt 120){
            $Scheduler.CliStillRunning=$true
            throw ('WPR '+$Operation+' exceeded 120 seconds; do not launch another stop/cancel while owned CLI PID '+$Cli.Id+' is still running.')
        }
    }
    $Scheduler.CliStillRunning=$false
    $Scheduler.CliProcess=$null
    $Code=$Cli.ExitCode; $Cli.Dispose()
    if($Code -ne 0){throw ('WPR '+$Operation+' failed with exit code '+$Code+'; preserve its logs, never stop a foreign recording.')}
    return [IO.File]::ReadAllText($LogPrefix+'.stdout.txt')
}
function Start-SchedulerRecording($Scheduler) {
    if((Get-PSDrive -Name C -PSProvider FileSystem).Free -lt 5GB){throw 'Scheduler disk guard: C must retain at least 5 GiB before each recording.'}
    $DefaultStatus=Invoke-SchedulerWpr $Scheduler @('-status') 'preflight-default-status'
    $OwnStatus=Invoke-SchedulerWpr $Scheduler @('-status','profiles','collectors','-details','-instancename',$Scheduler.Instance) 'preflight-own-status'
    if($DefaultStatus -notmatch 'WPR is not recording' -or $OwnStatus -notmatch 'WPR is not recording'){
        throw 'An existing WPR recording or unknown status was found. Leave it untouched.'
    }
    $LoggerText=@(& $XperfExecutable -loggers 2>&1) -join "`n"
    $LoggerCode=$LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $Scheduler.Directory 'preflight-loggers.txt'),$LoggerText,[Text.UTF8Encoding]::new($false))
    if($LoggerCode -ne 0 -or $LoggerText -match '(?i)NT Kernel Logger|WPR_initiated_|Windows Kernel Trace'){
        throw 'A kernel/WPR recorder already exists or the logger inventory failed; do not stop or replace it.'
    }
    $Scheduler.StartQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    $Scheduler.StartAttempted=$true
    $null=Invoke-SchedulerWpr $Scheduler @('-start','CPU','-filemode','-recordtempto',$Scheduler.Temporary,'-instancename',$Scheduler.Instance) 'start'
    $Scheduler.Started=$true
    $Status=Invoke-SchedulerWpr $Scheduler @('-status','profiles','collectors','-details','-instancename',$Scheduler.Instance) 'started-status'
    if($Status -match 'WPR is not recording' -or $Status -notmatch '(?i)CPU\.Verbose\.File'){
        throw 'The owned WPR recorder did not confirm an active CPU profile.'
    }
    Write-Output ('Scheduler recording active: '+$Scheduler.Instance)
}
function Stop-SchedulerRecording($Scheduler) {
    if(-not $Scheduler.Started -or $Scheduler.StopAttempted){return}
    $Scheduler.StopAttempted=$true
    $Scheduler.StopQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    $null=Invoke-SchedulerWpr $Scheduler @('-stop',$Scheduler.ETL,('FPS '+$EnemyCount+' AI execution and scheduling diagnostic'),'-skipPdbGen','-compress','-instancename',$Scheduler.Instance) 'stop'
    if(-not (Test-Path -LiteralPath $Scheduler.ETL -PathType Leaf) -or (Get-Item -LiteralPath $Scheduler.ETL).Length -le 0){throw 'WPR reported success without a nonempty owned ETL.'}
    $Status=Invoke-SchedulerWpr $Scheduler @('-status','profiles','collectors','-details','-instancename',$Scheduler.Instance) 'stopped-status'
    if($Status -notmatch 'WPR is not recording'){throw 'The owned WPR instance did not confirm it stopped; leave all other recorders untouched.'}
    $Scheduler.Stopped=$true
    $Scheduler.Bytes=(Get-Item -LiteralPath $Scheduler.ETL).Length
}
function Get-SchedulerEvidence($Scheduler) {
    [ordered]@{Requested=$true;Instance=$Scheduler.Instance;Profile='CPU.Verbose.File';StartAttempted=$Scheduler.StartAttempted;Started=$Scheduler.Started;Stopped=$Scheduler.Stopped;
        StartQpc=$Scheduler.StartQpc;StopQpc=$Scheduler.StopQpc;QpcFrequency=[Diagnostics.Stopwatch]::Frequency;ETL=$Scheduler.ETL;Bytes=$Scheduler.Bytes;
        CliStillRunning=$Scheduler.CliStillRunning;ETLEventCoverageVerified=$false;LostEventsVerified=$false;ClockAlignmentVerified=$false;
        StopAttempted=$Scheduler.StopAttempted;CliPid=if($null -ne $Scheduler.CliProcess){$Scheduler.CliProcess.Id}else{$null};
        Recovery='After the recorded owned CLI has exited, inspect only this instance with: wpr -status profiles collectors -details -instancename '+$Scheduler.Instance;
        Boundary='One owned WPR instance; no per-process UE ETW channels. ETL event coverage, lost events, PID/TID identities and UE/QPC alignment must be validated before reporting on-core/ready/blocked.'}
}
function New-PrivateUserDirectory([string]$Path) {
    $null = New-Item -ItemType Directory -Path $Path
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $Acl = [Security.AccessControl.DirectorySecurity]::new()
    $Acl.SetOwner($Identity); $Acl.SetAccessRuleProtection($true,$false)
    $Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($Identity,[Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',[Security.AccessControl.PropagationFlags]::None,[Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $Path -AclObject $Acl
}
function Read-BaselineStatus([string]$Path) {
    try {
        $Stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
        $Reader=[IO.StreamReader]::new($Stream)
        try { $Text=$Reader.ReadToEnd() } finally { $Reader.Dispose() }
        return ($Text | ConvertFrom-Json -ErrorAction Stop)
    } catch [IO.IOException] { return $null } catch [System.Management.Automation.ItemNotFoundException] { return $null }
}
function Write-BaselineControl([string]$Path,[ValidateSet('WAIT','START','STOP','EXIT')][string]$Value) {
    $Temporary=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($Temporary,$Value,[Text.UTF8Encoding]::new($false))
        for ($Attempt=0; $Attempt -lt 40; ++$Attempt) {
            try {
                if ([IO.File]::Exists($Path)) { [IO.File]::Replace($Temporary,$Path,[NullString]::Value) } else { [IO.File]::Move($Temporary,$Path) }
                return
            } catch [IO.IOException] { if ($Attempt -eq 39) { throw }; Start-Sleep -Milliseconds 25 }
        }
    } finally { if ([IO.File]::Exists($Temporary)) { [IO.File]::Delete($Temporary) } }
}
function Read-NativeCsvCompletion([string]$UserDirectory,[string]$LogPath) {
    # Only this fresh role-owned UserDir is searched. A file existing or having
    # some numeric rows does not establish FCsvProfiler::Finalize completion.
    $Files=@(Get-ChildItem -LiteralPath $UserDirectory -File -Recurse -Filter '*.csv' -ErrorAction Stop)
    if($Files.Count -eq 0){return $null}
    if($Files.Count -ne 1){throw 'Expected exactly one native CSV in this fresh diagnostic role UserDir.'}
    if(-not (Test-Path -LiteralPath $LogPath)){return $null}
    try {
        $LogStream=[IO.File]::Open($LogPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
        $LogReader=[IO.StreamReader]::new($LogStream)
        try{$LogText=$LogReader.ReadToEnd()}finally{$LogReader.Dispose()}
    } catch [IO.IOException] {return $null}
    if($LogText -notmatch 'LogCsvProfiler:.*Capture Ended\. Writing CSV to file' -or $LogText -notmatch 'LogCsvProfiler:.*CSV finalize time'){return $null}
    try {
        $Stream=[IO.File]::Open($Files[0].FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
        try {
            $null=$Stream.Seek([Math]::Max([long]0,$Stream.Length-131072),[IO.SeekOrigin]::Begin)
            $Reader=[IO.StreamReader]::new($Stream)
            try{$Tail=$Reader.ReadToEnd()}finally{$Reader.Dispose()}
        } finally {$Stream.Dispose()}
    } catch [IO.IOException] {return $null}
    if($Tail -notmatch '(?i)\[HasHeaderRowAtEnd\],1(?:,|\r?\n)' -or
        $Tail -notmatch '(?i)\[captureduration\],([0-9.eE+-]+)'){return $null}
    $Duration=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
    if($Tail -notmatch '(?i)\[starttimestamp\],([0-9]+)'){return $null};$StartUnix=[long]$Matches[1]
    if($Tail -notmatch '(?i)\[endtimestamp\],([0-9]+)'){return $null};$EndUnix=[long]$Matches[1]
    if([double]::IsNaN($Duration) -or [double]::IsInfinity($Duration) -or $Duration -lt 80 -or $EndUnix -lt $StartUnix){
        throw 'Native CSV finalized without a sufficient diagnostic window; exact alignment must still be checked offline.'
    }
    [pscustomobject]@{Path=$Files[0].FullName;Bytes=$Files[0].Length;CaptureDurationSeconds=$Duration;
        StartTimestampUnix=$StartUnix;EndTimestampUnix=$EndUnix;Finalized=$true;
        Alignment='Pending offline continuous FrameTime sequence matching; native CSV has no global GFrameCounter column.'}
}
function Get-BaselineFingerprint {
    $Paths=@($Project,$Module,$MapFile,(Join-Path $ProjectRoot 'Config/DefaultEngine.ini'),(Join-Path $ProjectRoot 'Config/DefaultGame.ini'),$PSCommandPath)
    $SourceInputs=@(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'Source/fpstrue') -File -Recurse)
    $Paths += @($SourceInputs | ForEach-Object FullName)
    # Also freeze literal native asset defaults (player/enemy BPs, meshes, ABPs,
    # weapon data), not merely the map. This is a tracked-input manifest, not an
    # assertion that every unused art asset in Content was recursively hashed.
    foreach($SourceInput in $SourceInputs | Where-Object Extension -in @('.h','.cpp')) {
        foreach($NativeAsset in [regex]::Matches([IO.File]::ReadAllText($SourceInput.FullName),'/Game/[A-Za-z0-9_/]+(?:\.[A-Za-z0-9_]+)?')) {
            $Package=($NativeAsset.Value -split '\.')[0]
            $AssetFile=Join-Path $ProjectRoot ('Content/'+$Package.Substring(6)+'.uasset')
            if(Test-Path -LiteralPath $AssetFile -PathType Leaf){$Paths+=$AssetFile}
        }
    }
    $TerrainDirectory=Split-Path -Parent $MapFile
    $Paths += @(Get-ChildItem -LiteralPath $TerrainDirectory -File -Filter '*.uasset' | ForEach-Object FullName)
    if ('Shipping' -in $BuildKinds) {
        $Paths += $ShippingClientExecutable
        $PackagedProjectRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ShippingClientExecutable))
        $PakRoot=Join-Path $PackagedProjectRoot 'Content/Paks'
        $Containers=@(Get-ChildItem -LiteralPath $PakRoot -File -ErrorAction Stop | Where-Object Extension -in @('.pak','.utoc','.ucas'))
        if($Containers.Count -eq 0){throw 'Shipping data containers missing; do not profile a bare executable.'}
        $Paths += @($Containers | ForEach-Object FullName)
    }
    @($Paths | Sort-Object -Unique | ForEach-Object {[pscustomobject]@{Path=$_;SHA256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
}
function Get-BaselineCompetitors([int[]]$OwnedIds) {
    @(Get-Process -Name UnrealEditor,UnrealEditor-Cmd,fpstrue,fpstrue-Win64-Shipping,fpstrue-Win64-Development,UnrealInsights,UnrealBuildTool,UnrealHeaderTool,cl,link,ShaderCompileWorker -ErrorAction SilentlyContinue |
        Where-Object {-not $_.HasExited -and $_.Id -notin $OwnedIds})
}
function Save-BaselineCompetitorEvidence($Conflicts,[string]$CaseRoot) {
    $Identifiers=@($Conflicts | ForEach-Object Id)
    # Do not capture another task's full command line: it may contain tickets.
    $Live=@(Get-CimInstance Win32_Process | Where-Object ProcessId -in $Identifiers)
    $Evidence=@(foreach($Identifier in $Identifiers){
        $Peer=$Live | Where-Object ProcessId -eq $Identifier | Select-Object -First 1
        [pscustomobject]@{Id=$Identifier;Name=if($Peer){$Peer.Name}else{'ExitedBeforeDetails'};
            Executable=if($Peer){$Peer.ExecutablePath}else{$null};ParentId=if($Peer){$Peer.ParentProcessId}else{$null};
            Started=if($Peer){$Peer.CreationDate.ToString('o')}else{$null};Detected=(Get-Date).ToString('o')}
    })
    Write-BaselineJson $Evidence (Join-Path $CaseRoot 'conflicts.json')
}
function Get-Percentile([double[]]$Values,[double]$Fraction) {
    $Sorted=@($Values | Sort-Object)
    $Sorted[[Math]::Max(0,[Math]::Min($Sorted.Count-1,[Math]::Ceiling($Fraction*$Sorted.Count)-1))]
}
function Assert-BaselineNetworkCounters($Summary,[string]$Role) {
    $Network=$Summary.networkConnections
    $Expected=if($Role -eq 'Server'){2}else{1}
    $Required=@('valid','driverObjectId','driverName','expectedConnections','identitySamples','startSeconds','stopSeconds','windowSeconds','startQpc','stopQpc','secondsPerCycle64','connections','totals','accounting')
    if($null -eq $Network -or @($Required | Where-Object {$_ -cnotin $Network.PSObject.Properties.Name}).Count -or
        $Network.valid -ne $true -or $Network.expectedConnections -ne $Expected -or @($Network.connections).Count -ne $Expected -or
        $Network.identitySamples -ne $Summary.validationSamples -or $Network.identitySamples -lt 100 -or
        [string]::IsNullOrWhiteSpace($Network.driverName) -or [string]$Network.driverObjectId -notmatch '^\d+$'){
        throw ('Missing or invalid stable network connection coverage: '+$Role)
    }
    foreach($Field in @('startSeconds','stopSeconds','windowSeconds','secondsPerCycle64')){
        if($null -eq $Network.$Field -or [double]::IsNaN([double]$Network.$Field) -or [double]::IsInfinity([double]$Network.$Field)){throw ('Invalid network clock: '+$Role)}
    }
    $Duration=[double]$Network.windowSeconds
    if($Duration -lt 59.8 -or $Duration -gt 75 -or [Math]::Abs($Duration-[double]$Summary.captureSeconds) -gt 0.25 -or
        [Math]::Abs(([double]$Network.stopSeconds-[double]$Network.startSeconds)-$Duration) -gt 0.00001 -or
        [string]$Network.startQpc -notmatch '^\d+$' -or [string]$Network.stopQpc -notmatch '^\d+$' -or
        [uint64]$Network.stopQpc -le [uint64]$Network.startQpc -or [double]$Network.secondsPerCycle64 -le 0 -or
        [Math]::Abs(([uint64]$Network.stopQpc-[uint64]$Network.startQpc)*[double]$Network.secondsPerCycle64-$Duration) -gt 0.01){throw ('Network counter window mismatch: '+$Role)}
    $Ids=@{}
    $Totals=@{inBytes=[long]0;outBytes=[long]0;inPackets=[long]0;outPackets=[long]0}
    foreach($Connection in @($Network.connections)){
        if($null -eq $Connection -or [string]$Connection.objectId -notmatch '^\d+$' -or [string]$Connection.engineConnectionId -notmatch '^\d+$' -or
            [string]::IsNullOrWhiteSpace($Connection.objectName) -or [string]::IsNullOrWhiteSpace($Connection.remoteAddress) -or $Ids.ContainsKey([string]$Connection.objectId)){
            throw ('Duplicate or invalid network identity: '+$Role)
        }
        $Ids[[string]$Connection.objectId]=$true
        foreach($Name in @('inBytes','outBytes','inPackets','outPackets')){
            $Counter=$Connection.$Name
            if($null -eq $Counter -or @('start','end','delta','perSecond' | Where-Object {$_ -cnotin $Counter.PSObject.Properties.Name}).Count){throw ('Missing network counter '+$Role+'/'+$Name)}
            foreach($Field in @('start','end','delta')){
                if([string]$Counter.$Field -notmatch '^\d+$' -or [long]$Counter.$Field -gt [int]::MaxValue){throw ('Negative/overflowed native int32 network counter: '+$Role+'/'+$Name)}
            }
            $Delta=[long]$Counter.end-[long]$Counter.start
            if($Delta -lt 0 -or $Delta -ne [long]$Counter.delta){throw ('Reset/regressed network counter: '+$Role+'/'+$Name)}
            $ExpectedRate=$Delta/$Duration
            if($null -eq $Counter.perSecond -or [double]::IsNaN([double]$Counter.perSecond) -or [double]::IsInfinity([double]$Counter.perSecond) -or
                [Math]::Abs([double]$Counter.perSecond-$ExpectedRate) -gt [Math]::Max(0.00001,[Math]::Abs($ExpectedRate)*1e-8)){throw ('Network rate mismatch: '+$Role+'/'+$Name)}
            $Totals[$Name]+=$Delta
        }
    }
    foreach($Name in @('inBytes','outBytes','inPackets','outPackets')){
        $Total=$Network.totals.$Name
        $ExpectedRate=$Totals[$Name]/$Duration
        if($null -eq $Total -or [string]$Total.delta -notmatch '^\d+$' -or [long]$Total.delta -ne $Totals[$Name] -or
            $null -eq $Total.perSecond -or [double]::IsNaN([double]$Total.perSecond) -or [double]::IsInfinity([double]$Total.perSecond) -or
            [Math]::Abs([double]$Total.perSecond-$ExpectedRate) -gt [Math]::Max(0.00001,[Math]::Abs($ExpectedRate)*1e-8)){
            throw ('Network role total mismatch: '+$Role+'/'+$Name)
        }
    }
    return $Network
}
function Assert-BaselineHitProxyGeometry($Snapshot,[string]$Role) {
    $ExpectedProxies=$EnemyCount*6
    $ExpectedQueries=if($Role -eq 'Server'){$ExpectedProxies}else{0}
    $HitProxies=$Snapshot.hitProxyComponents
    if($null -eq $HitProxies -or $HitProxies.schemaVersion -ne 3 -or $HitProxies.geometry -cne $HitProxyGeometry -or
        $HitProxies.count -ne $ExpectedProxies -or $HitProxies.minPerEnemy -ne 6 -or $HitProxies.maxPerEnemy -ne 6 -or
        $HitProxies.unexpectedTaggedComponents -ne 0 -or $HitProxies.queryEnabled -ne $ExpectedQueries -or
        $HitProxies.physicsStatesCreated -ne $ExpectedQueries -or $HitProxies.invalidBodySetups -ne 0 -or
        $HitProxies.otherElements -ne 0 -or $Snapshot.mesh.collisionEnabled -ne 0 -or $Snapshot.mesh.validBodyInstances -ne 0){
        throw ('Six homogeneous hit proxies did not replace live skeletal collision: '+$Role)
    }
    if($HitProxyGeometry -eq 'NativeBox'){
        # Read-only observation: a non-query client may have no allocated setup.
        if($HitProxies.nativeBoxComponents -ne $ExpectedProxies -or $HitProxies.staticMeshComponents -ne 0 -or $HitProxies.uniqueHullAssets -ne 0 -or
            $HitProxies.bodySetupsPresent -lt $ExpectedQueries -or $HitProxies.bodySetupsPresent -gt $ExpectedProxies -or
            $HitProxies.boxElements -ne $HitProxies.bodySetupsPresent -or $HitProxies.convexElements -ne 0){
            throw ('Expected exactly one native box per allocated proxy setup: '+$Role)
        }
    } elseif($HitProxies.nativeBoxComponents -ne 0 -or $HitProxies.staticMeshComponents -ne $ExpectedProxies -or $HitProxies.uniqueHullAssets -ne 1 -or
        $HitProxies.bodySetupsPresent -ne $ExpectedProxies -or $HitProxies.boxElements -ne 0 -or $HitProxies.convexElements -ne $ExpectedProxies){
        throw ('Expected exactly one convex element per shared-mesh proxy setup: '+$Role)
    }
}
function Assert-BaselineNativeUnits($Summary,[string]$Role,[string]$CaseRoot,$FrameRows) {
    $Units=$Summary.nativeUnits
    $UnitFile=Join-Path $CaseRoot ($Role+'.units.csv')
    if($null -eq $Units -or $Units.schemaVersion -ne 1 -or $Units.enabled -isnot [bool] -or -not $Units.enabled -or
        $Units.valid -isnot [bool] -or -not $Units.valid -or $Units.recordedFrames -ne $Summary.frames -or
        $Units.matchedSamples -ne $Summary.frames -or $Units.missingFrames -ne 0 -or $Units.duplicateFrameSamples -ne 0 -or
        $Units.extraSamples -lt 0 -or $Units.sourceSamples -ne ($Units.matchedSamples+$Units.extraSamples) -or
        $Units.tickFlushTimerEnabled -isnot [bool] -or
        ($Role -eq 'Server' -and (-not $Units.tickFlushTimerEnabled -or $Units.tickFlushReportCvar -ne 1)) -or
        -not (Test-Path -LiteralPath $UnitFile -PathType Leaf)){
        throw ('Missing native unit callback coverage or requested TickFlush timer: '+$Role)
    }
    $UnitRows=@(Import-Csv -LiteralPath $UnitFile)
    $Columns='Frame,Qpc,ElapsedSeconds,RecordedFrameMatch,NativeDeltaSeconds,NativeTrueDeltaSeconds,GameThreadMs,RenderThreadMs,RHIThreadMs,GPUFrameMs,IdleMs,IdleOvershootMs,GameDriverTickFlushMs,DemoDriverTickFlushMs,FlushAsyncLoadingMs,FlushAsyncLoadingCount,SyncLoadCount,NumAsyncPackages,NativeBinThisFrame'
    if($UnitRows.Count -ne $Units.sourceSamples -or $UnitRows.Count -eq 0 -or ($UnitRows[0].PSObject.Properties.Name -join ',') -cne $Columns){
        throw ('Missing or mismatched native unit CSV schema: '+$Role)
    }
    $ExpectedFrames=@{};foreach($FrameRow in $FrameRows){$ExpectedFrames[[string]$FrameRow.Frame]=$true}
    $SeenFrames=@{};$MatchedRows=@()
    foreach($UnitRow in $UnitRows){
        if([string]$UnitRow.Frame -notmatch '^\d+$' -or [string]$UnitRow.Qpc -notmatch '^\d+$' -or [uint64]$UnitRow.Qpc -eq 0 -or
            $SeenFrames.ContainsKey([string]$UnitRow.Frame) -or $UnitRow.RecordedFrameMatch -cnotin @('0','1') -or
            ($UnitRow.RecordedFrameMatch -eq '1') -ne $ExpectedFrames.ContainsKey([string]$UnitRow.Frame)){
            throw ('Native unit CSV frame identity or duplicate mismatch: '+$Role)
        }
        $SeenFrames[[string]$UnitRow.Frame]=$true
        if($UnitRow.RecordedFrameMatch -eq '1'){$MatchedRows+=$UnitRow}
    }
    if($MatchedRows.Count -ne $Summary.frames){throw ('Native unit CSV does not cover recorded frames: '+$Role)}
    $MetricNames=@('GameThreadMs','RenderThreadMs','RHIThreadMs','GPUFrameMs','IdleMs','IdleOvershootMs','GameDriverTickFlushMs','DemoDriverTickFlushMs','FlushAsyncLoadingMs')
    foreach($MetricName in $MetricNames){
        $Metric=$Units.metrics.$MetricName
        $ZeroIsMissing=$MetricName -in @('GameThreadMs','RenderThreadMs','RHIThreadMs','GPUFrameMs')
        $Applicable=(-not $ZeroIsMissing -or ($Role -eq $RenderRole -and -not $Summary.nullRhi)) -and
            ($MetricName -ne 'GameDriverTickFlushMs' -or $Units.tickFlushTimerEnabled)
        $Finite=0;$Positive=0;$Zero=0;$Total=0.0
        foreach($UnitRow in $MatchedRows){
            $Value=0.0
            if([double]::TryParse([string]$UnitRow.$MetricName,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$Value) -and
                -not [double]::IsNaN($Value) -and -not [double]::IsInfinity($Value) -and $Value -ge 0){
                ++$Finite;$Total+=$Value;if($Value -gt 0){++$Positive}else{++$Zero}
            }
        }
        $Complete=$Applicable -and $Finite -eq $MatchedRows.Count -and (-not $ZeroIsMissing -or $Positive -eq $MatchedRows.Count)
        if($null -eq $Metric -or $Metric.applicable -isnot [bool] -or $Metric.applicable -ne $Applicable -or
            $Metric.complete -isnot [bool] -or $Metric.complete -ne $Complete -or $Metric.zeroMayMeanUnavailable -isnot [bool] -or
            $Metric.zeroMayMeanUnavailable -ne $ZeroIsMissing -or $Metric.finiteSamples -ne $Finite -or $Metric.positiveSamples -ne $Positive -or
            $Metric.zeroSamples -ne $Zero -or $Metric.invalidSamples -ne ($MatchedRows.Count-$Finite)){
            throw ('Native unit metric coverage contradicts its CSV: '+$Role+'/'+$MetricName)
        }
        # Unavailable thread/GPU counters stay null, never turn into zero cost.
        if($Complete){
            if($null -eq $Metric.meanMs -or [double]::IsNaN([double]$Metric.meanMs) -or [double]::IsInfinity([double]$Metric.meanMs) -or
                [Math]::Abs([double]$Metric.meanMs-$Total/$MatchedRows.Count) -gt 0.000001){throw ('Native unit mean contradicts its CSV: '+$Role+'/'+$MetricName)}
        } elseif($null -ne $Metric.meanMs){throw ('Unavailable native unit metric must remain null: '+$Role+'/'+$MetricName)}
    }
    return [ordered]@{File=$UnitFile;Samples=$UnitRows.Count;TickFlushTimerRequested=1;Observation=$Units;
        Boundary='Exact callback-frame coverage only; unit counters can lag/repeat and are not execution CPU, OS scheduler states or interchangeable GPU frame identities.'}
}
$Fingerprint=Get-BaselineFingerprint
$FingerprintText=$Fingerprint | ConvertTo-Json -Depth 4 -Compress
$Experiment=[ordered]@{Created=(Get-Date).ToString('o');Map=$Map;Cases=$Cases;Fingerprint=$Fingerprint;WarmupSeconds=20;CaptureSeconds=60;
    Resolution='1600x900';Quality=2;Population=$EnemyCount;Clients=2;RenderedRole=$RenderRole;Seed=1337;View=$View;EnemyReplication=$EnemyReplication;EnemyHitProxies=$EnemyHitProxies;HitProxyGeometry=$HitProxyGeometry;ServerAnimationThreads=$ServerAnimationThreads;
    AnimationThreading='Reference preserves historical engine defaults; Parallel adds only DS -useperfthreads. No per-instance/CVar forcing; compare actual runtime gate snapshots and independent traces.';
    ServerPacing=$PacingPolicy;
    ContinuousFloorChecks=[bool]$ContinuousFloorChecks;
    ClientOcclusion=$ClientOcclusion;
    NetworkCounters='Stable parent connections, START/STOP cumulative deltas; engine connection accounting incl. PacketOverhead, not NIC or per-Actor bytes. Server retains relevance for the full requested AI population. Replication, hit-proxy and threading modes are recorded separately; change one factor per comparison.';
    Backend='Same Editor dedicated server for Editor and Shipping client cases';
    ExitPolicy='Independent per-role controls; clients COMPLETE + natural exit 0, then server EXIT';
    Diagnostic=$HasDiagnostics;DiagnosticOnly=$HasDiagnostics;FormalBaseline=(-not $HasDiagnostics);CpuDiagnostic=[bool]$CpuDiagnostic;TraceDiagnostic=[bool]$DiagnosticTrace;
    TraceRoles=@($TraceRoles | Where-Object {$DiagnosticTrace});ServerTraceOnly=[bool]$ServerTraceOnly;
    Boundary='Warmed fixed-view baseline. Whole-process local competition retained. No WAN, all-AI-attacking, Shipping DS capacity or input/visual-quality claim.';
    Hardware=@{CPU=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);
        GPU=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);RAM=@(Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory)}}
Write-BaselineJson $Experiment (Join-Path $Run 'experiment.json')
$Results=@()
$ReferenceView=$null
$ReferenceQuality=$null
$ReferenceFov=$null
foreach ($Case in $Cases) {
    $CaseRoot=Join-Path $Run $Case.Name
    $null=New-Item -ItemType Directory -Path $CaseRoot
    $Owned=@();$PmProcess=$null;$Failure=$null;$Phase='Ready';$PhaseAt=Get-Date;$Started=Get-Date
    # Keep the authority alive until both clients have exited normally. Releasing
    # one shared EXIT lets host closure race a client's final ticker and trigger
    # normal disconnect/default-map loading after its samples were already flushed.
    $Controls=@{}
    foreach($Role in @('Server','Client1','Client2')){$Controls[$Role]=Join-Path $CaseRoot ($Role+'.control.txt')}
    $Result=[ordered]@{Name=$Case.Name;Kind=$Case.Kind;Round=$Case.Round;EnemyReplication=$EnemyReplication;EnemyHitProxies=$EnemyHitProxies;HitProxyGeometry=$HitProxyGeometry;ServerAnimationThreads=$Case.ServerAnimationThreads;ServerPacing=$PacingPolicy;ClientOcclusion=$ClientOcclusion;ContinuousFloorChecks=[bool]$ContinuousFloorChecks;CpuDiagnostic=[bool]$CpuDiagnostic;DiagnosticOnly=$HasDiagnostics;FormalBaseline=(-not $HasDiagnostics);Valid=$false;Error=$null;Processes=@();Roles=@{};PresentMon=$null}
    $DiagnosticInfo=$null;$DiagnosticCase=$null;$DiagnosticManifestPath=$null;$ObservedRolePhases=@{};$Scheduler=$null
    if($CpuDiagnostic){
        $DiagnosticCase=$CaseRoot
        $DiagnosticManifestPath=Join-Path $CaseRoot 'cpu-diagnostic-manifest.json'
        $DiagnosticInfo=[ordered]@{SchemaVersion=2;Case=$Case.Name;HitProxyGeometry=$HitProxyGeometry;DiagnosticOnly=$true;FormalBaseline=$false;CpuOnly=(-not $DiagnosticScheduler);TraceEnabled=$false;SchedulerRequested=[bool]$DiagnosticScheduler;
            NativeCsvPolicy='No engine CSV profiler or UE Trace requested. Native frames.csv CPU accounting, pacing.csv and engine units.csv observation; optional WPR is separate scheduling evidence.';
            StartedUtc=[datetime]::UtcNow.ToString('o');Roles=@{};
            Boundary='Native GameThread CPU and engine unit observation overhead is included for server and rendered client; optional WPR records system scheduling including the peer. Compare the same build/settings and activity. WPR must pass offline coverage/symbol/alignment validation before OS scheduling or sampled-stack attribution.'}
        $Result.Diagnostic=$DiagnosticInfo
    }
    if($DiagnosticTrace){
        $DiagnosticCase=Join-Path $DiagnosticRun $Case.Name
        $DiagnosticManifestPath=Join-Path $DiagnosticCase 'diagnostic-manifest.json'
        $null=New-Item -ItemType Directory -Path $DiagnosticCase
        $DiagnosticInfo=[ordered]@{SchemaVersion=1;Case=$Case.Name;HitProxyGeometry=$HitProxyGeometry;DiagnosticOnly=$true;FormalBaseline=$false;TraceOnly=[bool]$DiagnosticTraceOnly;TraceRoles=$TraceRoles;ServerTraceOnly=[bool]$ServerTraceOnly;
            NativeCsvPolicy=if($DiagnosticTraceOnly){'Intentionally disabled for startup-failure A/B isolation; no native CSV or stat-unit thread/GPU timings claimed.'}else{'Finite native capture; require complete finalization; exact window coverage remains an offline gate.'};
            TraceDirectory=$DiagnosticCase;StartedUtc=[datetime]::UtcNow.ToString('o');StopwatchFrequency=[Diagnostics.Stopwatch]::Frequency;
            ServerCsvFrameBudget=$DiagnosticServerCsvFrames;RenderedClientCsvFrameBudget=$DiagnosticClientCsvFrames;
            Roles=@{};StatusObservations=@();ControlEvents=@();NativeCsv=@{};AlignmentVerified=$false;
            Boundary='Extra Trace/CSV/named-event overhead. Native summary timingPolicy text predates external instrumentation and is not applicable to this diagnostic case. Exact capture-window alignment and coverage require offline validation; do not use whole-process means.'}
        $Result.Diagnostic=$DiagnosticInfo
    }
    if($DiagnosticScheduler){
        $SchedulerDirectory=Join-Path $DiagnosticRun ($Case.Name+'/Scheduler')
        $null=New-Item -ItemType Directory -Path $SchedulerDirectory
        $SchedulerTemp=Join-Path $SchedulerDirectory 'Temp'
        $null=New-Item -ItemType Directory -Path $SchedulerTemp
        $Scheduler=@{Directory=$SchedulerDirectory;Temporary=$SchedulerTemp;Instance=('FPS_Scheduler_'+[guid]::NewGuid().ToString('N'));
            ETL=(Join-Path $SchedulerDirectory 'CpuScheduling.etl');StartAttempted=$false;Started=$false;Stopped=$false;StopAttempted=$false;
            CliStillRunning=$false;CliProcess=$null;StartQpc=$null;StopQpc=$null;Bytes=0}
        $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler
    }
    try {
        if ((Get-BaselineFingerprint | ConvertTo-Json -Depth 4 -Compress) -ne $FingerprintText) { throw 'Inputs changed before case.' }
        $Conflicts=Get-BaselineCompetitors @()
        if ($Conflicts.Count) { Save-BaselineCompetitorEvidence $Conflicts $CaseRoot; throw ('Unrelated profiling/build/game processes: '+($Conflicts.Id -join ',')) }
        if (@([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveUdpListeners() | Where-Object Port -eq $Port).Count) { throw 'Requested test port is in use.' }
        foreach($Role in @('Server','Client1','Client2')){Write-BaselineControl $Controls[$Role] 'WAIT'}
        $Launches=@()
        foreach ($Role in @('Server','Client1','Client2')) {
            $User=Join-Path $CaseRoot ('Users/'+$Role)
            $RoleControl=$Controls[$Role]
            New-PrivateUserDirectory $User
            $IsEditor=$Role -eq 'Server' -or $Case.Kind -eq 'Editor'
            $Executable=[IO.Path]::GetFullPath($(if($IsEditor){$Editor}else{$ShippingClientExecutable}))
            # Native URL portal selects the map-authored PlayerStartTag. This is
            # deterministic initial spawning, not teleport or a gameplay RPC.
            $Travel=if($Role -eq 'Server'){$Map+'?game=/Script/fpstrue.fpstruePvpGameMode'}else{'127.0.0.1:'+$Port+'#GroupBaseline'+$Role}
            $Arguments=@()
            if($IsEditor){$Arguments+=('"'+$Project+'"')}
            $Arguments+=@($Travel,'-NoLiveCoding','-NoP4','-NoSplash','-NoSteam','-NoSound','-Unattended','-PvpBaseline',
                ('-PvpBaselineRole='+$Role),('-PvpBaselineEnemies='+$EnemyCount),('-PvpBaselineControl="'+$RoleControl+'"'),('-PvpBaselineOutput="'+$CaseRoot+'"'),
                ('-PvpBaselineRoot="'+$OutputRoot+'"'),('-PvpBaselineView='+$View),('-EnemyReplication='+$EnemyReplication),('-EnemyHitProxies='+$EnemyHitProxies),
                ('-UserDir="'+$User+'"'),('-abslog="'+(Join-Path $CaseRoot ($Role+'.log'))+'"'))
            if($IsEditor){$Arguments+=@('-DDC=InstalledNoZenLocalFallback','-DDC-ForceMemoryCache','-ini:Game:[StartupActions]:bAddPacks=False')}
            if($Role -eq 'Server') {
                $Arguments+=@('-server','-NullRHI',('-port='+$Port),('-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyPopulation='+$EnemyCount),
                    '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemySpawnInterval=0.05','-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyRespawnDelay=600')
                if($Case.ServerAnimationThreads -eq 'Parallel'){$Arguments+='-useperfthreads'}
                if($null -ne $ServerTickRate){$Arguments+=('-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate='+$ServerTickRate)}
                if($ContinuousFloorChecks){$Arguments+='-PvpBaselineAlwaysCheckFloor'}
            } else {
                # Shipping intentionally ignores a positional travel URL. Both
                # client builds use the same bounded loopback benchmark entry;
                # native pending connection / handshake / replication are unchanged.
                $Arguments+=('-PvpBaselineConnect='+$Travel)
                if($IsEditor){$Arguments+='-game'}
                if($Role -eq $RenderRole){$Arguments+=@('-dx12','-Windowed','-ForceRes','-ResX=1600','-ResY=900','-NoVSync','-PvpBaselineRendered')}
                else{$Arguments+='-NullRHI'}
            }
            if($Role -eq $RenderRole){
                if($ClientOcclusion -eq 'Buffer3'){$Arguments+='-ini:Engine:[ConsoleVariables]:r.NumBufferedOcclusionQueries=3'}
                elseif($ClientOcclusion -eq 'HZB'){$Arguments+='-ini:Engine:[ConsoleVariables]:r.HZBOcclusion=1'}
            }
            if($CaptureScreenshots -and $Role -eq $RenderRole){$Arguments+='-PvpBaselineScreenshot'}
            if($CpuAccounting -and ($Role -eq 'Server' -or $Role -eq $RenderRole)){
                $Arguments+=@('-PvpBaselineCpuAccounting','-ini:Engine:[ConsoleVariables]:net.ReportGameTickFlushTime=1')
                $DiagnosticInfo.Roles[$Role]=[ordered]@{UserDirectory=$User;CpuAccountingRequested=$true;TraceEnabled=[bool]($DiagnosticTrace -and $Role -in $TraceRoles)}
            }
            if($DiagnosticTrace -and $Role -in $TraceRoles){
                $Channels=if($Role -eq 'Server'){'cpu,frame,bookmark,task,log,counters'}else{'cpu,gpu,frame,bookmark,task,log,counters'}
                $TracePath=Join-Path $DiagnosticCase ($Role+'.utrace')
                $CsvFrames=if($Role -eq 'Server'){$DiagnosticServerCsvFrames}else{$DiagnosticClientCsvFrames}
                $Arguments+=@(('-trace='+$Channels),('-tracefile="'+$TracePath+'"'),'-statnamedevents')
                if(-not $DiagnosticTraceOnly){
                    $Arguments+=@(('-csvCaptureFrames='+$CsvFrames),
                        '-csvCategories=Animation,AnimationParallelEvaluation,CharacterMovement,CharacterMovementDetailed,ChaosPhysics,ChaosPhysicsSolver,Networking,NavigationSystem,fpstrueAI,fpstrueSignificance',
                        '-csvGpuStats','-csvCompression=0')
                }
                $DiagnosticInfo.Roles[$Role]=[ordered]@{Trace=$TracePath;TraceEnabled=$true;CpuAccountingRequested=$true;Channels=$Channels;NativeCsvFrames=if($DiagnosticTraceOnly){$null}else{$CsvFrames};UserDirectory=$User}
            }
            $Process=Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
            $Owned+=[pscustomobject]@{Role=$Role;Id=$Process.Id;Process=$Process;StartedUtc=$Process.StartTime.ToUniversalTime();Executable=$Executable;ControlPath=$RoleControl;Forced=$false}
            $Launches+=[pscustomobject]@{Role=$Role;Id=$Process.Id;StartedUtc=$Process.StartTime.ToUniversalTime().ToString('o');Executable=$Executable;ControlPath=$RoleControl;Arguments=$Arguments}
            if($Role -eq 'Server'){Start-Sleep -Seconds 3}
        }
        Write-BaselineJson $Launches (Join-Path $CaseRoot 'launch.json')
        if($HasDiagnostics){Write-BaselineJson $DiagnosticInfo $DiagnosticManifestPath}
        Write-Output ('Baseline '+$Case.Name+' ('+$EnemyCount+' AI, '+$Case.Kind+' rendered client, fixed Editor DS).')
        while($true) {
            if(((Get-Date)-$Started).TotalSeconds -gt $TimeoutSeconds){throw ('Timeout in '+$Phase)}
            if(($DiagnosticTrace -or $DiagnosticScheduler) -and (Get-PSDrive -Name C -PSProvider FileSystem).Free -lt 5GB){throw 'Diagnostic disk guard: C fell below 5 GiB; stop and retain invalid partial evidence.'}
            if($null -ne $Scheduler -and $Scheduler.Started -and -not $Scheduler.StopAttempted -and
                ([Diagnostics.Stopwatch]::GetTimestamp()-$Scheduler.StartQpc)/[Diagnostics.Stopwatch]::Frequency -gt 90){throw 'Owned scheduler recording exceeded its 90-second window; retain partial evidence.'}
            $Conflicts=Get-BaselineCompetitors @($Owned.Id)
            if($Conflicts.Count){Save-BaselineCompetitorEvidence $Conflicts $CaseRoot;throw ('Other processes appeared: '+($Conflicts.Id -join ','))}
            $States=@($Owned | ForEach-Object {Read-BaselineStatus (Join-Path $CaseRoot ($_.Role+'.status.json'))})
            if($DiagnosticTrace){foreach($State in $States){
                if($null -ne $State -and $ObservedRolePhases[$State.role] -ne $State.phase){
                    $ObservedRolePhases[$State.role]=$State.phase
                    $DiagnosticInfo.StatusObservations += [pscustomobject]@{Role=$State.role;Phase=$State.phase;ObservedUtc=[datetime]::UtcNow.ToString('o');
                        ParentStopwatch=[Diagnostics.Stopwatch]::GetTimestamp();Frames=$State.frames;CaptureSeconds=$State.captureSeconds}
                }
            }}
            foreach($State in $States){if($State.phase -eq 'FAILED'){throw ($State.role+': '+$State.reason)}}
            foreach($Exited in @($Owned | Where-Object {$_.Process.HasExited})){
                if($Phase -eq 'ClientsExit'){
                    if($Exited.Role -eq 'Server'){throw 'Server exited before both clients completed normal shutdown.'}
                    if($Exited.Process.ExitCode -ne 0){throw ($Exited.Role+' nonzero client shutdown exit '+$Exited.Process.ExitCode)}
                }
                elseif($Phase -eq 'ServerExit'){
                    if($Exited.Process.ExitCode -ne 0){throw ($Exited.Role+' nonzero shutdown exit '+$Exited.Process.ExitCode)}
                }
                else{throw ($Exited.Role+' exited before its final release.')}
            }
            if($Phase -eq 'Ready' -and $States.Count -eq 3 -and @($States | Where-Object phase -eq 'READY').Count -eq 3){$Phase='Warmup';$PhaseAt=Get-Date}
            elseif($Phase -eq 'Warmup' -and ((Get-Date)-$PhaseAt).TotalSeconds -ge 21){
                if($null -ne $Scheduler){Start-SchedulerRecording $Scheduler; $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler; Write-BaselineJson $DiagnosticInfo $DiagnosticManifestPath}
                foreach($Role in @('Server','Client1','Client2')){Write-BaselineControl $Controls[$Role] 'START'}
                if($DiagnosticTrace){$DiagnosticInfo.ControlEvents += [pscustomobject]@{Command='START';Utc=[datetime]::UtcNow.ToString('o');ParentStopwatch=[Diagnostics.Stopwatch]::GetTimestamp()}}
                $Phase='CaptureStart'
            }
            elseif($Phase -eq 'CaptureStart' -and @($States | Where-Object phase -eq 'CAPTURING').Count -eq 3){
                $Phase='Capture';$PhaseAt=Get-Date
                if($PresentMon -and (Test-Path -LiteralPath $PresentMonExecutable)) {
                    $RenderProcess=@($Owned | Where-Object Role -eq $RenderRole)[0]
                    $PmArguments=@('-process_id',$RenderProcess.Id,'-output_file',('"'+(Join-Path $CaseRoot 'PresentMon.csv')+'"'),
                        '-session_name',('FpstrueBaseline_'+[guid]::NewGuid().ToString('N')),'-timed','60','-terminate_after_timed','-no_top','-qpc_time_s')
                    $PmProcess=Start-Process -FilePath $PresentMonExecutable -ArgumentList $PmArguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $CaseRoot 'PresentMon.stderr.txt') -RedirectStandardOutput (Join-Path $CaseRoot 'PresentMon.stdout.txt')
                }
            }
            elseif($Phase -eq 'Capture' -and ((Get-Date)-$PhaseAt).TotalSeconds -ge 61){
                foreach($Role in @('Server','Client1','Client2')){Write-BaselineControl $Controls[$Role] 'STOP'}
                if($DiagnosticTrace){$DiagnosticInfo.ControlEvents += [pscustomobject]@{Command='STOP';Utc=[datetime]::UtcNow.ToString('o');ParentStopwatch=[Diagnostics.Stopwatch]::GetTimestamp()}}
                $Phase='Flush'
            }
            elseif($Phase -eq 'Flush' -and @($States | Where-Object phase -eq 'FLUSHED').Count -eq 3){
                # STOP is asynchronous: wait until every role has recorded its real
                # stop anchor. Offline QPC clipping excludes the extra flush tail.
                if($null -ne $Scheduler){Stop-SchedulerRecording $Scheduler; $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler; Write-BaselineJson $DiagnosticInfo $DiagnosticManifestPath}
                if($DiagnosticTrace -and -not $DiagnosticTraceOnly){$Phase='DiagnosticFlush';$PhaseAt=Get-Date}
                else{
                    foreach($Role in @('Client1','Client2')){Write-BaselineControl $Controls[$Role] 'EXIT'}
                    $Phase='ClientsExit'
                }
            }
            elseif($Phase -eq 'DiagnosticFlush'){
                if(((Get-Date)-$PhaseAt).TotalSeconds -ge $DiagnosticFlushTimeoutSeconds){throw 'Native diagnostic CSV did not finalize within the bounded flush wait.'}
                foreach($Role in @('Server',$RenderRole)){
                    if(-not $DiagnosticInfo.NativeCsv.Contains($Role)){
                        $Native=Read-NativeCsvCompletion $DiagnosticInfo.Roles[$Role].UserDirectory (Join-Path $CaseRoot ($Role+'.log'))
                        if($null -ne $Native){$DiagnosticInfo.NativeCsv[$Role]=$Native}
                    }
                }
                if($DiagnosticInfo.NativeCsv.Count -eq 2){
                    Write-BaselineJson $DiagnosticInfo (Join-Path $DiagnosticCase 'diagnostic-manifest.json')
                    foreach($Role in @('Client1','Client2')){Write-BaselineControl $Controls[$Role] 'EXIT'}
                    $Phase='ClientsExit'
                }
            }
            elseif($Phase -eq 'ClientsExit'){
                $Clients=@($Owned | Where-Object Role -ne 'Server')
                $ClientStates=@($States | Where-Object {$_.role -in @('Client1','Client2')})
                if(@($Clients | Where-Object {-not $_.Process.HasExited}).Count -eq 0 -and
                    $ClientStates.Count -eq 2 -and @($ClientStates | Where-Object phase -eq 'COMPLETE').Count -eq 2){
                    # Exit codes were checked above. Never release DS merely
                    # because client status says COMPLETE while it is still alive.
                    Write-BaselineControl $Controls['Server'] 'EXIT'
                    $Phase='ServerExit'
                }
            }
            elseif($Phase -eq 'ServerExit' -and @($Owned | Where-Object {-not $_.Process.HasExited}).Count -eq 0){break}
            Start-Sleep -Milliseconds 500
        }
        foreach($Entry in $Owned) {
            $Entry.Process.Refresh()
            if($Entry.Process.ExitCode -ne 0){throw ($Entry.Role+' nonzero exit '+$Entry.Process.ExitCode)}
            $Status=Read-BaselineStatus (Join-Path $CaseRoot ($Entry.Role+'.status.json'))
            if($null -eq $Status -or $Status.phase -ne 'COMPLETE'){throw ('Missing COMPLETE: '+$Entry.Role)}
            $Summary=Get-Content -LiteralPath (Join-Path $CaseRoot ($Entry.Role+'.summary.json')) -Raw | ConvertFrom-Json
            if(-not $Summary.valid){throw ('Invalid summary: '+$Entry.Role)}
            if($Summary.schemaVersion -ne 2 -or $Summary.role -cne $Entry.Role -or $Summary.pid -ne $Entry.Id -or $Summary.validationSamples -lt 100){throw ('Summary identity or validation coverage mismatch: '+$Entry.Role)}
            if($Summary.enemyReplication -cne $EnemyReplication -or $Status.enemyReplication -cne $EnemyReplication){throw ('Native replication A/B mode mismatch: '+$Entry.Role)}
            $Animation=$Summary.animationDiagnostics
            if($null -eq $Animation -or $null -eq $Animation.start -or $null -eq $Animation.stop -or
                $Animation.start.enemies -ne $EnemyCount -or $Animation.stop.enemies -ne $EnemyCount){throw ('Missing boundary animation diagnostics: '+$Entry.Role)}
            $ExpectedPerfThreads=$Entry.Role -eq 'Server' -and $Case.ServerAnimationThreads -eq 'Parallel'
            foreach($Snapshot in @($Animation.start,$Animation.stop)){
                if($null -eq $Snapshot.tacticalManagers){throw ('Missing tactical boundary snapshot: '+$Entry.Role)}
                $Managers=@($Snapshot.tacticalManagers)
                if($Entry.Role -eq 'Server' -and $Managers.Count -eq 0){throw 'Missing server tactical managers.'}
                $Participants=0
                foreach($Manager in $Managers){
                    # An inert map-placed Actor is allowed on clients; an initialized
                    # tactical ledger is not. Existence alone does not mean simulation.
                    if($Entry.Role -ne 'Server' -and ($Manager.innerCapacity -ne 0 -or $Manager.outerCapacity -ne 0 -or
                        $Manager.queuedEnemies -ne 0 -or $Manager.activeAttackers -ne 0)){
                        throw ('Client initialized an authority tactical ledger: '+$Entry.Role)
                    }
                    if($Manager.occupiedInnerSlots -lt 0 -or $Manager.occupiedInnerSlots -gt $Manager.innerCapacity -or
                        $Manager.occupiedOuterSlots -lt 0 -or $Manager.occupiedOuterSlots -gt $Manager.outerCapacity -or
                        $Manager.activeAttackers -lt 0 -or $Manager.activeAttackers -gt $Manager.occupiedInnerSlots -or
                        $Manager.queuedEnemies -lt 0){throw ('Tactical capacity or admission invariant failed: '+$Entry.Role)}
                    # Outer occupants are also queued for Inner; do not count them twice.
                    $Participants+=$Manager.occupiedInnerSlots+$Manager.queuedEnemies
                }
                if($Participants -gt $EnemyCount){throw 'Tactical participants exceed the fixed live population.'}
                if([bool]$Snapshot.forceUsePerfThreads -ne $ExpectedPerfThreads){throw ('Animation threading launch mismatch: '+$Entry.Role)}
                if($Entry.Role -eq 'Server' -and ($Snapshot.netMode -ne 1 -or -not $Snapshot.dedicatedServer -or
                    [bool]$Snapshot.shouldUseThreadingForPerformance -ne $ExpectedPerfThreads)){throw 'Dedicated-server threading gate mismatch.'}
                if($Entry.Role -eq 'Server'){
                    $ExpectedContinuousFloorEnemies=if($ContinuousFloorChecks){$EnemyCount}else{0}
                    if($null -eq $Snapshot.movement -or $null -eq $Snapshot.movement.alwaysCheckFloorEnemies -or
                        [int]$Snapshot.movement.alwaysCheckFloorEnemies -ne $ExpectedContinuousFloorEnemies){
                        throw 'Actual server continuous-floor-check population does not match this control.'
                    }
                }
                $ExpectedSimple=if($EnemyHitProxies -eq 'Simple'){$EnemyCount}else{0}
                if($Snapshot.simpleHitProxyEnemies -ne $ExpectedSimple){throw ('Actual enemy hit-proxy mode mismatch: '+$Entry.Role)}
                if($EnemyHitProxies -eq 'Simple'){
                    Assert-BaselineHitProxyGeometry $Snapshot $Entry.Role
                }
            }
            $NetworkCounters=Assert-BaselineNetworkCounters $Summary $Entry.Role
            if($CpuAccounting -and $Entry.Role -in @('Server',$RenderRole)){
                $Cpu=$Summary.cpuAccounting
                if($null -eq $Cpu -or -not $Cpu.enabled -or -not $Cpu.platformSupported -or -not $Cpu.valid -or
                    $Cpu.validFrameCpuIntervals -ne $Summary.frames -or $Cpu.validWorldCpuIntervals -ne $Summary.frames -or
                    $Cpu.osQueryFailures -ne 0 -or $Cpu.negativeFrameCpuDeltas -ne 0 -or $Cpu.negativeWorldCpuDeltas -ne 0 -or
                    $null -eq $Cpu.meanFrameCpuMs -or $null -eq $Cpu.meanWorldCpuMs -or
                    [double]::IsNaN([double]$Cpu.meanFrameCpuMs) -or [double]::IsInfinity([double]$Cpu.meanFrameCpuMs) -or
                    [double]::IsNaN([double]$Cpu.meanWorldCpuMs) -or [double]::IsInfinity([double]$Cpu.meanWorldCpuMs)){
                    throw ('Missing or incomplete native GameThread CPU accounting: '+$Entry.Role)
                }
                $DiagnosticInfo.Roles[$Entry.Role].CpuAccounting=$Cpu
                $DiagnosticInfo.Roles[$Entry.Role].ClockAnchors=$Summary.traceAlignment
            }
            if($Summary.minEnemies -ne $EnemyCount -or $Summary.maxEnemies -ne $EnemyCount -or $Summary.minPlayers -ne 2 -or $Summary.maxPlayers -ne 2 -or
                $Summary.captureSeconds -lt 59.8 -or $Summary.captureSeconds -gt 75 -or $Summary.actualMapPackage -cne $Map){throw ('Population, duration or map mismatch: '+$Entry.Role)}
            if($Entry.Role -eq 'Server') {
                $Activity=$Summary.aiActivity
                if($null -eq $Activity -or $Activity.minControllers -ne $EnemyCount -or $Activity.maxControllers -ne $EnemyCount -or
                    $Activity.minBehaviorReady -ne $EnemyCount -or $Activity.maxBehaviorReady -ne $EnemyCount -or
                    $Activity.validationSamples -ne $Summary.validationSamples -or
                    ($Activity.maxMoving -le 0 -and $Activity.maxAttacking -le 0)) {
                    throw ('Server AI activity invalid: require '+$EnemyCount+' running behavior controllers throughout and observed movement or attacks.')
                }
            }
            $Pacing=$Summary.pacingDiagnostics
            $ExpectedPacingFrames=$CpuAccounting -and ($Entry.Role -eq 'Server' -or $Entry.Role -eq $RenderRole)
            if($null -eq $Pacing -or $Pacing.perFrameDiagnostic -isnot [bool] -or [bool]$Pacing.perFrameDiagnostic -ne $ExpectedPacingFrames){throw ('Missing or unexpected pacing diagnostics: '+$Entry.Role)}
            foreach($Boundary in @('start','stop')){
                $Snapshot=$Pacing.$Boundary
                if($null -eq $Snapshot -or $Snapshot.netDriverValid -isnot [bool] -or -not $Snapshot.netDriverValid -or
                    $Snapshot.engineQueryValid -isnot [bool]){throw ('Missing live NetDriver pacing readback: '+$Entry.Role+'/'+$Boundary)}
                foreach($Field in @('queryInputDeltaSeconds','appDeltaSeconds','appIdleSeconds','appIdleOvershootSeconds')){
                    if($null -eq $Snapshot.$Field -or [double]::IsNaN([double]$Snapshot.$Field) -or [double]::IsInfinity([double]$Snapshot.$Field) -or [double]$Snapshot.$Field -lt 0){throw ('Invalid raw pacing field: '+$Entry.Role+'/'+$Boundary+'/'+$Field)}
                }
                if($Snapshot.engineQueryValid){
                    if($null -eq $Snapshot.queriedEngineMaxTickRateHz -or [double]::IsNaN([double]$Snapshot.queriedEngineMaxTickRateHz) -or [double]::IsInfinity([double]$Snapshot.queriedEngineMaxTickRateHz)){throw 'Invalid safe engine pacing query.'}
                } elseif($null -ne $Snapshot.queriedEngineMaxTickRateHz){throw 'Unsafe engine pacing query must be null.'}
                if($Entry.Role -eq 'Server' -and $null -ne $ServerTickRate){
                    if([int]$Snapshot.netServerMaxTickRate -ne $ServerTickRate -or -not $Snapshot.engineQueryValid -or
                        [Math]::Abs([double]$Snapshot.queriedEngineMaxTickRateHz-$ServerTickRate) -gt 0.0001 -or
                        $Snapshot.useFixedFrameRate -or $Snapshot.appUseFixedTimeStep -or $Snapshot.appBenchmarking){
                        throw ('Explicit server tick target not confirmed, or another pacing override is active: '+$Boundary)
                    }
                }
            }
            $ExpectedShipping=$Case.Kind -eq 'Shipping' -and $Entry.Role -ne 'Server'
            if([bool]$Summary.isShipping -ne $ExpectedShipping -or [bool]$Summary.rendered -ne ($Entry.Role -eq $RenderRole)){throw ('Build/render identity mismatch: '+$Entry.Role)}
            if($Entry.Role -eq $RenderRole){
                if($Summary.width -ne 1600 -or $Summary.height -ne 900 -or $Summary.nullRhi){throw 'Actual rendered viewport mismatch.'}
                foreach($NativeName in @('r.NumBufferedOcclusionQueries','r.HZBOcclusion','r.OneFrameThreadLag','r.RHICmdBypass')){
                    $NativeValue=$Summary.quality.$NativeName
                    if($null -eq $NativeValue -or [double]::IsNaN([double]$NativeValue) -or [double]::IsInfinity([double]$NativeValue)){throw ('Missing actual renderer CVar readback: '+$NativeName)}
                }
                if(($ClientOcclusion -eq 'Buffer3' -and [double]$Summary.quality.'r.NumBufferedOcclusionQueries' -ne 3) -or
                    ($ClientOcclusion -eq 'HZB' -and [double]$Summary.quality.'r.HZBOcclusion' -ne 1)){
                    throw ('Requested client occlusion override did not take effect: '+$ClientOcclusion)
                }
                $ActualView=@($Summary.viewLocation)+@($Summary.viewRotation)
                if($null -eq $ReferenceView){$ReferenceView=$ActualView}
                for($ViewIndex=0;$ViewIndex -lt 6;++$ViewIndex){if([Math]::Abs([double]$ActualView[$ViewIndex]-[double]$ReferenceView[$ViewIndex]) -gt 0.1){throw 'Camera changed between repeated/build cases.'}}
                $ActualQuality=$Summary.quality | ConvertTo-Json -Compress
                if($null -eq $ReferenceQuality){$ReferenceQuality=$ActualQuality}
                if($ActualQuality -cne $ReferenceQuality){throw 'Actual scalability/render feature settings changed between cases.'}
                if($null -eq $ReferenceFov){$ReferenceFov=[double]$Summary.fov}
                if($Summary.fov -le 0 -or [Math]::Abs([double]$Summary.fov-$ReferenceFov) -gt 0.1){throw 'Actual FOV changed between cases.'}
            }
            $Rows=@(Import-Csv -LiteralPath (Join-Path $CaseRoot ($Entry.Role+'.frames.csv')))
            $ExpectedColumns='Frame,ElapsedSeconds,FrameEndIntervalMs,WorldTickWallMs,WorldTickValid,CpuAccountingEnabled,GTEndCpuValid,GTEndThreadId,GTEndCpuQueryError,GTPreviousEndUser100ns,GTPreviousEndKernel100ns,GTEndUser100ns,GTEndKernel100ns,GTFrameCpuValid,GTFrameUser100nsDelta,GTFrameKernel100nsDelta,GTFrameCpuMs,GTWorldCpuValid,GTWorldBeginUser100ns,GTWorldBeginKernel100ns,GTWorldEndUser100ns,GTWorldEndKernel100ns,GTWorldUser100nsDelta,GTWorldKernel100nsDelta,GTWorldCpuMs'
            if($Rows.Count -eq 0 -or ($Rows[0].PSObject.Properties.Name -join ',') -cne $ExpectedColumns){throw 'Expected exact schema-2 frame columns.'}
            if($CpuAccounting -and ($Entry.Role -eq 'Server' -or $Entry.Role -eq $RenderRole)){
                $PacingRows=@(Import-Csv -LiteralPath (Join-Path $CaseRoot ($Entry.Role+'.pacing.csv')))
                $PacingColumns='Frame,AppDeltaSeconds,AppIdleSeconds,AppIdleOvershootSeconds,NetDriverValid,NetServerMaxTickRate,MaxNetTickRate,EngineQueryValid,QueryInputDeltaSeconds,QueriedEngineMaxTickRateHz'
                if($PacingRows.Count -ne $Rows.Count -or $PacingRows.Count -eq 0 -or ($PacingRows[0].PSObject.Properties.Name -join ',') -cne $PacingColumns){throw 'Missing or mismatched diagnostic pacing schema.'}
                for($PacingIndex=0;$PacingIndex -lt $PacingRows.Count;++$PacingIndex){
                    $PacingRow=$PacingRows[$PacingIndex]
                    if([long]$PacingRow.Frame -ne [long]$Rows[$PacingIndex].Frame -or $PacingRow.NetDriverValid -cne '1' -or $PacingRow.EngineQueryValid -cnotin @('0','1')){throw 'Diagnostic pacing/frame identity or validity mismatch.'}
                    foreach($Field in @('AppDeltaSeconds','AppIdleSeconds','AppIdleOvershootSeconds','QueryInputDeltaSeconds')){
                        if([string]::IsNullOrWhiteSpace($PacingRow.$Field) -or [double]::IsNaN([double]$PacingRow.$Field) -or [double]::IsInfinity([double]$PacingRow.$Field) -or [double]$PacingRow.$Field -lt 0){throw ('Invalid diagnostic pacing sample: '+$Field)}
                    }
                    if($PacingRow.EngineQueryValid -eq '0'){
                        if(-not [string]::IsNullOrEmpty($PacingRow.QueriedEngineMaxTickRateHz)){throw 'Unsafe per-frame engine pacing query must be blank.'}
                    } elseif([string]::IsNullOrWhiteSpace($PacingRow.QueriedEngineMaxTickRateHz) -or [double]::IsNaN([double]$PacingRow.QueriedEngineMaxTickRateHz) -or [double]::IsInfinity([double]$PacingRow.QueriedEngineMaxTickRateHz)){throw 'Invalid per-frame engine pacing query.'}
                    if($Entry.Role -eq 'Server' -and $null -ne $ServerTickRate -and
                        ([int]$PacingRow.NetServerMaxTickRate -ne $ServerTickRate -or $PacingRow.EngineQueryValid -ne '1' -or [Math]::Abs([double]$PacingRow.QueriedEngineMaxTickRateHz-$ServerTickRate) -gt 0.0001)){throw 'Explicit server tick target changed inside diagnostic capture.'}
                }
                $DiagnosticInfo.Roles[$Entry.Role].Pacing=[ordered]@{File=(Join-Path $CaseRoot ($Entry.Role+'.pacing.csv'));Samples=$PacingRows.Count;
                    Boundary='Exact global-frame pairing; FApp idle/overshoot are pacing observations, not OS scheduler states. Query is a guarded re-evaluation, not the internal UpdateTimeAndHandleMaxTickRate local.'}
                $DiagnosticInfo.Roles[$Entry.Role].NativeUnits=Assert-BaselineNativeUnits $Summary $Entry.Role $CaseRoot $Rows
            }
            foreach($Row in $Rows){
                if($Row.WorldTickValid -cnotin @('0','1')){throw 'Invalid selected-World frame validity.'}
                if($Row.WorldTickValid -eq '0'){
                    if(-not [string]::IsNullOrEmpty($Row.WorldTickWallMs)){throw 'An absent World frame contains a stale wall time.'}
                } elseif([string]::IsNullOrWhiteSpace($Row.WorldTickWallMs) -or [double]::IsNaN([double]$Row.WorldTickWallMs) -or
                    [double]::IsInfinity([double]$Row.WorldTickWallMs) -or [double]$Row.WorldTickWallMs -lt 0){throw 'Invalid selected-World tick wall time.'}
            }
            if($Entry.Role -eq 'Server' -and @($Rows | Where-Object WorldTickValid -eq '1').Count -eq 0){throw 'Server has no valid World samples.'}
            if($DiagnosticTrace){
                $DiagnosticInfo.Roles[$Entry.Role+'.BaselineWindow']=[ordered]@{FirstGlobalFrame=$Rows[0].Frame;LastGlobalFrame=$Rows[-1].Frame;
                    FirstElapsedSeconds=$Rows[0].ElapsedSeconds;LastElapsedSeconds=$Rows[-1].ElapsedSeconds;Samples=$Rows.Count;
                    Boundary='Global frame numbers are not assumed to equal Insights frame indexes; offline alignment is mandatory.'}
            }
            if($Rows.Count -lt 100 -or $Rows.Count -ne $Summary.frames){throw ('Insufficient or inconsistent frame samples: '+$Entry.Role)}
            $Values=[double[]]@($Rows | ForEach-Object {[double]$_.FrameEndIntervalMs})
            if(@($Values | Where-Object {$_ -le 0 -or [double]::IsNaN($_) -or [double]::IsInfinity($_)}).Count){throw 'Nonfinite or nonpositive frame sample.'}
            for($Index=1;$Index -lt $Rows.Count;++$Index){
                if([long]$Rows[$Index].Frame -le [long]$Rows[$Index-1].Frame -or [double]$Rows[$Index].ElapsedSeconds -le [double]$Rows[$Index-1].ElapsedSeconds){throw 'Frame numbers/time are not strictly monotonic.'}
            }
            if([double]$Rows[-1].ElapsedSeconds -lt 59.5 -or ([Math]::Abs(($Values | Measure-Object -Sum).Sum - 1000.0*$Summary.captureSeconds)) -gt 1500){throw 'Frame samples do not cover the full capture window.'}
            $Mean=($Values | Measure-Object -Average).Average
            $WorldTicks=[double[]]@($Rows | Where-Object WorldTickValid -eq '1' | ForEach-Object {[double]$_.WorldTickWallMs})
            if(@($WorldTicks | Where-Object {$_ -lt 0 -or [double]::IsNaN($_) -or [double]::IsInfinity($_)}).Count){throw 'Invalid selected-World tick sample.'}
            $WorldTickResult=if($WorldTicks.Count){[ordered]@{Samples=$WorldTicks.Count;MeanMs=($WorldTicks | Measure-Object -Average).Average;
                P95Ms=(Get-Percentile $WorldTicks 0.95);P99Ms=(Get-Percentile $WorldTicks 0.99);
                Boundary='World tick wall scope, including dependent waits; not stat-unit GameThread time.'}}else{$null}
            $Result.Roles[$Entry.Role]=[ordered]@{Samples=$Values.Count;FrameEndIntervalMeanMs=$Mean;P95Ms=(Get-Percentile $Values 0.95);
                P99Ms=(Get-Percentile $Values 0.99);MaxMs=($Values | Measure-Object -Maximum).Maximum;EquivalentFPS=1000.0/$Mean;
                FramesOver16_667Percent=100.0*@($Values | Where-Object {$_ -gt (1000.0/60.0)}).Count/$Values.Count;
                WorldTickWall=$WorldTickResult;AIActivity=$Summary.aiActivity;NetworkConnections=$NetworkCounters;RawSummary=$Summary}
        }
        if((Get-BaselineFingerprint | ConvertTo-Json -Depth 4 -Compress) -ne $FingerprintText){throw 'Inputs changed during case.'}
        if($DiagnosticTrace){
            foreach($Role in $TraceRoles){
                $TracePath=$DiagnosticInfo.Roles[$Role].Trace
                if(-not (Test-Path -LiteralPath $TracePath -PathType Leaf) -or (Get-Item -LiteralPath $TracePath).Length -le 0){throw ('Missing diagnostic trace: '+$Role)}
                $DiagnosticInfo.Roles[$Role].TraceBytes=(Get-Item -LiteralPath $TracePath).Length
                $DiagnosticInfo.Roles[$Role].TraceSHA256=(Get-FileHash -LiteralPath $TracePath -Algorithm SHA256).Hash
                if(-not $DiagnosticTraceOnly){
                    $DiagnosticInfo.NativeCsv[$Role] | Add-Member -NotePropertyName SHA256 -NotePropertyValue (Get-FileHash -LiteralPath $DiagnosticInfo.NativeCsv[$Role].Path -Algorithm SHA256).Hash
                }
            }
        }
        if($null -ne $PmProcess){
            if(-not $PmProcess.WaitForExit(10000)){Stop-Process -Id $PmProcess.Id -Force}
            $PmProcess.Refresh()
            $PresentFile=Join-Path $CaseRoot 'PresentMon.csv'
            $PresentRows=if(Test-Path -LiteralPath $PresentFile){@(Import-Csv -LiteralPath $PresentFile)}else{@()}
            $Result.PresentMon=[ordered]@{ExitCode=$PmProcess.ExitCode;Rows=$PresentRows.Count;Valid=$false;
                Boundary='External present telemetry; not pooled with engine-frame intervals. Failure does not invent present timing.'}
            $SwapChains=@($PresentRows | Group-Object SwapChainAddress)
            $Result.PresentMon.SwapChains=$SwapChains.Count
            if($PmProcess.ExitCode -eq 0 -and $PresentRows.Count -gt 10 -and $SwapChains.Count -eq 1 -and $null -ne $PresentRows[0].MsBetweenPresents){
                $PresentValues=[double[]]@($PresentRows | Select-Object -Skip 1 | ForEach-Object {[double]$_.MsBetweenPresents})
                if(@($PresentValues | Where-Object {$_ -le 0 -or [double]::IsNaN($_) -or [double]::IsInfinity($_)}).Count -eq 0){
                    $Result.PresentMon.Valid=$true
                    $Result.PresentMon.MeanMs=($PresentValues | Measure-Object -Average).Average
                    $Result.PresentMon.P95Ms=Get-Percentile $PresentValues 0.95
                    $Result.PresentMon.P99Ms=Get-Percentile $PresentValues 0.99
                }
            }
        }
        $Result.Valid=$true
    } catch {$Failure=$_.Exception.Message;$Result.Error=$Failure;Write-Warning ($Case.Name+': '+$Failure)}
    finally {
        # A failed start can leave a partially active *owned named instance*.
        # Query only that fresh name; never issue a global cancel or race a live CLI.
        if($null -ne $Scheduler -and $Scheduler.StartAttempted -and -not $Scheduler.Started -and -not $Scheduler.CliStillRunning){
            try {
                $PartialStatus=Invoke-SchedulerWpr $Scheduler @('-status','profiles','collectors','-details','-instancename',$Scheduler.Instance) 'partial-start-status'
                if($PartialStatus -notmatch 'WPR is not recording' -and $PartialStatus -match '(?i)CPU\.Verbose\.File'){$Scheduler.Started=$true}
            } catch {Write-Warning ('Unable to verify owned partial recorder '+$Scheduler.Instance+': '+$_.Exception.Message)}
        }
        if($null -ne $Scheduler -and $Scheduler.Started -and -not $Scheduler.StopAttempted -and -not $Scheduler.CliStillRunning){
            try {Stop-SchedulerRecording $Scheduler}
            catch {$Result.Valid=$false;$Result.Error=('Owned scheduler cleanup: '+$_.Exception.Message);Write-Warning $Result.Error}
        }
        if($null -ne $Scheduler){$DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler}
        foreach($Entry in $Owned){
            $Entry.Process.Refresh()
            if(-not $Entry.Process.HasExited){
                $Live=Get-Process -Id $Entry.Id -ErrorAction SilentlyContinue
                if($Live -and $Live.StartTime.ToUniversalTime() -eq $Entry.StartedUtc -and [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($Live.Path),$Entry.Executable)){
                    $Entry.Forced=$true;Stop-Process -Id $Entry.Id -Force;$null=$Entry.Process.WaitForExit(10000)
                }
            }
            $Entry.Process.Refresh()
            $Result.Processes+=[pscustomobject]@{Role=$Entry.Role;Id=$Entry.Id;StartedUtc=$Entry.StartedUtc.ToString('o');Executable=$Entry.Executable;ControlPath=$Entry.ControlPath;Forced=$Entry.Forced;ExitCode=$Entry.Process.ExitCode}
            $Entry.Process.Dispose()
        }
        if($null -ne $PmProcess){if(-not $PmProcess.HasExited){Stop-Process -Id $PmProcess.Id -Force};$PmProcess.Dispose()}
        $Result.DurationSeconds=((Get-Date)-$Started).TotalSeconds
        if($HasDiagnostics){
            $DiagnosticInfo.FinishedUtc=[datetime]::UtcNow.ToString('o');$DiagnosticInfo.Failure=$Result.Error
            Write-BaselineJson $DiagnosticInfo $DiagnosticManifestPath
        }
        Write-BaselineJson $Result (Join-Path $CaseRoot 'result.json')
        $Results+=[pscustomobject]$Result
        Write-BaselineJson $Results (Join-Path $Run 'results.json')
    }
    if(-not $Result.Valid){throw ('Baseline stopped; invalid evidence retained: '+$CaseRoot)}
}
$Aggregates=@()
foreach($Kind in $BuildKinds){ foreach($Threads in $ServerAnimationThreads){
    $Runs=@($Results | Where-Object {$_.Kind -eq $Kind -and $_.ServerAnimationThreads -eq $Threads})
    $Frames=@($Runs | ForEach-Object {$_.Roles[$RenderRole].FrameEndIntervalMeanMs})
    $Aggregates+=[pscustomobject]@{Build=$Kind;ServerAnimationThreads=$Threads;ServerBuild='Editor dedicated server';Repeats=$Runs.Count;RenderedRole=$RenderRole;EnemyReplication=$EnemyReplication;EnemyHitProxies=$EnemyHitProxies;HitProxyGeometry=$HitProxyGeometry;ServerPacing=$PacingPolicy;ClientOcclusion=$ClientOcclusion;ContinuousFloorChecks=[bool]$ContinuousFloorChecks;CpuDiagnostic=[bool]$CpuDiagnostic;DiagnosticOnly=$HasDiagnostics;FormalBaseline=(-not $HasDiagnostics);
        MeanFrameEndIntervalMs=($Frames | Measure-Object -Average).Average;
        MeanRunP95Ms=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].P95Ms}) | Measure-Object -Average).Average;
        MeanRunP99Ms=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].P99Ms}) | Measure-Object -Average).Average;
        MeanRunOver16_667Percent=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].FramesOver16_667Percent}) | Measure-Object -Average).Average;
        MeanServerWorldTickWallMs=(@($Runs | ForEach-Object {$_.Roles.Server.WorldTickWall.MeanMs}) | Measure-Object -Average).Average;
        MeanServerGTFrameCpuMs=if($CpuAccounting){(@($Runs | ForEach-Object {$_.Roles.Server.RawSummary.cpuAccounting.meanFrameCpuMs}) | Measure-Object -Average).Average}else{$null};
        MeanServerWorldCpuMs=if($CpuAccounting){(@($Runs | ForEach-Object {$_.Roles.Server.RawSummary.cpuAccounting.meanWorldCpuMs}) | Measure-Object -Average).Average}else{$null};
        MeanServerOutBytesPerSecond=(@($Runs | ForEach-Object {$_.Roles.Server.NetworkConnections.totals.outBytes.perSecond}) | Measure-Object -Average).Average;
        MeanServerOutPacketsPerSecond=(@($Runs | ForEach-Object {$_.Roles.Server.NetworkConnections.totals.outPackets.perSecond}) | Measure-Object -Average).Average;
        MeanRenderedClientInBytesPerSecond=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].NetworkConnections.totals.inBytes.perSecond}) | Measure-Object -Average).Average;
        NetworkBoundary='Mean of independent per-run engine connection-counter rates. Server totals sum two peers. Not NIC, per-Actor replication or exact cross-role byte matching.'}
} }
Write-BaselineJson $Aggregates (Join-Path $Run 'summary.json')
Write-Output ('Completed separate build baselines: '+$Run)
$Aggregates | Format-Table -AutoSize
