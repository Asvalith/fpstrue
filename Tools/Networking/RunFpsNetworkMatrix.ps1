<#
Bounded FPS networking verification. Functional runs reuse RunPvpSmoke; capacity
runs use the game's explicit normal-input driver, never synthetic shot ACKs or
server damage shortcuts. Trace is an optional separate diagnostic; trace timings
cannot pass the no-trace capacity budget. Every peer is owned by this invocation.
Budgets are frozen before launch. Missing/reset counters are failures, not zero.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:\ueprojrct\fpstrue_safe2',
    [string]$EngineRoot = 'E:\program\ue554\UE_5.5',
    [string]$Map = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked',
    [ValidateSet('Functional','Capacity','All')][string]$Suite = 'All',
    [ValidateSet('Normal','Weak')][string[]]$Profiles = @('Normal','Weak'),
    [ValidateRange(2,8)][int[]]$ClientCounts = @(2,4,8),
    [ValidateRange(1,3)][int]$Repeat = 3,
    [ValidateRange(0,160)][int]$EnemyPopulation = 160,
    [ValidateRange(5,60)][int]$WarmupSeconds = 10,
    [ValidateRange(10,120)][int]$CaptureSeconds = 45,
    [ValidateRange(1,65500)][int]$Port = 17931,
    [ValidateRange(90,600)][int]$TimeoutSeconds = 300,
    [ValidateSet('30')][string]$ServerTickRate = '30',
    [ValidateSet('System','Respect')][string]$ServerTimerResolutionPolicy = 'System',
    [string]$BudgetFile = '',
    [switch]$EnableNetTrace,
    [switch]$DropFirstFireActivation,
    [ValidateRange(128,8192)][int]$TraceMaxMiB = 1024,
    [switch]$RenderedReconnect,
    [string]$AnalyzeDirectory = '',
    [switch]$DryRun,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
if($DropFirstFireActivation -and (-not $EnableNetTrace -or $Suite -ne 'Capacity')){
    throw 'DropFirstFireActivation requires the explicit Capacity trace diagnostic; it cannot certify normal network budgets.'
}

function Read-NetworkFields([string]$Text) {
    $Fields = @{}
    foreach ($Match in [regex]::Matches($Text, '(\w+)=([^\s]+)')) { $Fields[$Match.Groups[1].Value] = $Match.Groups[2].Value }
    return $Fields
}
function Read-NetworkNumber($Value) {
    $Number = 0.0
    if ($null -eq $Value -or -not [double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$Number) -or [double]::IsNaN($Number) -or [double]::IsInfinity($Number)) { return $null }
    return $Number
}
function Get-NetworkPercentile([object[]]$Values, [double]$Fraction) {
    $Finite = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($Finite.Count -eq 0) { return $null }
    return [math]::Round([double]$Finite[[math]::Max(0,[math]::Ceiling($Fraction*$Finite.Count)-1)], 4)
}
function Get-NetworkMean([object[]]$Values) {
    $Finite=@($Values|Where-Object {$null -ne $_})
    if($Finite.Count -eq 0){return $null}
    return [math]::Round([double](($Finite|Measure-Object -Average).Average),4)
}
function Get-NetworkWindow([string]$Text) {
    $Start = @([regex]::Matches($Text, 'PVP_CAPACITY\s+START\b[^\r\n]*'))
    $Stop = @([regex]::Matches($Text, 'PVP_CAPACITY\s+STOP\b[^\r\n]*'))
    if ($Start.Count -ne 1 -or $Stop.Count -ne 1 -or $Stop[0].Index -le $Start[0].Index) { throw 'Capacity capture needs exactly one ordered START and STOP per peer.' }
    $StartFields = Read-NetworkFields $Start[0].Value; $StopFields = Read-NetworkFields $Stop[0].Value
    $From = Read-NetworkNumber $StartFields.LocalSeconds; $To = Read-NetworkNumber $StopFields.LocalSeconds
    if ($null -eq $From -or $null -eq $To -or $To -le $From) { throw 'Capture markers lack a valid peer-local monotonic interval.' }
    return [pscustomobject]@{Start=$From;Stop=$To;Seconds=$To-$From;StartFields=$StartFields;StopFields=$StopFields}
}
function Get-NetworkConnectionRates([string]$Text, $Window, [bool]$Server) {
    $Connections = @{}
    foreach ($Line in [regex]::Matches($Text, 'PVP_NET_SAMPLE\b[^\r\n]*')) {
        $Fields = Read-NetworkFields $Line.Value
        $Time = Read-NetworkNumber $Fields.LocalSeconds
        if ($null -eq $Time -or $Time -lt $Window.Start -or $Time -gt $Window.Stop) { continue }
        if (-not $Fields.Connection) { throw 'A network sample has no stable connection identity.' }
        if (-not $Connections.ContainsKey($Fields.Connection)) { $Connections[$Fields.Connection] = [Collections.Generic.List[object]]::new() }
        $Numbers = @{Time=$Time}
        foreach ($Name in @('InBytes','OutBytes','InPackets','OutPackets','MovementCorrections')) {
            $Number = Read-NetworkNumber $Fields[$Name]
            if ($null -eq $Number -or $Number -ne [math]::Floor($Number) -or ($Name -ne 'MovementCorrections' -and $Number -lt 0)) { throw "Missing/invalid cumulative $Name in connection $($Fields.Connection)." }
            $Numbers[$Name] = $Number
        }
        foreach($Name in @('OutLost','InLost')){$Numbers[$Name]=Read-NetworkNumber $Fields[$Name]}
        $Connections[$Fields.Connection].Add([pscustomobject]$Numbers)
    }
    $Result = @()
    foreach ($Connection in $Connections.Keys) {
        $Rows = @($Connections[$Connection] | Sort-Object Time)
        if ($Rows.Count -lt 2) { throw "Connection $Connection has fewer than two samples." }
        $Seconds = $Rows[-1].Time - $Rows[0].Time
        if ($Seconds -lt $Window.Seconds - 2.5) { throw "Connection $Connection does not cover the capture window." }
        for ($Index=1; $Index -lt $Rows.Count; ++$Index) {
            if ($Rows[$Index].Time -le $Rows[$Index-1].Time -or $Rows[$Index].Time-$Rows[$Index-1].Time -gt 2.5) { throw "Connection $Connection has unordered or missing samples." }
            foreach ($Name in @('InBytes','OutBytes','InPackets','OutPackets')) {
                if ($Rows[$Index].$Name -lt $Rows[$Index-1].$Name) { throw "Connection $Connection counter $Name wrapped or reset." }
            }
            if (-not $Server -and ($Rows[$Index].MovementCorrections -lt 0 -or $Rows[$Index].MovementCorrections -lt $Rows[$Index-1].MovementCorrections)) { throw 'Client movement correction counter is unavailable or reset.' }
        }
        $Result += [pscustomobject]@{
            Connection=$Connection;SampleCount=$Rows.Count;Seconds=$Seconds
            OutKiBPerSecond=($Rows[-1].OutBytes-$Rows[0].OutBytes)/$Seconds/1024
            InKiBPerSecond=($Rows[-1].InBytes-$Rows[0].InBytes)/$Seconds/1024
            OutPacketsPerSecond=($Rows[-1].OutPackets-$Rows[0].OutPackets)/$Seconds
            InPacketsPerSecond=($Rows[-1].InPackets-$Rows[0].InPackets)/$Seconds
            EngineObservedOutLostPackets=if($null -ne $Rows[-1].OutLost -and $null -ne $Rows[0].OutLost -and $Rows[-1].OutLost -ge $Rows[0].OutLost){$Rows[-1].OutLost-$Rows[0].OutLost}else{$null}
            EngineObservedInLostPackets=if($null -ne $Rows[-1].InLost -and $null -ne $Rows[0].InLost -and $Rows[-1].InLost -ge $Rows[0].InLost){$Rows[-1].InLost-$Rows[0].InLost}else{$null}
            MovementCorrectionsPerSecond=if($Server){$null}else{($Rows[-1].MovementCorrections-$Rows[0].MovementCorrections)/$Seconds}
        }
    }
    return $Result
}
function Get-NetworkFrameCosts([string]$Text, $Window) {
    $Rows = @(); $Frames = @{}
    foreach ($Line in [regex]::Matches($Text, 'PVP_NET_FRAME\b[^\r\n]*')) {
        $Fields=Read-NetworkFields $Line.Value; $Time=Read-NetworkNumber $Fields.LocalSeconds
        if ($null -eq $Time -or $Time -lt $Window.Start -or $Time -gt $Window.Stop) { continue }
        $Frame=Read-NetworkNumber $Fields.Frame
        if ($null -eq $Frame -or $Frame -lt 0 -or $Frame -ne [math]::Floor($Frame) -or $Frames.ContainsKey([string]$Frame)) { throw 'Frame identity is missing or duplicated.' }
        $Frames[[string]$Frame]=$true
        $TickFlush=Read-NetworkNumber $Fields.ReplicationTickFlushMs; $WorldCpu=Read-NetworkNumber $Fields.WorldTickCpuMs
        $WorldWall=Read-NetworkNumber $Fields.WorldTickWallMs
        $PacingIdle=Read-NetworkNumber $Fields.PacingIdleMs;$IdleOvershoot=Read-NetworkNumber $Fields.IdleOvershootMs
        $Rows += [pscustomobject]@{Frame=$Frame;Time=$Time;TickFlush=if($null -ne $TickFlush -and $TickFlush -ge 0){$TickFlush}else{$null};WorldCpu=if($null -ne $WorldCpu -and $WorldCpu -ge 0){$WorldCpu}else{$null}
            WorldWall=if($null -ne $WorldWall -and $WorldWall -ge 0){$WorldWall}else{$null}
            PacingIdle=if($null -ne $PacingIdle -and $PacingIdle -ge 0){$PacingIdle}else{$null};IdleOvershoot=if($null -ne $IdleOvershoot -and $IdleOvershoot -ge 0){$IdleOvershoot}else{$null}}
    }
    $Rows=@($Rows|Sort-Object Time,Frame)
    if ($Rows.Count -lt 2) { throw 'Frame CPU/tick-flush instrumentation is missing.' }
    $Span=$Rows[-1].Frame-$Rows[0].Frame+1
    if ($Span -le 0) { throw 'Frame samples are unordered.' }
    $Valid=@($Rows | Where-Object { $null -ne $_.TickFlush -and $null -ne $_.WorldCpu })
    $Intervals=@()
    for($Index=1;$Index -lt $Rows.Count;++$Index){
        if($Rows[$Index].Frame -le $Rows[$Index-1].Frame -or $Rows[$Index].Time -le $Rows[$Index-1].Time){throw 'Frame timestamps or identities are unordered.'}
        $Intervals+=($Rows[$Index].Time-$Rows[$Index-1].Time)*1000
    }
    return [pscustomobject]@{Frames=$Rows.Count;Span=$Span;ValidFrames=$Valid.Count;Coverage=$Valid.Count/$Span
        WindowCoverage=($Rows[-1].Time-$Rows[0].Time)/$Window.Seconds
        FrameIntervalP95Ms=(Get-NetworkPercentile $Intervals .95)
        WorldTickCpuMeanMs=(Get-NetworkMean @($Rows|ForEach-Object WorldCpu))
        WorldTickCpuP95Ms=(Get-NetworkPercentile @($Rows | ForEach-Object WorldCpu) 0.95)
        WorldTickWallSamples=@($Rows|Where-Object {$null -ne $_.WorldWall}).Count
        WorldTickWallMeanMs=(Get-NetworkMean @($Rows|ForEach-Object WorldWall));WorldTickWallP95Ms=(Get-NetworkPercentile @($Rows|ForEach-Object WorldWall) .95)
        ReplicationTickFlushP95Ms=(Get-NetworkPercentile @($Rows | ForEach-Object TickFlush) 0.95)
        PacingIdleSamples=@($Rows|Where-Object {$null -ne $_.PacingIdle}).Count
        PacingIdleMeanMs=(Get-NetworkMean @($Rows|ForEach-Object PacingIdle));PacingIdleP95Ms=(Get-NetworkPercentile @($Rows|ForEach-Object PacingIdle) .95)
        IdleOvershootSamples=@($Rows|Where-Object {$null -ne $_.IdleOvershoot}).Count
        IdleOvershootMeanMs=(Get-NetworkMean @($Rows|ForEach-Object IdleOvershoot));IdleOvershootP95Ms=(Get-NetworkPercentile @($Rows|ForEach-Object IdleOvershoot) .95)
        Interpretation='WorldTickCpu is GameThread execution measured with GetThreadTimes; single-frame samples may be quantized (15.625 ms steps observed on this host), so decimal formatting does not imply sub-millisecond precision. Prefer cumulative/mean cost for attribution. WorldTickWall brackets this World tick with a monotonic clock; wall minus CPU is not measured blocked time and also includes scheduling/quantization effects. ReplicationTickFlush is driver elapsed scope and may overlap World work. Never add these costs or divide them across connections. World wall and pacing idle/overshoot are optional diagnostics with no additional acceptance thresholds; missing observations remain null.'}
}
function Read-NetworkLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $Stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
    $Reader=[IO.StreamReader]::new($Stream)
    try { return $Reader.ReadToEnd() } finally { $Reader.Dispose();$Stream.Dispose() }
}
function Write-NetworkJson($Object,[string]$Path) {
    [IO.File]::WriteAllText($Path,($Object | ConvertTo-Json -Depth 14),[Text.UTF8Encoding]::new($false))
}
function Get-NetworkFingerprint([string]$Root,[string]$MapPackage) {
    $Paths=@((Join-Path $Root 'fpstrue.uproject'),(Join-Path $Root 'Binaries/Win64/UnrealEditor-fpstrue.dll'),(Join-Path $Root ('Content/'+$MapPackage.Substring(6)+'.umap')))
    $Paths+=@(Get-ChildItem -LiteralPath (Join-Path $Root 'Source'),(Join-Path $Root 'Config') -Recurse -File | Where-Object {$_.Extension -in @('.h','.cpp','.cs','.ini')} | Select-Object -ExpandProperty FullName)
    return @($Paths | Sort-Object -Unique | ForEach-Object {[pscustomobject]@{Path=$_;Sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
}
function Test-NetworkCapacity([string]$Directory,[int]$Clients,$Budget,[string]$Profile,[int]$Enemies=160,[string]$TimerPolicy='System') {
    $Failures=[Collections.Generic.List[string]]::new();$Roles=@{}
    foreach ($Role in @('Server')+@(1..$Clients | ForEach-Object {'Client'+$_})) {
        try {
            $Text=Read-NetworkLog (Join-Path $Directory ($Role+'.log'))
            $Environment=$null
            if($Role -eq 'Server'){
                $EnvironmentRecord=[regex]::Match($Text,'PVP_NET_ENV\b[^\r\n]*\bTimerPolicy=[^\r\n]*')
                if($EnvironmentRecord.Success){$Environment=Read-NetworkFields $EnvironmentRecord.Value}
                if($TimerPolicy -eq 'Respect' -and ($null -eq $Environment -or $Environment.TimerPolicy -ne 'Respect' -or $Environment.Applied -ne '1')){$Failures.Add('Server: requested timer-resolution policy was not confirmed as applied.')}
            }
            $Ready=[regex]::Match($Text,'PVP_CAPACITY\s+READY\b[^\r\n]*')
            $Done=[regex]::Match($Text,'PVP_CAPACITY\s+DONE\b[^\r\n]*')
            if(-not $Ready.Success -or -not $Done.Success){throw 'Actual readiness or completed drain evidence is missing.'}
            $ReadyFields=Read-NetworkFields $Ready.Value;$DoneFields=Read-NetworkFields $Done.Value
            if($DoneFields.Outstanding -ne '0' -or $DoneFields.Overflow -ne '0'){throw 'Drain has outstanding inputs or measurement-buffer overflow.'}
            if($Role -eq 'Server' -and ($ReadyFields.Clients -ne [string]$Clients -or $ReadyFields.Enemies -ne [string]$Enemies)){throw 'Server population/readiness does not match the frozen workload.'}
            $Window=Get-NetworkWindow $Text
            if ($Window.Seconds -lt [double]$Budget.MinimumCaptureSeconds) { $Failures.Add("${Role}: capture shorter than the frozen minimum.") }
            $Rates=@(Get-NetworkConnectionRates $Text $Window ($Role -eq 'Server'))
            $Expected=if($Role -eq 'Server'){$Clients}else{1}
            if($Rates.Count -ne $Expected){$Failures.Add("${Role}: expected $Expected stable parent connections, found $($Rates.Count).")}
            foreach($Rate in $Rates){
                $OutBudget=if($Role -eq 'Server'){$Budget.MaximumServerConnectionOutKiBPerSecond}else{$Budget.MaximumClientOutKiBPerSecond}
                $PacketBudget=if($Role -eq 'Server'){$Budget.MaximumServerConnectionOutPacketsPerSecond}else{$Budget.MaximumClientOutPacketsPerSecond}
                if($Rate.OutKiBPerSecond -gt $OutBudget){$Failures.Add("${Role}: connection $($Rate.Connection) exceeds send-byte budget.")}
                if($Rate.OutPacketsPerSecond -gt $PacketBudget){$Failures.Add("${Role}: connection $($Rate.Connection) exceeds packet budget.")}
                if($Role -ne 'Server' -and $Rate.MovementCorrectionsPerSecond -gt $Budget.MaximumClientMovementCorrectionsPerSecond){$Failures.Add("${Role}: movement correction rate exceeds budget.")}
            }
            $Costs=Get-NetworkFrameCosts $Text $Window
            if($Costs.Coverage -lt $Budget.MinimumFrameCoverage -or $Costs.WindowCoverage -lt $Budget.MinimumFrameCoverage){$Failures.Add("${Role}: missing frame-cost coverage.")}
            if($Role -eq 'Server'){
                if($Costs.WorldTickCpuP95Ms -gt $Budget.MaximumServerWorldTickCpuP95Ms){$Failures.Add('Server: World CPU P95 exceeds budget.')}
                if($Costs.FrameIntervalP95Ms -gt $Budget.MaximumServerFrameIntervalP95Ms){$Failures.Add('Server: frame interval P95 misses the 30 Hz workload budget.')}
                if($Costs.ReplicationTickFlushP95Ms -gt $Budget.MaximumServerReplicationTickFlushP95Ms){$Failures.Add('Server: replication TickFlush P95 exceeds budget.')}
            }
            $Roles[$Role]=[pscustomobject]@{Window=$Window;Connections=$Rates;FrameCosts=$Costs;TimerEnvironment=$Environment}
        }
        catch{$Failures.Add("${Role}: $($_.Exception.Message)")}
    }
    $Latency=$null
    try{
        $Latency=& (Join-Path $PSScriptRoot 'MeasureFpsShotLatency.ps1') -RunDirectory $Directory -SameHostUtc | ConvertFrom-Json
        $ProfileBudget=$Budget.Profiles.$Profile
        foreach($Index in 1..$Clients){
            $Client=@($Latency.Clients | Where-Object Client -eq ('Client'+$Index))
            if($Client.Count -ne 1){$Failures.Add("Client${Index}: missing shot ledger.");continue}
            $Row=$Client[0]
            if($Row.Predicted -lt $Budget.MinimumShotsPerClient){$Failures.Add("Client${Index}: insufficient actual shot input.")}
            if($Row.MissingFinalAck -gt 0 -or $Row.InvalidIdentityEvents -gt 0 -or $Row.OrphanShotIdentities -gt 0 -or $Row.ShotsWithDiagnosticIssues -gt 0){$Failures.Add("Client${Index}: incomplete or inconsistent shot confirmation.")}
            foreach($Percentile in @('P95','P99')){
                $Value=$Row.('PredictionToAck'+$Percentile+'Ms');$Limit=$ProfileBudget.('MaximumPredictionToAck'+$Percentile+'Ms')
                if($null -eq $Value -or $Value -gt $Limit){$Failures.Add("Client${Index}: $Percentile input-to-final-ACK exceeds budget or is missing.")}
            }
            if($null -eq $Row.RejectionPercent -or $Row.RejectionPercent -gt $ProfileBudget.MaximumUnexpectedRejectionPercent){$Failures.Add("Client${Index}: shot rejection budget exceeded or unavailable.")}
            if($null -eq $Row.PredictionEvidence -or -not $Row.PredictionEvidence.Clean -or $Row.PredictionEvidence.ImmediatePredictedDeductions -lt 1 -or $Row.PredictionEvidence.SettledAuthoritativeAmmoEvents -lt 1){$Failures.Add("Client${Index}: ammo prediction/settlement was not proven.")}
            if($null -eq $Row.PredictionEvidence -or $Row.PredictionEvidence.ConfirmedReloadsWithoutReplay -lt 1){$Failures.Add("Client${Index}: no confirmed local reload prediction was exercised.")}
        }
    }catch{$Failures.Add('Shot accounting unavailable: '+$_.Exception.Message)}
    return [pscustomobject]@{Passed=$Failures.Count -eq 0;Failures=@($Failures);Roles=$Roles;ShotLatency=$Latency
        Scope='Same-host all-client safe-space movement/fire workload. Engine connection counters include protocol overhead; no NIC-byte, rendered smoothness, player-hit fairness or public-network claim.'}
}

if($SelfTest){
    $Checks=[Collections.Generic.List[string]]::new()
    function Assert-NetworkSelfTest([string]$Name,[bool]$Condition){if(-not $Condition){throw "SelfTest failed: $Name"};$Checks.Add($Name)}
    Assert-NetworkSelfTest 'NaN stays unavailable' ($null -eq (Read-NetworkNumber 'nan'))
    Assert-NetworkSelfTest 'Empty stays unavailable' ($null -eq (Read-NetworkNumber $null))
    Assert-NetworkSelfTest 'P99 nearest rank' ((Get-NetworkPercentile @(1,2,3,100) .99) -eq 100)
    $Fixture=@'
PVP_CAPACITY START Role=Client Client=1 LocalSeconds=10
PVP_NET_SAMPLE Role=Client Client=1 LocalSeconds=10 Connection=7 OutBytes=100 InBytes=500 OutPackets=2 InPackets=4 MovementCorrections=0
PVP_NET_SAMPLE Role=Client Client=1 LocalSeconds=11 Connection=7 OutBytes=1124 InBytes=2548 OutPackets=4 InPackets=8 MovementCorrections=1
PVP_NET_FRAME Role=Client Client=1 LocalSeconds=10 Frame=10 ReplicationTickFlushMs=0 WorldTickCpuMs=3
PVP_NET_FRAME Role=Client Client=1 LocalSeconds=11 Frame=11 ReplicationTickFlushMs=1 WorldTickCpuMs=5
PVP_CAPACITY STOP Role=Client Client=1 LocalSeconds=11
'@
    $Window=Get-NetworkWindow $Fixture;$Rates=@(Get-NetworkConnectionRates $Fixture $Window $false);$Costs=Get-NetworkFrameCosts $Fixture $Window
    Assert-NetworkSelfTest 'Per-connection byte delta has correct units' ($Rates.Count -eq 1 -and $Rates[0].OutKiBPerSecond -eq 1 -and $Rates[0].InKiBPerSecond -eq 2)
    Assert-NetworkSelfTest 'Corrections use measured cumulative delta' ($Rates[0].MovementCorrectionsPerSecond -eq 1)
    Assert-NetworkSelfTest 'Measured zero TickFlush remains valid' ($Costs.Coverage -eq 1 -and $Costs.ReplicationTickFlushP95Ms -eq 1 -and $Costs.WorldTickCpuP95Ms -eq 5)
    $BufferedFrames="PVP_NET_FRAME Role=Client Client=1 LocalSeconds=11 Frame=11 ReplicationTickFlushMs=1 WorldTickCpuMs=5`nPVP_NET_FRAME Role=Client Client=1 LocalSeconds=10 Frame=10 ReplicationTickFlushMs=0 WorldTickCpuMs=3"
    $BufferedCosts=Get-NetworkFrameCosts $BufferedFrames $Window
    Assert-NetworkSelfTest 'Buffered output order cannot change original CPU-frame intervals' ($BufferedCosts.Coverage -eq 1 -and $BufferedCosts.FrameIntervalP95Ms -eq 1000)
    foreach($Case in @(
        @{Name='Counter reset invalidates rates';Text=$Fixture.Replace('OutBytes=1124','OutBytes=99')},
        @{Name='Missing counter invalidates rates';Text=$Fixture.Replace('OutPackets=4','OutPackets=nan')},
        @{Name='Unknown client corrections invalidate acceptance';Text=$Fixture.Replace('MovementCorrections=1','MovementCorrections=-1')},
        @{Name='Missing stable identity invalidates rates';Text=$Fixture.Replace('Connection=7','Connection=')}
    )){$Rejected=$false;try{$null=Get-NetworkConnectionRates $Case.Text $Window $false}catch{$Rejected=$true};Assert-NetworkSelfTest $Case.Name $Rejected}
    $MissingCosts=Get-NetworkFrameCosts ($Fixture.Replace('WorldTickCpuMs=5','WorldTickCpuMs=nan')) $Window
    Assert-NetworkSelfTest 'Missing CPU reduces coverage instead of becoming zero' ($MissingCosts.Coverage -eq .5)
    $UnavailableCosts=Get-NetworkFrameCosts ($Fixture.Replace('ReplicationTickFlushMs=1','ReplicationTickFlushMs=-1')) $Window
    Assert-NetworkSelfTest 'Unavailable previous-frame TickFlush remains missing' ($UnavailableCosts.Coverage -eq .5)
    Assert-NetworkSelfTest 'Legacy rows keep pacing observations unavailable' ($null -eq $Costs.PacingIdleMeanMs -and $null -eq $Costs.IdleOvershootP95Ms -and $Costs.PacingIdleSamples -eq 0)
    $PacingFixture=$Fixture.Replace('WorldTickCpuMs=3','WorldTickCpuMs=3 PacingIdleMs=10 IdleOvershootMs=0').Replace('WorldTickCpuMs=5','WorldTickCpuMs=5 PacingIdleMs=20 IdleOvershootMs=2')
    $PacingCosts=Get-NetworkFrameCosts $PacingFixture $Window
    Assert-NetworkSelfTest 'Pacing reports measured mean and P95 without new gates' ($PacingCosts.PacingIdleMeanMs -eq 15 -and $PacingCosts.PacingIdleP95Ms -eq 20 -and $PacingCosts.IdleOvershootMeanMs -eq 1 -and $PacingCosts.IdleOvershootP95Ms -eq 2)
    Assert-NetworkSelfTest 'Legacy rows keep World wall unavailable' ($Costs.WorldTickWallSamples -eq 0 -and $null -eq $Costs.WorldTickWallMeanMs -and $null -eq $Costs.WorldTickWallP95Ms)
    $WallFixture=$Fixture.Replace('WorldTickCpuMs=3','WorldTickCpuMs=3 WorldTickWallMs=8').Replace('WorldTickCpuMs=5','WorldTickCpuMs=5 WorldTickWallMs=12')
    $WallCosts=Get-NetworkFrameCosts $WallFixture $Window
    Assert-NetworkSelfTest 'World wall is reported independently without deriving blocked time' ($WallCosts.WorldTickWallSamples -eq 2 -and $WallCosts.WorldTickWallMeanMs -eq 10 -and $WallCosts.WorldTickWallP95Ms -eq 12)
    $Rejected=$false;try{$null=Get-NetworkWindow ($Fixture+"`nPVP_CAPACITY START LocalSeconds=12")}catch{$Rejected=$true}
    Assert-NetworkSelfTest 'Duplicate captures cannot silently merge' $Rejected
    [pscustomobject]@{Mode='SelfTest';Passed=$true;Checks=$Checks.Count;Scenarios=@($Checks);Scope='Pure parser/units/invalid-evidence checks; no UE processes or network acceptance.'}|ConvertTo-Json -Depth 4
    return
}

if(-not $BudgetFile){$BudgetFile=Join-Path $PSScriptRoot 'FpsNetworkBudgets.json'}
$Budget=Get-Content -LiteralPath $BudgetFile -Raw | ConvertFrom-Json
if($Budget.SchemaVersion -ne 1){throw 'Unsupported budget schema.'}
if($Map -notmatch '^/Game/[A-Za-z0-9_/]+$'){throw 'Map must be a package name without URL/console options.'}
if($ClientCounts.Count -eq 0 -or $Profiles.Count -eq 0 -or @($ClientCounts | Select-Object -Unique).Count -ne $ClientCounts.Count -or @($Profiles | Select-Object -Unique).Count -ne $Profiles.Count){throw 'Counts/profiles must be nonempty and distinct.'}
if($ServerTimerResolutionPolicy -ne 'System' -and $Suite -ne 'Capacity'){throw 'ServerTimerResolutionPolicy is an isolated Capacity diagnostic; select -Suite Capacity.'}
if($AnalyzeDirectory){
    if($ClientCounts.Count -ne 1 -or $Profiles.Count -ne 1){throw 'AnalyzeDirectory requires one client count and one profile.'}
    $Analysis=Test-NetworkCapacity (Resolve-Path -LiteralPath $AnalyzeDirectory).Path $ClientCounts[0] $Budget $Profiles[0] $EnemyPopulation $ServerTimerResolutionPolicy
    $Analysis | ConvertTo-Json -Depth 14
    if(-not $Analysis.Passed){exit 1}
    return
}
$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot);$EngineRoot=[IO.Path]::GetFullPath($EngineRoot)
$Run=Join-Path $ProjectRoot ('Saved/Networking/NetworkMatrix_'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'_'+[guid]::NewGuid().ToString('N').Substring(0,8))
$Cases=@()
if($Suite -in @('Functional','All')){
    foreach($Profile in $Profiles){
        $Cases+=[pscustomobject]@{Kind='Functional';Name='Functional_'+$Profile;Clients=2;Profile=$Profile;Round=1}
        $Cases+=[pscustomobject]@{Kind='Reconnect';Name='Reconnect_'+$Profile;Clients=2;Profile=$Profile;Round=1}
        if($Profile -eq 'Normal'){$Cases+=[pscustomobject]@{Kind='Policy';Name='Policy_DeterministicStale';Clients=2;Profile=$Profile;Round=1}}
    }
}
if($Suite -in @('Capacity','All')){
    foreach($Round in 1..$Repeat){foreach($Count in $ClientCounts){foreach($Profile in $Profiles){$Cases+=[pscustomobject]@{Kind='Capacity';Name=('Capacity_{0}_{1}_R{2}' -f $Count,$Profile,$Round);Clients=$Count;Profile=$Profile;Round=$Round}}}}
}
$Plan=[pscustomobject]@{Directory=$Run;Cases=$Cases;BudgetFile=[IO.Path]::GetFullPath($BudgetFile);Budget=$Budget
    TraceDiagnostic=[bool]$EnableNetTrace;FormalRepeats=$Repeat -ge $Budget.MinimumFormalRepeats
    WarmupSeconds=$WarmupSeconds;CaptureSeconds=$CaptureSeconds;EnemyPopulation=$EnemyPopulation;ServerTickRate=$ServerTickRate;ClientFrameRateCapHz=60
    ServerTimerResolutionPolicy=$ServerTimerResolutionPolicy
    TimerPolicyBoundary='System leaves OS policy unchanged. Respect is server-only diagnostic policy for existing timer-resolution requests; it is not an extra timer-resolution request and requires source Applied=1 readback. Never pool different policies.'
    Boundary='2/4/8 are requested workloads, not claimed supported capacities. Functional correctness, strict experience, illegal-late policy and capacity are separate results. Trace changes cost and cannot certify no-trace performance.'}
if($DryRun){$Plan|ConvertTo-Json -Depth 8;return}
$Editor=Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe';$Project=Join-Path $ProjectRoot 'fpstrue.uproject'
foreach($File in @($Editor,$Project,(Join-Path $ProjectRoot 'Binaries/Win64/UnrealEditor-fpstrue.dll'))){if(-not(Test-Path -LiteralPath $File -PathType Leaf)){throw "Required file missing: $File"}}
if(Test-Path -LiteralPath $Run){throw 'Refusing to overwrite a prior matrix.'}
$null=New-Item -ItemType Directory -Path $Run
Write-NetworkJson $Plan (Join-Path $Run 'Plan.json')
$BudgetHash=(Get-FileHash -LiteralPath $BudgetFile -Algorithm SHA256).Hash
$Fingerprint=Get-NetworkFingerprint $ProjectRoot $Map;$FingerprintJson=$Fingerprint|ConvertTo-Json -Depth 3 -Compress
Write-NetworkJson $Fingerprint (Join-Path $Run 'InputFingerprint.json')
$Results=[Collections.Generic.List[object]]::new();$CaseIndex=0
foreach($Case in $Cases){
    $CaseIndex++;$CasePort=$Port+$CaseIndex
    if($CasePort -gt 65535){throw 'Matrix ports exceed the UDP range.'}
    $CaseDirectory=Join-Path $Run $Case.Name;$null=New-Item -ItemType Directory -Path $CaseDirectory
    $ProfileSettings=$Budget.Profiles.($Case.Profile)
    $Result=[ordered]@{Name=$Case.Name;Kind=$Case.Kind;Clients=$Case.Clients;Profile=$Case.Profile;Round=$Case.Round;Passed=$false;Failure=$null;Directory=$CaseDirectory;DiagnosticOnly=[bool]($EnableNetTrace -or $DropFirstFireActivation -or $ServerTimerResolutionPolicy -ne 'System');ServerTimerResolutionPolicy=$ServerTimerResolutionPolicy;FaultInjection=$(if($DropFirstFireActivation){'Client1FirstFireOneWorldFlushLoss'}else{'None'})}
    try{
        if((Get-FileHash -LiteralPath $BudgetFile -Algorithm SHA256).Hash -ne $BudgetHash -or (Get-NetworkFingerprint $ProjectRoot $Map | ConvertTo-Json -Depth 3 -Compress) -cne $FingerprintJson){throw 'Frozen budget or build/source/configuration changed.'}
        if($Case.Kind -ne 'Capacity'){
            $SmokeParams=@{ProjectRoot=$ProjectRoot;EngineRoot=$EngineRoot;Map=$Map;Port=$CasePort;TimeoutSeconds=$TimeoutSeconds
                PacketLagMs=[int]$ProfileSettings.OutgoingLagMs;PacketLagVarianceMs=[int]$ProfileSettings.LagVarianceMs;PacketLossPercent=[int]$ProfileSettings.LossPercent
                IncludeReconnect=($Case.Kind -in @('Reconnect','Policy'));DeepChecks=$true;IncludePve=$true;EnableNetTrace=[bool]$EnableNetTrace;TraceMaxMiB=$TraceMaxMiB}
            if($Case.Kind -eq 'Reconnect'){$SmokeParams.ReconnectMode='TransportOutage';$SmokeParams.RenderedClients=[bool]$RenderedReconnect}
            if($Case.Kind -eq 'Policy'){$SmokeParams.WeakPolicyCorrectness=$true}
            # A policy-correct stale rejection never substitutes for this strict run.
            try{$SmokeOutput=& (Join-Path $PSScriptRoot 'RunPvpSmoke.ps1') @SmokeParams;$SmokeReport=($SmokeOutput -join "`n")|ConvertFrom-Json}
            finally{if($SmokeOutput){[IO.File]::WriteAllText((Join-Path $CaseDirectory 'SmokeOutput.json'),($SmokeOutput -join "`n"),[Text.UTF8Encoding]::new($false))}}
            $Result.Passed=[bool]$SmokeReport.Passed;$Result['FunctionalReport']=$SmokeReport
        }
        else{
            if(@([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveUdpListeners()|Where-Object Port -eq $CasePort).Count){throw "UDP port $CasePort is occupied."}
            $Owned=[Collections.Generic.List[object]]::new();$Clock=[Diagnostics.Stopwatch]::StartNew();$TracePaths=@()
            $Shared=@(('"'+$Project+'"'),'-NoLiveCoding','-NoP4','-NoSplash','-NoSteam','-NoSound','-Unattended','-NullRHI','-PvpNetworkCapacity','-PvpNetMetrics',
                '-ini:Game:[StartupActions]:bAddPacks=False',
                '-ddc=fpstrueEditorTestDDC',('-LocalDataCachePath="'+(Join-Path $ProjectRoot 'Saved/LocalDDC')+'"'),
                ('-PvpNetworkClients='+$Case.Clients),('-PvpNetworkWarmupSeconds='+$WarmupSeconds),('-PvpNetworkCaptureSeconds='+$CaptureSeconds),
                ('-PvpNetworkEnemies='+$EnemyPopulation),
                ('-PktLag='+$ProfileSettings.OutgoingLagMs),('-PktLagVariance='+$ProfileSettings.LagVarianceMs),('-PktLoss='+$ProfileSettings.LossPercent),
                '-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate=30',
                ('-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyPopulation='+$EnemyPopulation))
            try{
                foreach($RoleIndex in 0..$Case.Clients){
                    $Role=if($RoleIndex -eq 0){'Server'}else{'Client'+$RoleIndex};$Log=Join-Path $CaseDirectory ($Role+'.log')
                    $UserDirectory=Join-Path $CaseDirectory ('PrivateUsers/'+$Role)
                    $null=New-Item -ItemType Directory -Path $UserDirectory -Force
                    $Identity=[Security.Principal.WindowsIdentity]::GetCurrent().User;$Acl=[Security.AccessControl.DirectorySecurity]::new()
                    $Acl.SetOwner($Identity);$Acl.SetAccessRuleProtection($true,$false)
                    $Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($Identity,[Security.AccessControl.FileSystemRights]::FullControl,[Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',[Security.AccessControl.PropagationFlags]::None,[Security.AccessControl.AccessControlType]::Allow))
                    Set-Acl -LiteralPath $UserDirectory -AclObject $Acl
                    $Arguments=$Shared+@('-abslog="'+$Log+'"',('-PvpNetworkClient='+$RoleIndex),('-UserDir="'+$UserDirectory+'"'))
                    if($RoleIndex -eq 0){$Arguments+=@(($Map+'?game=/Script/fpstrue.fpstruePvpGameMode'),'-server','-useperfthreads',('-port='+$CasePort))}
                    else{$Arguments+=@(('127.0.0.1:'+$CasePort),'-game','-ExecCmds="t.MaxFPS 60"')}
                    if($RoleIndex -eq 0 -and $ServerTimerResolutionPolicy -eq 'Respect'){$Arguments+='-PvpTimerResolution=Respect'}
                    if($EnableNetTrace){$TracePath=Join-Path $CaseDirectory ($Role+'.utrace');$TracePaths+=$TracePath;$Arguments+=@('-trace=net,cpu,frame,bookmark','-NetTrace=2','-PvpShotLifecycleTrace',('-tracefile="'+$TracePath+'"'))}
                    if($DropFirstFireActivation -and $RoleIndex -eq 1){$Arguments+='-PvpDropFirstFireActivation'}
                    $Process=Start-Process -FilePath $Editor -ArgumentList $Arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
                    $Owned.Add([pscustomobject]@{Name=$Role;Process=$Process;Id=$Process.Id;StartedUtc=$Process.StartTime.ToUniversalTime();Path=$Editor;Log=$Log})
                    if($RoleIndex -eq 0){
                        while((Read-NetworkLog $Log) -notmatch ('IpNetDriver listening on port '+$CasePort+'\b')){
                            $Process.Refresh();if($Process.HasExited){throw 'Server exited before listening.'};if($Clock.Elapsed.TotalSeconds -gt $TimeoutSeconds){throw 'Server listening timed out.'};Start-Sleep -Milliseconds 250
                        }
                    }
                }
                $Complete=$false
                while(-not $Complete){
                    $Complete=$true
                    foreach($Peer in $Owned){
                        $Peer.Process.Refresh();$Text=Read-NetworkLog $Peer.Log
                        if($Text -match '(?im)PVP_(?:CAPACITY|SMOKE)[^\r\n]*\bFAIL\b|Fatal error:|Assertion failed:|Unhandled Exception:|Blueprint Runtime Error|Ran out of memory allocating'){throw "$($Peer.Name) reported a runtime failure."}
                        $Stopped=$Text -match 'PVP_CAPACITY\s+DONE\b'
                        if($Peer.Process.HasExited -and (-not $Stopped -or $Peer.Process.ExitCode -ne 0)){throw "$($Peer.Name) exited unsuccessfully or before capture completion."}
                        if(-not $Stopped){$Complete=$false}
                    }
                    if($EnableNetTrace){$TraceBytes=0L;foreach($TracePath in $TracePaths){if(Test-Path -LiteralPath $TracePath){$TraceBytes+=(Get-Item -LiteralPath $TracePath).Length}};if($TraceBytes -gt [long]$TraceMaxMiB*1MB){throw 'Trace size budget exceeded; diagnostic is incomplete.'}}
                    if($Clock.Elapsed.TotalSeconds -gt $TimeoutSeconds){throw 'Capacity capture timed out; missing capture/drain DONE markers are not acceptance.'}
                    if(-not $Complete){Start-Sleep -Milliseconds 250}
                }
            }
            finally{
                $CleanupFailures=[Collections.Generic.List[string]]::new()
                foreach($Peer in $Owned){
                    try{
                        $Current=Get-Process -Id $Peer.Id -ErrorAction SilentlyContinue
                        if($null -ne $Current -and $Current.StartTime.ToUniversalTime() -eq $Peer.StartedUtc -and [string]::Equals($Current.Path,$Peer.Path,[StringComparison]::OrdinalIgnoreCase)){
                            Stop-Process -Id $Current.Id -Force -ErrorAction Stop
                            if(-not $Peer.Process.WaitForExit(10000)){throw 'Owned process has not exited after cleanup.'}
                        }
                    }catch{$CleanupFailures.Add($Peer.Name+': '+$_.Exception.Message)}
                }
                $Result['Processes']=@($Owned|ForEach-Object{[pscustomobject]@{Role=$_.Name;Id=$_.Id;StartedUtc=$_.StartedUtc;Log=$_.Log}})
                $Result['CleanupFailures']=@($CleanupFailures)
            }
            if($CleanupFailures.Count -gt 0){throw ($CleanupFailures -join ' | ')}
            $Acceptance=Test-NetworkCapacity $CaseDirectory $Case.Clients $Budget $Case.Profile $EnemyPopulation $ServerTimerResolutionPolicy
            $Result['Acceptance']=$Acceptance;$Result.Passed=[bool]$Acceptance.Passed
            if(-not $Acceptance.Passed){$Result.Failure=$Acceptance.Failures -join ' | '}
            if($EnableNetTrace){$Result['NoTraceBudgetCertified']=$false;$Result.Passed=$false;$Result.Failure='Trace diagnostic retained; run without trace for capacity acceptance.'}
        }
    }
    catch{$Result.Passed=$false;$Result.Failure=$_.Exception.Message}
    try{
        if((Get-NetworkFingerprint $ProjectRoot $Map|ConvertTo-Json -Depth 3 -Compress) -cne $FingerprintJson -or (Get-FileHash -LiteralPath $BudgetFile -Algorithm SHA256).Hash -ne $BudgetHash){throw 'Inputs or frozen budgets changed during capture.'}
    }catch{$Result.Passed=$false;$Result.Failure=($Result.Failure+' '+$_.Exception.Message).Trim()}
    $Results.Add([pscustomobject]$Result);Write-NetworkJson $Result (Join-Path $CaseDirectory 'Result.json')
    Write-Output ($Case.Name+': '+$(if($Result.Passed){'PASS'}else{'FAIL - '+$Result.Failure}))
}
$Formal=$Repeat -ge $Budget.MinimumFormalRepeats -and $CaptureSeconds -ge $Budget.MinimumCaptureSeconds -and -not $EnableNetTrace -and $ServerTimerResolutionPolicy -eq 'System'
$Summary=[pscustomobject]@{Passed=@($Results|Where-Object {-not $_.Passed}).Count -eq 0;FormalCapacityCertified=($Suite -in @('Capacity','All') -and $Formal -and @($Results|Where-Object { $_.Kind -eq 'Capacity' -and -not $_.Passed }).Count -eq 0)
    Directory=$Run;Plan=$Plan;Results=@($Results);BudgetSha256=$BudgetHash;Scope='Bounded local Editor evidence only; no WAN, Shipping, anticheat, rendered smoothness or dynamic-cover fairness certification.'}
Write-NetworkJson $Summary (Join-Path $Run 'Summary.json')
$Summary|ConvertTo-Json -Depth 5
if(-not $Summary.Passed){exit 1}
