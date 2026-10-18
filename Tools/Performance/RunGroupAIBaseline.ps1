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
    [string]$Map = '/Game/PerformanceCandidates/GroupAITerrain/GroupAITerrain160',
    [ValidateSet('Editor','Shipping')][string[]]$BuildKinds = @('Editor','Shipping'),
    [string]$ShippingClientExecutable = '',
    [ValidateSet('Client1','Client2')][string]$RenderRole = 'Client1',
    [ValidateSet('Player','Overview')][string]$View = 'Player',
    [ValidateSet('SharedCandidates','Reference')][string]$FootIKQueries = 'SharedCandidates',
    [ValidateRange(1,5)][int]$Repeat = 3,
    [ValidateRange(1024,65000)][int]$Port = 17871,
    [ValidateRange(180,900)][int]$TimeoutSeconds = 600,
    [string]$RunName = ('GroupAITerrainBaseline_' + (Get-Date -Format 'yyyyMMdd_HHmmss')),
    [switch]$PresentMon,
    [switch]$CaptureScreenshots,
    [switch]$DiagnosticTrace,
    [switch]$DiagnosticTraceOnly,
    [switch]$DiagnosticScheduler,
    [string]$DiagnosticRoot = 'C:/UEBuild/FPSDiagnostics',
    [ValidateRange(100,50000)][int]$DiagnosticServerCsvFrames = 2500,
    [ValidateRange(100,50000)][int]$DiagnosticClientCsvFrames = 5000,
    [ValidateRange(30,500)][int]$DiagnosticFlushTimeoutSeconds = 300,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$FootIKQueries=if($FootIKQueries -ieq 'Reference'){'Reference'}else{'SharedCandidates'}
if ($RunName -notmatch '^[A-Za-z0-9_-]+$' -or $Map -notmatch '^/Game/[A-Za-z0-9_/]+$') { throw 'Invalid run or map package name.' }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
if ($ShippingClientExecutable) { $ShippingClientExecutable = [IO.Path]::GetFullPath($ShippingClientExecutable) }
$Project = Join-Path $ProjectRoot 'fpstrue.uproject'
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Module = Join-Path $ProjectRoot 'Binaries/Win64/UnrealEditor-fpstrue.dll'
$MapFile = Join-Path $ProjectRoot ('Content/' + $Map.Substring(6) + '.umap')
$OutputRoot = Join-Path $ProjectRoot 'Saved/Profiling'
$Run = Join-Path $OutputRoot $RunName
$DiagnosticRoot=[IO.Path]::GetFullPath($DiagnosticRoot)
$DiagnosticRun=Join-Path $DiagnosticRoot $RunName
if($DiagnosticTraceOnly -and -not $DiagnosticTrace){throw 'DiagnosticTraceOnly requires DiagnosticTrace.'}
if($DiagnosticScheduler -and (-not $DiagnosticTrace -or -not $DiagnosticTraceOnly -or $PresentMon)){
    throw 'DiagnosticScheduler requires a single Editor DiagnosticTraceOnly run without PresentMon.'
}
if($DiagnosticTrace -and ($Repeat -ne 1 -or $BuildKinds.Count -ne 1 -or $BuildKinds[0] -ne 'Editor')){
    throw 'DiagnosticTrace is an independent Editor-only single repeat, never a Shipping or formal repeated baseline.'
}
if($DiagnosticTrace -and ([IO.Path]::GetPathRoot($DiagnosticRoot) -ine 'C:\' -or $DiagnosticRoot.TrimEnd('\').Length -le 3)){
    throw 'DiagnosticRoot must be an explicit non-root C-drive directory for bounded trace storage.'
}
$PresentMonExecutable = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/PresentMon/Win64/PresentMon-1.8.0-x64.exe'
if ('Shipping' -in $BuildKinds -and -not $ShippingClientExecutable) { throw 'Provide the actual packaged Shipping executable, not an Editor or bootstrap.' }
$Cases = @()
for ($Round=1; $Round -le $Repeat; ++$Round) {
    $Order = @($BuildKinds)
    if ($Round % 2 -eq 0) { [array]::Reverse($Order) }
    foreach ($Kind in $Order) { $Cases += [pscustomobject]@{Kind=$Kind;Round=$Round;Name=('{0}_R{1}' -f $Kind,$Round)} }
}
if ($DryRun) {
    [pscustomobject]@{Run=$Run;Map=$Map;Cases=$Cases;Server='Frozen Editor -server';Clients=2;RenderedClients=1;
        Resolution='1600x900';Quality='High / scalability level 2; engine feature defaults retained';WarmupSeconds=20;CaptureSeconds=60;
        Population=160;Seed=1337;View=$View;FootIKQueries=$FootIKQueries;PresentMon=[bool]$PresentMon;
        Diagnostic=[ordered]@{Enabled=[bool]$DiagnosticTrace;TraceOnly=[bool]$DiagnosticTraceOnly;Output=$DiagnosticRun;ServerCsvFrames=$DiagnosticServerCsvFrames;
            CpuAccounting=[bool]$DiagnosticTrace;Scheduler=[ordered]@{Requested=[bool]$DiagnosticScheduler;Recorder='One named WPR CPU.Verbose.File instance; UE contextswitch/stacksampling remain disabled';RequiresAdministrator=$true;
                Analysis='ETL existence is not proof of complete Running/Ready/Wait data; verify coverage, PID/TID mappings, lost events and QPC alignment offline.'};
            RenderedClientCsvFrames=$DiagnosticClientCsvFrames;FlushTimeoutSeconds=$DiagnosticFlushTimeoutSeconds;
            Budget='Trace/named events (and optional native CSV) add CPU/memory/disk overhead. Reserve at least 5 GiB on C; traces may consume several GiB. Never merge diagnostic timings into the formal baseline.';
            Window=if($DiagnosticTraceOnly){'Native CSV intentionally disabled. Trace window alignment requires offline validation; no native thread/GPU timing claimed.'}else{'Native CSV must finalize; exact baseline/native/Trace frame alignment is an offline validation, not an assumed FrameIndex.'}};
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
if($DiagnosticTrace){
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
    $null=Invoke-SchedulerWpr $Scheduler @('-stop',$Scheduler.ETL,'FPS 160 AI execution and scheduling diagnostic','-skipPdbGen','-instancename',$Scheduler.Instance) 'stop'
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
    $Paths += @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'Content/FirstPerson/Animations/FootIK') -File -Filter '*.uasset' | ForEach-Object FullName)
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
$Fingerprint=Get-BaselineFingerprint
$FingerprintText=$Fingerprint | ConvertTo-Json -Depth 4 -Compress
$Experiment=[ordered]@{Created=(Get-Date).ToString('o');Map=$Map;Cases=$Cases;Fingerprint=$Fingerprint;WarmupSeconds=20;CaptureSeconds=60;
    Resolution='1600x900';Quality=2;Population=160;Clients=2;RenderedRole=$RenderRole;Seed=1337;View=$View;FootIKQueries=$FootIKQueries;
    Backend='Same Editor dedicated server for Editor and Shipping client cases';
    ExitPolicy='Independent per-role controls; clients COMPLETE + natural exit 0, then server EXIT';
    Diagnostic=[bool]$DiagnosticTrace;
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
    $Result=[ordered]@{Name=$Case.Name;Kind=$Case.Kind;Round=$Case.Round;FootIKQueries=$FootIKQueries;Valid=$false;Error=$null;Processes=@();Roles=@{};PresentMon=$null}
    $DiagnosticInfo=$null;$DiagnosticCase=$null;$ObservedRolePhases=@{};$Scheduler=$null
    if($DiagnosticTrace){
        $DiagnosticCase=Join-Path $DiagnosticRun $Case.Name
        $null=New-Item -ItemType Directory -Path $DiagnosticCase
        $DiagnosticInfo=[ordered]@{SchemaVersion=1;Case=$Case.Name;DiagnosticOnly=$true;FormalBaseline=$false;TraceOnly=[bool]$DiagnosticTraceOnly;
            NativeCsvPolicy=if($DiagnosticTraceOnly){'Intentionally disabled for startup-failure A/B isolation; no native CSV or stat-unit thread/GPU timings claimed.'}else{'Finite native capture; require complete finalization; exact window coverage remains an offline gate.'};
            TraceDirectory=$DiagnosticCase;StartedUtc=[datetime]::UtcNow.ToString('o');StopwatchFrequency=[Diagnostics.Stopwatch]::Frequency;
            ServerCsvFrameBudget=$DiagnosticServerCsvFrames;RenderedClientCsvFrameBudget=$DiagnosticClientCsvFrames;
            Roles=@{};StatusObservations=@();ControlEvents=@();NativeCsv=@{};AlignmentVerified=$false;
            Boundary='Extra Trace/CSV/named-event overhead. Native summary timingPolicy text predates external instrumentation and is not applicable to this diagnostic case. Exact capture-window alignment and coverage require offline validation; do not use whole-process means.'}
        $Result.Diagnostic=$DiagnosticInfo
        if($DiagnosticScheduler){
            $SchedulerDirectory=Join-Path $DiagnosticCase 'Scheduler'
            $null=New-Item -ItemType Directory -Path $SchedulerDirectory
            $SchedulerTemp=Join-Path $SchedulerDirectory 'Temp'
            $null=New-Item -ItemType Directory -Path $SchedulerTemp
            $Scheduler=@{Directory=$SchedulerDirectory;Temporary=$SchedulerTemp;Instance=('FPS_Scheduler_'+[guid]::NewGuid().ToString('N'));
                ETL=(Join-Path $SchedulerDirectory 'CpuScheduling.etl');StartAttempted=$false;Started=$false;Stopped=$false;StopAttempted=$false;
                CliStillRunning=$false;CliProcess=$null;StartQpc=$null;StopQpc=$null;Bytes=0}
            $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler
        }
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
                ('-PvpBaselineRole='+$Role),('-PvpBaselineControl="'+$RoleControl+'"'),('-PvpBaselineOutput="'+$CaseRoot+'"'),
                ('-PvpProfileRoot="'+$OutputRoot+'"'),('-PvpBaselineView='+$View),('-EnemyFootIKQueries='+$FootIKQueries),
                ('-UserDir="'+$User+'"'),('-abslog="'+(Join-Path $CaseRoot ($Role+'.log'))+'"'))
            if($IsEditor){$Arguments+=@('-DDC=InstalledNoZenLocalFallback','-DDC-ForceMemoryCache')}
            if($Role -eq 'Server') {
                $Arguments+=@('-server','-NullRHI',('-port='+$Port),'-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyPopulation=160',
                    '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemySpawnInterval=0.05','-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyRespawnDelay=600')
            } else {
                # Shipping intentionally ignores a positional travel URL. Both
                # client builds use the same bounded loopback benchmark entry;
                # native pending connection / handshake / replication are unchanged.
                $Arguments+=('-PvpBaselineConnect='+$Travel)
                if($IsEditor){$Arguments+='-game'}
                if($Role -eq $RenderRole){$Arguments+=@('-dx12','-Windowed','-ForceRes','-ResX=1600','-ResY=900','-NoVSync','-PvpBaselineRendered')}
                else{$Arguments+='-NullRHI'}
            }
            if($CaptureScreenshots -and $Role -eq $RenderRole){$Arguments+='-PvpBaselineScreenshot'}
            if($DiagnosticTrace -and ($Role -eq 'Server' -or $Role -eq $RenderRole)){
                $Channels=if($Role -eq 'Server'){'cpu,frame,bookmark,task,log,counters'}else{'cpu,gpu,frame,bookmark,task,log,counters'}
                $TracePath=Join-Path $DiagnosticCase ($Role+'.utrace')
                $CsvFrames=if($Role -eq 'Server'){$DiagnosticServerCsvFrames}else{$DiagnosticClientCsvFrames}
                $Arguments+=@(('-trace='+$Channels),('-tracefile="'+$TracePath+'"'),'-statnamedevents','-PvpBaselineCpuAccounting')
                if(-not $DiagnosticTraceOnly){
                    $Arguments+=@(('-csvCaptureFrames='+$CsvFrames),
                        '-csvCategories=Animation,AnimationParallelEvaluation,CharacterMovement,CharacterMovementDetailed,ChaosPhysics,ChaosPhysicsSolver,Networking,NavigationSystem,fpstrueAI,fpstrueSignificance,fpstrueEnemyUI',
                        '-csvGpuStats','-csvCompression=0')
                }
                $DiagnosticInfo.Roles[$Role]=[ordered]@{Trace=$TracePath;Channels=$Channels;NativeCsvFrames=if($DiagnosticTraceOnly){$null}else{$CsvFrames};UserDirectory=$User}
            }
            $Process=Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
            $Owned+=[pscustomobject]@{Role=$Role;Id=$Process.Id;Process=$Process;StartedUtc=$Process.StartTime.ToUniversalTime();Executable=$Executable;ControlPath=$RoleControl;Forced=$false}
            $Launches+=[pscustomobject]@{Role=$Role;Id=$Process.Id;StartedUtc=$Process.StartTime.ToUniversalTime().ToString('o');Executable=$Executable;ControlPath=$RoleControl;Arguments=$Arguments}
            if($Role -eq 'Server'){Start-Sleep -Seconds 3}
        }
        Write-BaselineJson $Launches (Join-Path $CaseRoot 'launch.json')
        if($DiagnosticTrace){Write-BaselineJson $DiagnosticInfo (Join-Path $DiagnosticCase 'diagnostic-manifest.json')}
        Write-Output ('Baseline '+$Case.Name+' (160 AI, '+$Case.Kind+' rendered client, fixed Editor DS).')
        while($true) {
            if(((Get-Date)-$Started).TotalSeconds -gt $TimeoutSeconds){throw ('Timeout in '+$Phase)}
            if($DiagnosticTrace -and (Get-PSDrive -Name C -PSProvider FileSystem).Free -lt 5GB){throw 'Diagnostic disk guard: C fell below 5 GiB; stop and retain invalid partial evidence.'}
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
                if($null -ne $Scheduler){Start-SchedulerRecording $Scheduler; $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler; Write-BaselineJson $DiagnosticInfo (Join-Path $DiagnosticCase 'diagnostic-manifest.json')}
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
                if($null -ne $Scheduler){Stop-SchedulerRecording $Scheduler; $DiagnosticInfo.Scheduler=Get-SchedulerEvidence $Scheduler; Write-BaselineJson $DiagnosticInfo (Join-Path $DiagnosticCase 'diagnostic-manifest.json')}
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
            if($Summary.schemaVersion -ne 1 -or $Summary.role -cne $Entry.Role -or $Summary.pid -ne $Entry.Id -or $Summary.validationSamples -lt 100){throw ('Summary identity or validation coverage mismatch: '+$Entry.Role)}
            if($Summary.footIKQueries -cne $FootIKQueries -or $Status.footIKQueries -cne $FootIKQueries){throw ('Native Foot IK query mode mismatch: '+$Entry.Role)}
            if($DiagnosticTrace -and $Entry.Role -in @('Server',$RenderRole)){
                $Cpu=$Summary.cpuAccounting
                if($null -eq $Cpu -or -not $Cpu.enabled -or -not $Cpu.platformSupported -or -not $Cpu.valid -or
                    $Cpu.validFrameCpuIntervals -ne $Summary.frames -or $Cpu.validWorldCpuIntervals -ne $Summary.frames -or
                    $Cpu.osQueryFailures -ne 0 -or $Cpu.negativeFrameCpuDeltas -ne 0 -or $Cpu.negativeWorldCpuDeltas -ne 0){
                    throw ('Missing or incomplete native GameThread CPU accounting: '+$Entry.Role)
                }
                $DiagnosticInfo.Roles[$Entry.Role].CpuAccounting=$Cpu
                $DiagnosticInfo.Roles[$Entry.Role].ClockAnchors=$Summary.traceAlignment
            }
            if($Summary.minEnemies -ne 160 -or $Summary.maxEnemies -ne 160 -or $Summary.minPlayers -ne 2 -or $Summary.maxPlayers -ne 2 -or
                $Summary.captureSeconds -lt 59.8 -or $Summary.captureSeconds -gt 75 -or $Summary.actualMapPackage -cne $Map){throw ('Population, duration or map mismatch: '+$Entry.Role)}
            if($Entry.Role -eq 'Server') {
                $Activity=$Summary.aiActivity
                if($null -eq $Activity -or $Activity.minControllers -ne 160 -or $Activity.maxControllers -ne 160 -or
                    $Activity.validationSamples -ne $Summary.validationSamples -or
                    ($Activity.maxMoving -le 0 -and $Activity.maxAttacking -le 0)) {
                    throw 'Server AI activity invalid: require 160 controllers throughout and observed movement or attacks.'
                }
            }
            $ExpectedShipping=$Case.Kind -eq 'Shipping' -and $Entry.Role -ne 'Server'
            if([bool]$Summary.isShipping -ne $ExpectedShipping -or [bool]$Summary.rendered -ne ($Entry.Role -eq $RenderRole)){throw ('Build/render identity mismatch: '+$Entry.Role)}
            if($Entry.Role -eq $RenderRole){
                if($Summary.width -ne 1600 -or $Summary.height -ne 900 -or $Summary.nullRhi){throw 'Actual rendered viewport mismatch.'}
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
            # The native counter is per selected World/engine frame. Keep valid
            # zero-evaluation frames in the denominator; absent World ticks are
            # explicitly uncovered, never silently turned into valid zero work.
            $IKSummary=$Summary.enemyFootIK
            $IKCountColumns=[ordered]@{EnemyIKEvaluations='evaluations';EnemyIKWorldQueries='worldQueries';EnemyIKComponentQueries='componentQueries';
                EnemyIKReusedFootprints='reusedFootprints';EnemyIKFallbackFootprints='fallbackFootprints'}
            foreach($Row in $Rows){
                if($Row.EnemyIKStatsValid -cnotin @('0','1') -or $Row.EnemyIKStatsValid -cne $Row.WorldTickValid){throw 'Foot IK frame validity does not match the selected World/frame.'}
                if([string]::IsNullOrWhiteSpace($Row.EnemyIKSolveWallMs) -or [double]::IsNaN([double]$Row.EnemyIKSolveWallMs) -or
                    [double]::IsInfinity([double]$Row.EnemyIKSolveWallMs) -or [double]$Row.EnemyIKSolveWallMs -lt 0){throw 'Invalid enemy IK solve wall time.'}
                foreach($Column in $IKCountColumns.Keys){if($Row.$Column -notmatch '^\d+$'){throw ('Invalid Foot IK frame count: '+$Column)}}
                if($Row.EnemyIKStatsValid -eq '0' -and ([double]$Row.EnemyIKSolveWallMs -ne 0 -or
                    @($IKCountColumns.Keys | Where-Object {[uint64]$Row.$_ -ne 0}).Count)){throw 'A missing World frame contains stale Foot IK measurements.'}
            }
            $IKRows=@($Rows | Where-Object EnemyIKStatsValid -eq '1')
            $IKActiveFrames=@($IKRows | Where-Object {[uint64]$_.EnemyIKEvaluations -gt 0}).Count
            $IKRequiredFields=@('sampledFrames','validWorldFrames','completeFrameCoverage','framesWithEvaluations','zeroEvaluationFrames',
                'solveWallTotalMs','meanSolveWallMsPerFrame','maxSolveWallMs','evaluations','worldQueries','componentQueries','reusedFootprints','fallbackFootprints','timingBoundary')
            if($null -eq $IKSummary -or @($IKRequiredFields | Where-Object {$_ -cnotin $IKSummary.PSObject.Properties.Name}).Count){throw 'Missing native Foot IK summary fields.'}
            if($null -eq $IKSummary -or $IKSummary.sampledFrames -ne $Rows.Count -or $IKSummary.validWorldFrames -ne $IKRows.Count -or
                [bool]$IKSummary.completeFrameCoverage -ne ($IKRows.Count -eq $Rows.Count) -or $IKSummary.framesWithEvaluations -ne $IKActiveFrames -or
                $IKSummary.zeroEvaluationFrames -ne ($IKRows.Count-$IKActiveFrames)){throw 'Foot IK summary frame coverage mismatch.'}
            foreach($Column in $IKCountColumns.Keys){
                $Total=($IKRows | Measure-Object -Property $Column -Sum).Sum
                $NativeTotal=$IKSummary.($IKCountColumns[$Column])
                if([string]$NativeTotal -notmatch '^\d+$' -or [double]$NativeTotal -ne [double]$Total){throw ('Foot IK summary count mismatch: '+$Column)}
            }
            foreach($Field in @('solveWallTotalMs','meanSolveWallMsPerFrame','maxSolveWallMs')){
                $Value=$IKSummary.$Field
                if($null -eq $Value -or [double]::IsNaN([double]$Value) -or [double]::IsInfinity([double]$Value) -or [double]$Value -lt 0){throw ('Invalid native Foot IK wall timing: '+$Field)}
            }
            $IKWallTotal=($IKRows | Measure-Object -Property EnemyIKSolveWallMs -Sum).Sum
            $IKWallMean=if($IKRows.Count){$IKWallTotal/$IKRows.Count}else{0.0}
            $IKWallMax=if($IKRows.Count){($IKRows | Measure-Object -Property EnemyIKSolveWallMs -Maximum).Maximum}else{0.0}
            $IKRoundingTolerance=[Math]::Max(0.000001,$Rows.Count*0.00000051)
            if([Math]::Abs([double]$IKSummary.solveWallTotalMs-[double]$IKWallTotal) -gt $IKRoundingTolerance -or
                [Math]::Abs([double]$IKSummary.meanSolveWallMsPerFrame-[double]$IKWallMean) -gt 0.0000011 -or
                [Math]::Abs([double]$IKSummary.maxSolveWallMs-[double]$IKWallMax) -gt 0.0000011){throw 'Foot IK summary timing differs from frame samples.'}
            $Result.Roles[$Entry.Role]=[ordered]@{Samples=$Values.Count;FrameEndIntervalMeanMs=$Mean;P95Ms=(Get-Percentile $Values 0.95);
                P99Ms=(Get-Percentile $Values 0.99);MaxMs=($Values | Measure-Object -Maximum).Maximum;EquivalentFPS=1000.0/$Mean;
                FramesOver16_667Percent=100.0*@($Values | Where-Object {$_ -gt (1000.0/60.0)}).Count/$Values.Count;
                WorldTickWall=$WorldTickResult;EnemyFootIK=$IKSummary;AIActivity=$Summary.aiActivity;RawSummary=$Summary}
        }
        if((Get-BaselineFingerprint | ConvertTo-Json -Depth 4 -Compress) -ne $FingerprintText){throw 'Inputs changed during case.'}
        if($DiagnosticTrace){
            foreach($Role in @('Server',$RenderRole)){
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
        if($DiagnosticTrace){
            $DiagnosticInfo.FinishedUtc=[datetime]::UtcNow.ToString('o');$DiagnosticInfo.Failure=$Result.Error
            Write-BaselineJson $DiagnosticInfo (Join-Path $DiagnosticCase 'diagnostic-manifest.json')
        }
        Write-BaselineJson $Result (Join-Path $CaseRoot 'result.json')
        $Results+=[pscustomobject]$Result
        Write-BaselineJson $Results (Join-Path $Run 'results.json')
    }
    if(-not $Result.Valid){throw ('Baseline stopped; invalid evidence retained: '+$CaseRoot)}
}
$Aggregates=@()
foreach($Kind in $BuildKinds){
    $Runs=@($Results | Where-Object Kind -eq $Kind)
    $Frames=@($Runs | ForEach-Object {$_.Roles[$RenderRole].FrameEndIntervalMeanMs})
    $IKRuns=@($Runs | ForEach-Object {$_.Roles[$RenderRole].EnemyFootIK})
    $IKComplete=@($IKRuns | Where-Object {-not $_.completeFrameCoverage}).Count -eq 0
    $Aggregates+=[pscustomobject]@{Build=$Kind;Repeats=$Runs.Count;RenderedRole=$RenderRole;FootIKQueries=$FootIKQueries;
        MeanFrameEndIntervalMs=($Frames | Measure-Object -Average).Average;
        MeanRunP95Ms=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].P95Ms}) | Measure-Object -Average).Average;
        MeanRunP99Ms=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].P99Ms}) | Measure-Object -Average).Average;
        MeanRunOver16_667Percent=(@($Runs | ForEach-Object {$_.Roles[$RenderRole].FramesOver16_667Percent}) | Measure-Object -Average).Average;
        MeanServerWorldTickWallMs=(@($Runs | ForEach-Object {$_.Roles.Server.WorldTickWall.MeanMs}) | Measure-Object -Average).Average;
        EnemyIKCompleteFrameCoverage=$IKComplete;
        MeanEnemyIKSolveWallMsPerFrame=if($IKComplete){($IKRuns | Measure-Object -Property meanSolveWallMsPerFrame -Average).Average}else{$null};
        MeanEnemyIKWorldQueriesPerFrame=if($IKComplete){(@($IKRuns | ForEach-Object {$_.worldQueries/$_.validWorldFrames}) | Measure-Object -Average).Average}else{$null};
        MeanEnemyIKComponentQueriesPerFrame=if($IKComplete){(@($IKRuns | ForEach-Object {$_.componentQueries/$_.validWorldFrames}) | Measure-Object -Average).Average}else{$null}}
}
Write-BaselineJson $Aggregates (Join-Path $Run 'summary.json')
Write-Output ('Completed separate build baselines: '+$Run)
$Aggregates | Format-Table -AutoSize
