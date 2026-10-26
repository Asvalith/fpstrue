<#
Runs a headless server and two clients against the real PvP map.
RenderedClients opts into real offscreen RHI rendering and game-owned PNG
captures; the server remains headless and the default clients use NullRHI.
VisibleClients requires RenderedClients and shows the same scripted clients in
windowed mode for live observation, without changing their capture dimensions.
Local Editor mode uses UnrealEditor-Cmd -server and is not a packaged server check.
Packaged mode requires direct Development Server/Game executables under
Binaries/Win64 with matching build receipts, not the root bootstrap launchers.
Startup logs must also confirm Development configuration before a packaged run
can pass.
Pass requires gameplay markers from the server and replicated-state checks from
the clients. Rendered captures require component-state readiness and valid PNG
artifacts, but still need visual inspection. This is not a weak-network quality,
capacity or performance test.
Only processes launched by this invocation are stopped; logs remain in Saved.
RemoteLinux uses an existing staged Linux Development Server with a required
receipt/ELF hash, strict SSH trust and a unique, time/memory-bounded ordinary-user
systemd unit. It never provisions accounts, firewall rules or another service.
#>
[CmdletBinding()]
param(
    [ValidateSet('Editor', 'Packaged')][string]$ExecutionMode = 'Editor',
    [ValidateSet('Local', 'RemoteLinux')][string]$ServerMode = 'Local',
    [string]$ServerAddress,
    [string]$SshTarget,
    [string]$SshKeyPath,
    [string]$SshKnownHostsPath,
    [string]$RemotePackageRoot,
    [string]$RemoteRunUser = 'fpstrue-ds',
    [string]$RemoteServerSha256,
    [string]$EngineRoot = 'E:\program\ue554\UE_5.5',
    [string]$ProjectRoot = '',
    [string]$LocalCacheDirectory = '',
    [string]$ServerExecutable,
    [string]$ClientExecutable,
    [string]$Map = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked',
    [ValidateRange(1, 65535)][int]$Port = 7797,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180,
    [ValidateRange(0, 1000)][int]$PacketLagMs = 0,
    [ValidateRange(0, 1000)][int]$PacketLagVarianceMs = 0,
    [ValidateRange(0, 100)][int]$PacketLossPercent = 0,
    [bool]$DeepChecks = $true,
    [bool]$IncludePve = $true,
    [bool]$IncludeReconnect = $true,
    [ValidateSet('ProcessRestart', 'TransportOutage')][string]$ReconnectMode = 'ProcessRestart',
    [switch]$EnableNetTrace,
    [ValidateRange(1, 2)][int]$NetTraceVerbosity = 1,
    [ValidateRange(128, 8192)][int]$TraceMaxMiB = 1024,
    [switch]$RenderedClients,
    [switch]$VisibleClients,
    [switch]$MovingHit,
    [ValidateSet('Enabled','Disabled')][string]$TargetLatencyCompensation = 'Enabled',
    [switch]$WeakPolicyCorrectness,
    [switch]$DryRun,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
function Get-SmokeTraceArguments([string]$Path, [int]$Verbosity) {
    # Native net packets/bunches and CPU work share the same UE Trace clock.
    # Separate peer files prevent concurrent writers and ambiguous ownership.
    return @('-trace=net,cpu,frame,bookmark', ('-NetTrace=' + $Verbosity), ('-tracefile="' + $Path + '"'))
}
function Get-SmokeTraceArtifact([string]$Role, [string]$Path) {
    $Artifact = [ordered]@{ Role=$Role;Path=$Path;Bytes=$null;Sha256=$null;Complete=$false;Error=$null }
    for($Attempt=0;$Attempt -lt 10;++$Attempt){
        try {
            $Before=Get-Item -LiteralPath $Path -ErrorAction Stop
            $Artifact.Bytes=$Before.Length
            if($Before.Length -le 0){throw 'Native trace is empty.'}
            $Hash=(Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash
            $After=Get-Item -LiteralPath $Path -ErrorAction Stop
            if($After.Length -ne $Before.Length -or $After.LastWriteTimeUtc -ne $Before.LastWriteTimeUtc){throw 'Native trace changed while hashing.'}
            $Artifact.Sha256=$Hash;$Artifact.Complete=$true;$Artifact.Error=$null
            return [pscustomobject]$Artifact
        }
        catch{$Artifact.Error=$_.Exception.Message;if($Attempt -lt 9){Start-Sleep -Milliseconds 200}}
    }
    # A locked/missing trace is structured incomplete evidence. It must never
    # throw out of finally and prevent the main failed report from being saved.
    return [pscustomobject]$Artifact
}
function Get-SmokeLiveReconnectEvidence([string]$ServerText, [string]$ClientText, [int]$ClientPid, [string]$ResumeId, [int]$ReadyIndex) {
    $PidPattern = [regex]::Escape([string]$ClientPid)
    $ResumePattern = [regex]::Escape($ResumeId)
    $Failure = [regex]::Match($ClientText, 'PVP_SMOKE\s+LIVE_RECONNECT\s+FailureObserved\b[^\r\n]*\bPid=' + $PidPattern + '\b[^\r\n]*\bOperation=(\d+)\b')
    $Retry = [regex]::Match($ClientText, 'PVP_SMOKE\s+LIVE_RECONNECT\s+RetryStarted\b[^\r\n]*\bPid=' + $PidPattern + '\b[^\r\n]*\bOperation=(\d+)\b')
    $Restored = [regex]::Match($ClientText, 'PVP_SMOKE\s+LIVE_RECONNECT\s+Restored\b[^\r\n]*\bPid=' + $PidPattern + '\b[^\r\n]*\bOperation=(\d+)\b[^\r\n]*\bResumeId=' + $ResumePattern + '\b[^\r\n]*\bInitialResumeId=' + $ResumePattern + '\b[^\r\n]*\bFailureObserved=1\b')
    $Started = [regex]::Match($ServerText, 'PVP_SMOKE\s+LIVE_OUTAGE\s+Started\b[^\r\n]*\bResumeId=' + $ResumePattern + '\b')
    $Repaired = [regex]::Match($ServerText, 'PVP_SMOKE\s+LIVE_OUTAGE\s+Restored\b[^\r\n]*\bResumeId=' + $ResumePattern + '\b')
    $EngineTimeout = [regex]::Match($ClientText, 'UNetConnection::Tick:\s+Connection TIMED OUT\b')
    $SameProcess = $Failure.Success -and $Retry.Success -and $Restored.Success -and $Failure.Index -lt $Retry.Index -and $Retry.Index -lt $Restored.Index -and
        $Failure.Groups[1].Value -eq $Retry.Groups[1].Value -and $Retry.Groups[1].Value -eq $Restored.Groups[1].Value
    $ClockEvents = @([regex]::Matches($ClientText, 'CLOCK_SAMPLE PASS\b') | Where-Object { $_.Index -gt $Failure.Index -and $_.Index -lt $ReadyIndex })
    return [pscustomobject]@{
        SameClientProcess = $SameProcess
        ActualEngineTimeout = $EngineTimeout.Success -and $Failure.Success -and $EngineTimeout.Index -lt $Failure.Index
        OutageStarted = $Started.Success; OutageRestored = $Repaired.Success -and $Started.Success -and $Repaired.Index -gt $Started.Index
        ClockResampled = $Failure.Success -and $ClockEvents.Count -gt 0
        AutomaticRetryTransport = $SameProcess -and $EngineTimeout.Success -and $Started.Success -and $Repaired.Success -and $Repaired.Index -gt $Started.Index
    }
}
function Get-SmokeRecoveryPerformanceText([string]$Text, [string]$Mode, [int]$ClientPid) {
    if($Mode -ne 'TransportOutage'){return $Text}
    $Failure=[regex]::Match($Text,'PVP_SMOKE\s+LIVE_RECONNECT\s+FailureObserved\b[^\r\n]*\bPid='+[regex]::Escape([string]$ClientPid)+'\b')
    if(-not $Failure.Success){return ''}
    # Same-process recovery appends to the original log. Initial-world
    # scheduling/sharing receipts cannot prove the restored world is running.
    return $Text.Substring($Failure.Index)
}
function Get-SmokeClientRoleMap([hashtable]$ClientTexts) {
    $Rows=@();$Valid=$ClientTexts.Count -eq 2
    foreach($Name in $ClientTexts.Keys){
        $DriverIds=@([regex]::Matches($ClientTexts[$Name],'PVP_SMOKE\s+CLIENT_READY\s+Player=(\d+)\b')|ForEach-Object {$_.Groups[1].Value}|Select-Object -Unique)
        $ResumeIds=@([regex]::Matches($ClientTexts[$Name],'PVP_RESUME\s+CLIENT_READY\s+ResumeId=(\d+)\s+Restored=0\b')|ForEach-Object {$_.Groups[1].Value}|Select-Object -Unique)
        if($DriverIds.Count -ne 1 -or $ResumeIds.Count -ne 1 -or $DriverIds[0] -ne $ResumeIds[0]){$Valid=$false;continue}
        $Rows+=[pscustomobject]@{Client=$Name;PlayerId=[int]$DriverIds[0]}
    }
    $Rows=@($Rows|Sort-Object PlayerId)
    if($Rows.Count -ne 2 -or ($Rows.Count -eq 2 -and $Rows[0].PlayerId -eq $Rows[1].PlayerId)){$Valid=$false}
    return [pscustomobject]@{Valid=$Valid;Players=$Rows
        ActiveClient=if($Valid){$Rows[0].Client}else{$null};ObserverClient=if($Valid){$Rows[1].Client}else{$null}
        Contract='Original two-peer smoke: the lower server PlayerId executes gameplay; the higher observes. Client filenames and launch order do not define roles. Outage target is independently read from the server ResumeId.'}
}
function Assert-SmokeScenarioOptions([bool]$IncludePve, [bool]$IncludeReconnect,
    [bool]$RenderedClients, [bool]$VisibleClients, [bool]$MovingHit, [bool]$DeepChecks) {
    if ($VisibleClients -and -not $RenderedClients) {
        throw 'VisibleClients requires -RenderedClients; visible acceptance must use the real renderer.'
    }
    if ($MovingHit -and -not $DeepChecks) { throw 'MovingHit requires DeepChecks; a launch without the paired damage evidence cannot pass this scenario.' }
    if ($RenderedClients -and -not $IncludePve) {
        throw 'RenderedClients verifies replicated enemies as well as the local player; keep IncludePve enabled.'
    }
    if ($IncludeReconnect -and -not $IncludePve) {
        throw 'IncludeReconnect requires IncludePve; this recovery scenario verifies real offline AI damage before restoration.'
    }
}

# SelfTest reads this script's real pure functions/configuration through its AST;
# it never resolves an engine/project, starts processes, accesses SSH or writes evidence.
if ($SelfTest) {
    $SelfTokens = $null
    $SelfParseErrors = $null
    $SelfAst = [Management.Automation.Language.Parser]::ParseFile($PSCommandPath, [ref]$SelfTokens, [ref]$SelfParseErrors)
    if ($SelfParseErrors.Count -ne 0) { throw 'Smoke SelfTest could not parse its production script.' }
    $ConfigNode = $SelfAst.Find({ param($Node)
        $Node -is [Management.Automation.Language.AssignmentStatementAst] -and $Node.Left.Extent.Text -eq '$SmokeConfig'
    }, $true)
    if ($null -eq $ConfigNode) { throw 'Smoke SelfTest could not find its production scenario configuration.' }
    . ([scriptblock]::Create($ConfigNode.Extent.Text))
    foreach ($FunctionName in @('Get-SmokeShotIdentity', 'Get-SmokeShotLedger', 'Get-SmokeMovingHitEvidence', 'Get-SmokeWeakPolicyEvidence',
        'Get-SmokeRequiredDeepEvidence', 'Test-SmokeFailureLog', 'Get-SmokeTraceArguments', 'Get-SmokeLiveReconnectEvidence', 'Get-SmokeRecoveryPerformanceText', 'Get-SmokeClientRoleMap', 'Get-SmokeMovingHitIdentity', 'Test-SmokeMovingHealthDecrease', 'Get-SmokeNativeViewAlignment')) {
        $FunctionNode = $SelfAst.Find({ param($Node)
            $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq $FunctionName
        }, $true)
        if ($null -eq $FunctionNode) { throw "Smoke SelfTest could not find production function $FunctionName." }
        . ([scriptblock]::Create($FunctionNode.Extent.Text))
    }
    $SelfChecks = [Collections.Generic.List[string]]::new()
    function Assert-SmokeSelfTest([string]$Name, [bool]$Condition) {
        if (-not $Condition) { throw "Smoke SelfTest failed: $Name" }
        $SelfChecks.Add($Name)
    }
    $Present = 'PVP_SMOKE CLIENT_SHOT_PRESENT ShotId=1 Session=1 Generation=1 Count=1 WeaponNetGuid=7'
    $TraceArgs = @(Get-SmokeTraceArguments 'E:\space path\Client1.utrace' 1)
    Assert-SmokeSelfTest 'Trace has separate native network and CPU categories' ($TraceArgs[0] -eq '-trace=net,cpu,frame,bookmark' -and $TraceArgs[1] -eq '-NetTrace=1')
    Assert-SmokeSelfTest 'Trace filename preserves spaces as one argument' ($TraceArgs[2] -eq '-tracefile="E:\space path\Client1.utrace"')
    $LiveServer = "PVP_SMOKE LIVE_OUTAGE Started ResumeId=12`nPVP_SMOKE LIVE_OUTAGE Restored ResumeId=12"
    $LiveClient = "UNetConnection::Tick: Connection TIMED OUT. Closing connection.`nPVP_SMOKE LIVE_RECONNECT FailureObserved Pid=42 Operation=1 ResumeId=12`nPVP_SMOKE LIVE_RECONNECT RetryStarted Pid=42 Operation=1 Attempt=1`nCLOCK_SAMPLE PASS`nPVP_RESUME CLIENT_READY Restored=1`nPVP_SMOKE LIVE_RECONNECT Restored Pid=42 Operation=1 ResumeId=12 InitialResumeId=12 FailureObserved=1"
    $Live = Get-SmokeLiveReconnectEvidence $LiveServer $LiveClient 42 '12' $LiveClient.IndexOf('PVP_RESUME CLIENT_READY')
    Assert-SmokeSelfTest 'Live retry requires actual timeout, same PID and same participant' ($Live.AutomaticRetryTransport -and $Live.ActualEngineTimeout -and $Live.ClockResampled)
    Assert-SmokeSelfTest 'A restarted process cannot satisfy live recovery' (-not (Get-SmokeLiveReconnectEvidence $LiveServer ($LiveClient.Replace('Restored Pid=42','Restored Pid=43')) 42 '12' 99999).AutomaticRetryTransport)
    Assert-SmokeSelfTest 'Another operation cannot satisfy live recovery' (-not (Get-SmokeLiveReconnectEvidence $LiveServer ($LiveClient.Replace('Restored Pid=42 Operation=1','Restored Pid=42 Operation=2')) 42 '12' 99999).AutomaticRetryTransport)
    Assert-SmokeSelfTest 'Old initial clock does not prove resampling' (-not (Get-SmokeLiveReconnectEvidence $LiveServer ($LiveClient.Replace("`nCLOCK_SAMPLE PASS",'')) 42 '12' 99999).ClockResampled)
    Assert-SmokeSelfTest 'Failure callback alone is not a transport timeout' (-not (Get-SmokeLiveReconnectEvidence $LiveServer ($LiveClient.Replace('Connection TIMED OUT','Synthetic callback')) 42 '12' 99999).AutomaticRetryTransport)
    $OldPerformance='PVP_PERFORMANCE START NetMode=3 Gameplay=0 Render=1'
    Assert-SmokeSelfTest 'Initial-world performance cannot satisfy same-process restoration' ((Get-SmokeRecoveryPerformanceText ($OldPerformance+"`n"+$LiveClient) 'TransportOutage' 42) -notmatch $SmokeConfig.ClientPerformanceStart)
    Assert-SmokeSelfTest 'New-world performance is retained after the actual failure' ((Get-SmokeRecoveryPerformanceText ($LiveClient+"`n"+$OldPerformance) 'TransportOutage' 42) -match $SmokeConfig.ClientPerformanceStart)
    Assert-SmokeSelfTest 'Restarted process keeps its separate fresh performance log' ((Get-SmokeRecoveryPerformanceText $OldPerformance 'ProcessRestart' 42) -eq $OldPerformance)
    $RoleMap=Get-SmokeClientRoleMap @{Client1="PVP_RESUME CLIENT_READY ResumeId=257 Restored=0`nPVP_SMOKE CLIENT_READY Player=257";Client2="PVP_RESUME CLIENT_READY ResumeId=256 Restored=0`nPVP_SMOKE CLIENT_READY Player=256"}
    Assert-SmokeSelfTest 'Reversed asynchronous login maps actual lower-ID active client' ($RoleMap.Valid -and $RoleMap.ActiveClient -eq 'Client2' -and $RoleMap.ObserverClient -eq 'Client1')
    $RoleMap=Get-SmokeClientRoleMap @{Client1="PVP_RESUME CLIENT_READY ResumeId=256 Restored=0`nPVP_SMOKE CLIENT_READY Player=256";Client2="PVP_RESUME CLIENT_READY ResumeId=257 Restored=0`nPVP_SMOKE CLIENT_READY Player=257"}
    Assert-SmokeSelfTest 'Ordinary login mapping keeps original filenames unchanged' ($RoleMap.Valid -and $RoleMap.ActiveClient -eq 'Client1' -and $RoleMap.ObserverClient -eq 'Client2')
    Assert-SmokeSelfTest 'Role mapping cannot borrow another client handshake' (-not (Get-SmokeClientRoleMap @{Client1="PVP_RESUME CLIENT_READY ResumeId=256 Restored=0`nPVP_SMOKE CLIENT_READY Player=257";Client2="PVP_RESUME CLIENT_READY ResumeId=257 Restored=0`nPVP_SMOKE CLIENT_READY Player=256"}).Valid)
    $Ack = 'PVP_SMOKE CLIENT_SHOT_ACK ShotId=1 Session=1 Generation=1 Result=0 Outstanding=0 WeaponNetGuid=7'
    $Accept = 'PVP_SMOKE SERVER_SHOT_ACCEPT ShotId=1 Session=1 Generation=1 Result=0 WeaponNetGuid=7'
    $Client = $Present + "`n" + $Ack
    $Ledger = Get-SmokeShotLedger $Client $Accept -RequireShots
    Assert-SmokeSelfTest 'One predicted/accepted/acknowledged shot' ($Ledger.Complete -and $Ledger.Clean -and $Ledger.Accepted -eq 1)
    $Ledger = Get-SmokeShotLedger ($Present + "`n" + $Ack.Replace('Result=0', 'Result=10')) $Accept.Replace('ACCEPT', 'REJECT').Replace('Result=0', 'Result=10') -RequireShots
    Assert-SmokeSelfTest 'Explicit cancellation is final, not an accepted shot' ($Ledger.Complete -and $Ledger.Clean -and $Ledger.Cancelled -eq 1 -and $Ledger.Accepted -eq 0)
    $LostPayload = "PVP_SMOKE SHOT_INPUT_HOLE_FINAL ShotId=1 Session=1 Generation=1 WeaponNetGuid=7 Result=10 Reason=MissingPayload`n" + $Accept.Replace('ACCEPT','REJECT').Replace('Result=0','Result=10')
    $Ledger = Get-SmokeShotLedger ($Present+"`n"+$Ack.Replace('Result=0','Result=10')) $LostPayload -RequireShots
    Assert-SmokeSelfTest 'Final ACK for missing input closes the ledger but fails strict experience' ($Ledger.Complete -and -not $Ledger.Clean -and $Ledger.LostInputs -eq 1 -and $Ledger.Accepted -eq 0 -and $Ledger.GameplayCancellations -eq 0)
    $ResubmittedClient=$Present+"`nPVP_SMOKE CLIENT_SHOT_SUBMIT ShotId=1 Session=1 Generation=1 WeaponNetGuid=7 Attempt=1 MaxAttempts=6 RetryDelayMs=40 Transport=UnreliableBatch`nPVP_SMOKE CLIENT_SHOT_SUBMIT ShotId=1 Session=1 Generation=1 WeaponNetGuid=7 Attempt=2 MaxAttempts=6 RetryDelayMs=40 Transport=UnreliableBatch`n"+$Ack
    $Ledger=Get-SmokeShotLedger $ResubmittedClient $Accept -RequireShots
    Assert-SmokeSelfTest 'Unreliable batch redundancy is one prediction and one terminal execution' ($Ledger.Complete -and $Ledger.Clean -and $Ledger.Predicted -eq 1 -and $Ledger.Accepted -eq 1)
    foreach ($NonTerminal in @(1, 4)) {
        $Ledger = Get-SmokeShotLedger ($Present + "`n" + $Ack.Replace('Result=0', "Result=$NonTerminal")) $Accept.Replace('ACCEPT', 'REJECT').Replace('Result=0', "Result=$NonTerminal") -RequireShots
        Assert-SmokeSelfTest "Result $NonTerminal alone is not final" (-not $Ledger.Complete -and $Ledger.MissingFinalAck -eq 1)
    }
    $Ledger = Get-SmokeShotLedger $Present $Accept -RequireShots
    Assert-SmokeSelfTest 'Missing owner ACK fails' (-not $Ledger.Complete -and $Ledger.MissingFinalAck -eq 1)
    $Ledger = Get-SmokeShotLedger $Client '' -RequireShots
    Assert-SmokeSelfTest 'Missing server verdict fails' (-not $Ledger.Complete -and $Ledger.ConflictingVerdicts -eq 1)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Ack.Replace('Result=0', 'Result=5')) $Accept -RequireShots
    Assert-SmokeSelfTest 'Conflicting terminal ACKs fail' (-not $Ledger.Complete -and $Ledger.ConflictingVerdicts -eq 1)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Present) $Accept -RequireShots
    Assert-SmokeSelfTest 'Repeated prediction presentation fails' (-not $Ledger.Complete -and $Ledger.DuplicatePresentations -eq 1)
    $Ledger = Get-SmokeShotLedger $Client ($Accept + "`n" + $Accept) -RequireShots
    Assert-SmokeSelfTest 'Repeated server execution fails' (-not $Ledger.Complete -and $Ledger.ConflictingVerdicts -eq 1)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Ack) $Accept -RequireShots
    Assert-SmokeSelfTest 'Repeated identical ACK is idempotent' ($Ledger.Complete -and $Ledger.Accepted -eq 1)
    $Ledger = Get-SmokeShotLedger '' ''
    Assert-SmokeSelfTest 'Empty observer is explicitly not exercised' ($Ledger.Complete -and $Ledger.ExerciseStatus -eq 'NotExercised' -and $Ledger.Predicted -eq 0)
    $Ledger = Get-SmokeShotLedger '' '' -RequireShots
    Assert-SmokeSelfTest 'Empty active client fails' (-not $Ledger.Complete -and $Ledger.MissingRequiredShots)
    $Ledger = Get-SmokeShotLedger $Ack $Accept
    Assert-SmokeSelfTest 'Orphan owner ACK fails' (-not $Ledger.Complete -and $Ledger.OrphanAcknowledgements -eq 1)
    $Ledger = Get-SmokeShotLedger $Client ($Accept + "`n" + $Accept.Replace('WeaponNetGuid=7', 'WeaponNetGuid=8')) -RequireShots
    Assert-SmokeSelfTest 'Another client weapon is not this clients orphan server verdict' ($Ledger.Complete -and $Ledger.Accepted -eq 1)
    $Ledger = Get-SmokeShotLedger $Client ($Accept + "`n" + $Accept.Replace('ShotId=1', 'ShotId=2')) -RequireShots
    Assert-SmokeSelfTest 'An observed owner weapon cannot execute an unpredicted extra shot' (-not $Ledger.Complete -and $Ledger.OrphanServerVerdicts -eq 1)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Client.Replace('WeaponNetGuid=7', 'WeaponNetGuid=8')) ($Accept + "`n" + $Accept.Replace('WeaponNetGuid=7', 'WeaponNetGuid=8')) -RequireShots
    Assert-SmokeSelfTest 'Equal shot numbers on different weapons remain distinct' ($Ledger.Complete -and $Ledger.Accepted -eq 2)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Client.Replace('Generation=1', 'Generation=2')) ($Accept + "`n" + $Accept.Replace('Generation=1', 'Generation=2')) -RequireShots
    Assert-SmokeSelfTest 'Equal shot numbers in different generations remain distinct' ($Ledger.Complete -and $Ledger.Accepted -eq 2)
    $Ledger = Get-SmokeShotLedger ($Client + "`n" + $Client.Replace('Session=1', 'Session=2')) ($Accept + "`n" + $Accept.Replace('Session=1', 'Session=2')) -RequireShots
    Assert-SmokeSelfTest 'Equal shot numbers in different sessions remain distinct' ($Ledger.Complete -and $Ledger.Accepted -eq 2)
    $Ledger = Get-SmokeShotLedger ($Present + "`n" + $Ack.Replace('Outstanding=0', 'Outstanding=1')) $Accept -RequireShots
    Assert-SmokeSelfTest 'Unsettled owner outstanding ledger fails' (-not $Ledger.Complete -and $Ledger.LastOutstanding -eq 1)
    $Ledger = Get-SmokeShotLedger ($Present + "`n" + $Ack.Replace('Outstanding=0 ', '')) $Accept -RequireShots
    Assert-SmokeSelfTest 'Missing outstanding diagnostic fails' (-not $Ledger.Complete -and $Ledger.MissingStableIdentity -gt 0)
    $Ledger = Get-SmokeShotLedger $Client.Replace('WeaponNetGuid=7', 'WeaponNetGuid=0') $Accept -RequireShots
    Assert-SmokeSelfTest 'Zero weapon identity fails' (-not $Ledger.Complete -and $Ledger.MissingStableIdentity -gt 0)
    $Ledger = Get-SmokeShotLedger $Client.Replace('Generation=1', 'Generation=0') $Accept -RequireShots
    Assert-SmokeSelfTest 'Zero equipment generation fails' (-not $Ledger.Complete -and $Ledger.MissingStableIdentity -gt 0)
    $Ledger = Get-SmokeShotLedger ($Client.Replace('Count=1', 'Count=0')) $Accept -RequireShots
    Assert-SmokeSelfTest 'Zero presentation count fails' (-not $Ledger.Complete -and $Ledger.MissingStableIdentity -gt 0)
    foreach ($DamagedNumber in @(
        @{ Name = 'Fractional presentation count fails'; Client = $Client.Replace('Count=1', 'Count=1.5'); Server = $Accept },
        @{ Name = 'Fractional result code fails'; Client = $Client.Replace('Result=0', 'Result=0.5'); Server = $Accept.Replace('Result=0', 'Result=0.5') },
        @{ Name = 'Fractional outstanding count fails'; Client = $Client.Replace('Outstanding=0', 'Outstanding=0.5'); Server = $Accept },
        @{ Name = 'Fractional generation fails'; Client = $Client.Replace('Generation=1', 'Generation=1.5'); Server = $Accept },
        @{ Name = 'Fractional weapon identity fails'; Client = $Client.Replace('WeaponNetGuid=7', 'WeaponNetGuid=7.5'); Server = $Accept },
        @{ Name = 'Out-of-range shot identity fails'; Client = $Client.Replace('ShotId=1', 'ShotId=4294967296'); Server = $Accept },
        @{ Name = 'Out-of-range weapon identity fails'; Client = $Client.Replace('WeaponNetGuid=7', 'WeaponNetGuid=18446744073709551616'); Server = $Accept },
        @{ Name = 'Out-of-range result code fails'; Client = $Client.Replace('Result=0', 'Result=999999999999999999999'); Server = $Accept },
        @{ Name = 'Out-of-range outstanding count fails'; Client = $Client.Replace('Outstanding=0', 'Outstanding=999999999999999999999'); Server = $Accept },
        @{ Name = 'Unknown result code fails'; Client = $Client.Replace('Result=0', 'Result=11'); Server = $Accept.Replace('Result=0', 'Result=11') }
    )) {
        $Ledger = Get-SmokeShotLedger $DamagedNumber.Client $DamagedNumber.Server -RequireShots
        Assert-SmokeSelfTest $DamagedNumber.Name (-not $Ledger.Complete -and $Ledger.MissingStableIdentity -gt 0)
    }
    $QueuedAck = $Ack.Replace('Result=0', 'Result=1').Replace('Outstanding=0', 'Outstanding=1')
    $QueuedServer = $Accept.Replace('ACCEPT', 'REJECT').Replace('Result=0', 'Result=1')
    $DuplicateAck = $Ack.Replace('Result=0', 'Result=4').Replace('Outstanding=0', 'Outstanding=1')
    $DuplicateServer = $Accept.Replace('ACCEPT', 'REJECT').Replace('Result=0', 'Result=4')
    $Ledger = Get-SmokeShotLedger ($Present + "`n" + $QueuedAck + "`n" + $DuplicateAck + "`n" + $Ack) ($QueuedServer + "`n" + $DuplicateServer + "`n" + $Accept) -RequireShots
    Assert-SmokeSelfTest 'Queued/duplicate diagnostics do not hide the later real terminal ACK' ($Ledger.Complete -and $Ledger.Accepted -eq 1)
    $Ledger = Get-SmokeShotLedger ($Present + "`n" + $Ack.Replace('Result=0', 'Result=5')) $Accept.Replace('ACCEPT', 'REJECT').Replace('Result=0', 'Result=5') -RequireShots
    Assert-SmokeSelfTest 'Time rejection is accounted but not clean acceptance' ($Ledger.Complete -and -not $Ledger.Clean -and $Ledger.UnexpectedRejections -eq 1)
    $Moving = Get-SmokeMovingHitEvidence $false '' @()
    Assert-SmokeSelfTest 'Unrequested moving-hit is not a successful test' ($Moving.Status -eq 'NotRequested' -and $null -eq $Moving.Passed)
    $MovingIdentity='ShotId=1 Session=2 Generation=3 WeaponNetGuid=7 TargetNetGuid=9'
    $MovingRewind='PVP_SMOKE REWIND '+$MovingIdentity+' TargetTime=10.123 TimeSource=NativeView'
    $MovingServer='PVP_SMOKE MOVING_HIT_SERVER PASS '+$MovingIdentity+' HealthBefore=100 HealthAfter=90 TargetSpeed=600'+"`n"+$MovingRewind
    $MovingClient='PVP_SMOKE MOVING_HIT_CLIENT PASS '+$MovingIdentity+' HealthBefore=100 HealthAfter=90'
    $MovingAim='PVP_SMOKE MOVING_HIT_AIM '+$MovingIdentity+' TargetSpeed=600 TargetViewTime=10.123 ViewAgeMs=80 NativeView=1'
    $Moving = Get-SmokeMovingHitEvidence $true $MovingServer @('')
    Assert-SmokeSelfTest 'Moving-hit needs both server and client evidence' (-not $Moving.Passed -and $Moving.Status -eq 'MissingEvidence')
    $Moving = Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim+"`n"+$MovingClient)
    Assert-SmokeSelfTest 'Requested moving-hit uses paired evidence' ($Moving.Passed -and $Moving.Status -eq 'Passed')
    Assert-SmokeSelfTest 'Moving-hit cannot join different targets' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim+"`n"+$MovingClient.Replace('TargetNetGuid=9','TargetNetGuid=10'))).Passed)
    Assert-SmokeSelfTest 'Moving-hit cannot join different shots' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim+"`n"+$MovingClient.Replace('ShotId=1','ShotId=2'))).Passed)
    Assert-SmokeSelfTest 'Moving-hit rejects zero target identity' (-not (Get-SmokeMovingHitEvidence $true $MovingServer.Replace('TargetNetGuid=9','TargetNetGuid=0') @($MovingAim+"`n"+$MovingClient)).Passed)
    Assert-SmokeSelfTest 'Moving-hit requires real client-observed health reduction' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim+"`n"+$MovingClient.Replace('HealthAfter=90','HealthAfter=100'))).Passed)
    Assert-SmokeSelfTest 'Moving-hit requires the same local aimed input' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingClient)).Passed)
    Assert-SmokeSelfTest 'Moving-hit cannot borrow aimed input from another physical client' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim,$MovingClient)).Passed)
    $NativeOrderedServer='PVP_SMOKE MOVING_HIT_SERVER PASS ShotId=1 TargetSpeed=600 HealthBefore=100 HealthAfter=90 WeaponNetGuid=7 PlayerId=256 Generation=3 Session=2 TargetNetGuid=9'+"`n"+$MovingRewind
    Assert-SmokeSelfTest 'Native marker field order does not change moving-shot identity' (Get-SmokeMovingHitEvidence $true $NativeOrderedServer @($MovingAim+"`n"+$MovingClient)).Passed
    $MissingNative=Get-SmokeMovingHitEvidence $true ($MovingServer.Replace(' TimeSource=NativeView','')) @($MovingAim+"`n"+$MovingClient)
    Assert-SmokeSelfTest 'A hit without actual NativeView metadata stays NotVerified' (-not $MissingNative.Passed -and $MissingNative.Status -eq 'NotVerified')
    Assert-SmokeSelfTest 'Fallback time cannot certify NativeView alignment' (-not (Get-SmokeMovingHitEvidence $true ($MovingServer.Replace('TimeSource=NativeView','TimeSource=Fallback')) @($MovingAim+"`n"+$MovingClient)).Passed)
    Assert-SmokeSelfTest 'Client fallback cannot certify NativeView alignment' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim.Replace('NativeView=1','NativeView=0')+"`n"+$MovingClient)).Passed)
    Assert-SmokeSelfTest 'Target view times within 1.1ms are verified' (Get-SmokeMovingHitEvidence $true $MovingServer.Replace('TargetTime=10.123','TargetTime=10.1241') @($MovingAim+"`n"+$MovingClient)).Passed
    Assert-SmokeSelfTest 'Target view times outside 1.1ms are not verified' (-not (Get-SmokeMovingHitEvidence $true $MovingServer.Replace('TargetTime=10.123','TargetTime=10.1242') @($MovingAim+"`n"+$MovingClient)).Passed)
    Assert-SmokeSelfTest 'Native timestamp NaN stays unavailable' (-not (Get-SmokeMovingHitEvidence $true $MovingServer @($MovingAim.Replace('TargetViewTime=10.123','TargetViewTime=nan')+"`n"+$MovingClient)).Passed)
    Assert-SmokeSelfTest 'Unrequested moving-hit is not in the required gate' ('Deep.MovingHit' -notin @(Get-SmokeRequiredDeepEvidence $false))
    Assert-SmokeSelfTest 'Requested moving-hit is in the required gate' ('Deep.MovingHit' -in @(Get-SmokeRequiredDeepEvidence $true))
    Assert-SmokeScenarioOptions $true $true $false $false $false $true
    Assert-SmokeSelfTest 'Default scenario options are valid' $true
    foreach ($InvalidOptions in @(
        @{ Name = 'Recovery requires offline PvE damage'; Values = @($false, $true, $false, $false, $false, $true) },
        @{ Name = 'Moving-hit requires deep checks'; Values = @($true, $false, $false, $false, $true, $false) },
        @{ Name = 'Visible clients require rendered clients'; Values = @($true, $false, $false, $true, $false, $true) },
        @{ Name = 'Rendered scenario requires PvE'; Values = @($false, $false, $true, $false, $false, $true) }
    )) {
        $Rejected = $false
        $InvalidValues = $InvalidOptions.Values
        try { Assert-SmokeScenarioOptions @InvalidValues } catch { $Rejected = $true }
        Assert-SmokeSelfTest $InvalidOptions.Name $Rejected
    }
    foreach ($FailureLog in @(('PVP_SMOKE PASS=Final' + "`n" + 'Fatal error: late crash'),
        'PVP_VISUAL FAIL', 'Assertion failed: invariant', 'Unhandled Exception: access violation',
        'Blueprint Runtime Error', 'LogScript: Warning: Accessed None', 'OOM-killer', 'Memory cgroup out of memory',
        'Out of memory: Killed process', 'Ran out of memory allocating')) {
        Assert-SmokeSelfTest ('Failure marker: ' + ($FailureLog -replace '[\r\n]+', ' ')) (Test-SmokeFailureLog $FailureLog)
    }
    Assert-SmokeSelfTest 'Ordinary PASS is not a failure' (-not (Test-SmokeFailureLog 'PVP_SMOKE PASS=Final'))
    $PolicyServer = "PVP_SMOKE SERVER_SHOT_ACCEPT ShotId=1 Session=1 Generation=1 Result=0 Ammo=29 WeaponNetGuid=7`n" +
        "PVP_SMOKE SHOT_COMMIT ShotId=1 Session=1 Generation=1 WeaponNetGuid=7 AmmoBefore=30 AmmoAfter=29`n" +
        "PVP_SMOKE SERVER_SHOT_REJECT ShotId=2 Session=1 Generation=1 Result=5 Ammo=29 WeaponNetGuid=7`n" +
        "PVP_SMOKE SHOT_TIME_REJECT ShotId=2 Session=1 Generation=1 WeaponNetGuid=7 Stage=Admission Reason=Stale AgeMs=250.0 AllowedAgeMs=220.0`n" +
        'PVP_SMOKE SHOT_REJECT_BOUNDARY ShotId=2 Session=1 Generation=1 WeaponNetGuid=7 AmmoBefore=29 AmmoAfter=29 ReserveBefore=90 ReserveAfter=90 ExecutedBefore=1 ExecutedAfter=1'
    $PolicyClient = "PVP_SMOKE CLIENT_SHOT_ACK ShotId=2 Session=1 Generation=1 Result=5 Ammo=29 Outstanding=0 WeaponNetGuid=7`n" +
        "PVP_SMOKE CLIENT_PVE_COMPLETE PASS`nPVP_SMOKE WEAK_POLICY_SETTLED Ammo=29 Reserve=90 Outstanding=0 Firing=0 Reloading=0 Aiming=0 Sprinting=0"
    Assert-SmokeSelfTest 'Measured stale rejection with unchanged state is policy-correct' (Get-SmokeWeakPolicyEvidence $PolicyServer @($PolicyClient)).Passed
    Assert-SmokeSelfTest 'Execution stale is checked against its measured execution budget' (Get-SmokeWeakPolicyEvidence $PolicyServer.Replace('Stage=Admission','Stage=Execution') @($PolicyClient)).Passed
    foreach ($BadServer in @($PolicyServer.Replace('Reason=Stale','Reason=Future'),$PolicyServer.Replace('AmmoAfter=29 ReserveBefore','AmmoAfter=28 ReserveBefore'),
        $PolicyServer.Replace('AgeMs=250.0','AgeMs=200.0'),$PolicyServer.Replace('Result=5','Result=7'),$PolicyServer.Replace('AmmoBefore=30 AmmoAfter=29','AmmoBefore=30 AmmoAfter=28'))) {
        Assert-SmokeSelfTest 'Policy rejects future/within-budget/rate/mutation/extra-consumption violations' (-not (Get-SmokeWeakPolicyEvidence $BadServer @($PolicyClient)).Passed)
    }
    Assert-SmokeSelfTest 'Policy requires actual stale exercise' (-not (Get-SmokeWeakPolicyEvidence '' @($PolicyClient)).Passed)
    $InjectedPolicyClient=$PolicyClient+"`nPVP_SMOKE POLICY_STALE_INPUT ShotId=2 Session=1 Generation=1 WeaponNetGuid=7 OffsetSeconds=1.0`nPVP_SMOKE CLIENT_AMMO_PREDICT ShotId=2 Session=1 Generation=1 WeaponNetGuid=7 BaseAmmo=29 DisplayAmmo=28 Outstanding=1 IncludedShotId=1`nPVP_SMOKE CLIENT_AMMO_RECONCILE ShotId=2 Session=1 Generation=1 WeaponNetGuid=7 Result=5 BaseAmmo=29 DisplayAmmo=28 Outstanding=1 IncludedShotId=1 StateRevision=2 PendingUnincludedInputs=1 RejectedStillPending=0"
    Assert-SmokeSelfTest 'Deterministic stale input refunds itself and retains a later pending deduction' (Get-SmokeWeakPolicyEvidence $PolicyServer @($InjectedPolicyClient) $true).Passed
    Assert-SmokeSelfTest 'Policy cannot replace deterministic injection with natural packet loss' (-not (Get-SmokeWeakPolicyEvidence $PolicyServer @($PolicyClient) $true).Passed)
    Assert-SmokeSelfTest 'A rejected input remaining pending fails refund acceptance' (-not (Get-SmokeWeakPolicyEvidence $PolicyServer @($InjectedPolicyClient.Replace('RejectedStillPending=0','RejectedStillPending=1')) $true).Passed)
    Assert-SmokeSelfTest 'Overwriting subsequent predictions with authority ammo fails refund acceptance' (-not (Get-SmokeWeakPolicyEvidence $PolicyServer @($InjectedPolicyClient.Replace('Result=5 BaseAmmo=29 DisplayAmmo=28','Result=5 BaseAmmo=29 DisplayAmmo=29')) $true).Passed)
    $LatencySelfTest = & (Join-Path $PSScriptRoot 'MeasureFpsShotLatency.ps1') -SelfTest | ConvertFrom-Json
    Assert-SmokeSelfTest 'Shot latency parser preserves missing/phase/retry evidence' ([bool]$LatencySelfTest.Passed)
    [pscustomobject]@{ Mode = 'SelfTest'; Passed = $true; Checks = $SelfChecks.Count; Scenarios = @($SelfChecks)
        ShotLatencyParserChecks = $LatencySelfTest.Checks
        Scope = 'Offline production-parser/options/evidence regression only; no UE, game process, network or disk output. Not gameplay/network acceptance.' } | ConvertTo-Json -Depth 4
    return
}
Assert-SmokeScenarioOptions $IncludePve $IncludeReconnect ([bool]$RenderedClients) ([bool]$VisibleClients) ([bool]$MovingHit) $DeepChecks
if ($ReconnectMode -eq 'TransportOutage' -and -not $IncludeReconnect) { throw 'TransportOutage requires IncludeReconnect.' }
if ($WeakPolicyCorrectness -and (-not $DeepChecks -or -not $IncludePve -or -not $IncludeReconnect)) {
    throw 'WeakPolicyCorrectness requires all deep/PvE/reconnect checks; it is separate from strict zero-unexpected-rejection experience acceptance.'
}
if (-not $ProjectRoot) { $ProjectRoot = Join-Path $PSScriptRoot '..\..' }
# Keep the scenario contract in one place. C++ emits these only after checking
# actual gameplay/replication results, not merely after issuing an action.
$SmokeConfig = @{
    GameMode = '/Script/fpstrue.fpstruePvpGameMode'
    ClientCount = 2
    ServerMarkers = @('Spawn', 'Hit', 'Reload', 'AutoReload', 'Aim', 'Sprint', 'Death', 'Respawn', 'Final')
    PveServerMarkers = @('PveSpawn', 'PveEnemyHit', 'PvePlayerHit', 'PveEnemyDeath')
    ClientReady = 'PVP_SMOKE.*CLIENT_READY'
    ClientState = 'PVP_SMOKE.*CLIENT_STATE PASS'
    ClientMovement = 'PVP_SMOKE.*CLIENT_MOVEMENT PASS'
    ClientComplete = 'PVP_SMOKE.*CLIENT_COMPLETE PASS'
    ClientRemoteSprint = 'PVP_SMOKE.*CLIENT_REMOTE_SPRINT PASS'
    ClientRemoteAim = 'PVP_SMOKE.*CLIENT_REMOTE_AIM PASS'
    ServerPerformanceStart = 'PVP_PERFORMANCE\s+START\s+NetMode=1\s+Gameplay=1\s+Render=0\b'
    ServerPerformancePopulated = 'PVP_PERFORMANCE\s+POPULATED\s+NetMode=1\s+Enemies=[1-9]\d*\s+GameplayViews=[1-9]\d*\s+Render=0\b'
    ServerForbiddenRender = 'PVP_PERFORMANCE\s+(?:RENDER_APPLIED|SHARING_ACTIVE)\b'
    ClientPerformanceStart = 'PVP_PERFORMANCE\s+START\s+NetMode=3\s+Gameplay=0\s+Render=1\b'
    ClientPerformanceApplied = 'PVP_PERFORMANCE\s+RENDER_APPLIED\s+NetMode=3\s+Enemies=[1-9]\d*\s+GameplayViews=0\b'
    ClientSharingActive = 'PVP_PERFORMANCE\s+SHARING_ACTIVE\s+NetMode=3\s+Followers=[1-9]\d*\b'
    Failure = '(?im)PVP_(?:SMOKE|VISUAL)[^\r\n]*\bFAIL\b|Fatal error:|Assertion failed:|Unhandled Exception:|LogScript: Warning: Accessed None|Blueprint Runtime Error'
    ExplicitOom = '(?im)\bOOM[- ]kill(?:er)?\b|\bMemory cgroup out of memory\b|\bOut of memory:\s*Killed process\b|\bRan out of memory allocating\b'
    PollMilliseconds = 250
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$ProjectPath = Join-Path $ProjectRoot 'fpstrue.uproject'
if (-not $LocalCacheDirectory) { $LocalCacheDirectory = Join-Path $ProjectRoot 'Saved\LocalDDC' }
$LocalCacheDirectory = [IO.Path]::GetFullPath($LocalCacheDirectory)
$EditorArguments = @()
$PackageReceipts = @{}
$IsRemote = $ServerMode -eq 'RemoteLinux'
if ($IsRemote) {
    # All values crossing the remote shell have a closed alphabet. Never accept
    # shell fragments, traversal, a root run user, or an unverified host/key.
    if ($ServerAddress -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$' -or $ServerAddress.Contains('..')) {
        throw 'RemoteLinux requires a plain ServerAddress hostname/IPv4 without a port, options or shell syntax.'
    }
    if ($SshTarget -notmatch '^(?:[a-z_][a-z0-9_-]{0,31}@)?[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$' -or $SshTarget.Contains('..')) {
        throw 'SshTarget must be a plain SSH alias or user@hostname/IPv4.'
    }
    if ($RemoteRunUser -notmatch '^[a-z_][a-z0-9_-]{0,31}$' -or $RemoteRunUser -eq 'root') {
        throw 'RemoteRunUser must name an existing ordinary Linux account, not root.'
    }
    if ($RemotePackageRoot -notmatch '^/(?:[A-Za-z0-9_-][A-Za-z0-9._-]*/)+[A-Za-z0-9_-][A-Za-z0-9._-]*$' -or
        @($RemotePackageRoot.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
        throw 'RemotePackageRoot must be an explicit absolute package subdirectory without spaces or traversal.'
    }
    if ($RemoteServerSha256 -notmatch '^[A-Fa-f0-9]{64}$') {
        throw 'RemoteLinux requires the expected SHA-256 of the exact deployed Server ELF.'
    }
    foreach ($CredentialFile in @($SshKeyPath, $SshKnownHostsPath)) {
        if (-not $CredentialFile -or -not (Test-Path -LiteralPath $CredentialFile -PathType Leaf)) {
            throw 'RemoteLinux requires existing SshKeyPath and previously verified SshKnownHostsPath files. Private-key contents are never read by this script.'
        }
    }
    $SshKeyPath = (Resolve-Path -LiteralPath $SshKeyPath).Path
    $SshKnownHostsPath = (Resolve-Path -LiteralPath $SshKnownHostsPath).Path
    $SshPath = (Get-Command ssh.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $SshArguments = @('-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=yes',
        '-o', ('UserKnownHostsFile=' + $SshKnownHostsPath), '-o', 'ConnectTimeout=8',
        '-o', 'ConnectionAttempts=1', '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2', '-i', $SshKeyPath)
    $RemoteExecutable = $RemotePackageRoot + '/fpstrue/Binaries/Linux/fpstrueServer'
    $RemoteReceipt = $RemotePackageRoot + '/fpstrue/Binaries/Linux/fpstrueServer.target'
}
elseif ($ServerAddress -or $SshTarget -or $SshKeyPath -or $SshKnownHostsPath -or $RemotePackageRoot -or $RemoteServerSha256) {
    throw 'Remote connection parameters require -ServerMode RemoteLinux.'
}
if ($ExecutionMode -eq 'Editor') {
    if ($ServerExecutable -or $ClientExecutable) {
        throw 'ServerExecutable and ClientExecutable require -ExecutionMode Packaged; Editor mode never launches packaged files.'
    }
    $EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
    $EditorPath = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    foreach ($RequiredFile in @($EditorPath, $ProjectPath)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            throw "Required file is missing: $RequiredFile"
        }
    }
    $ServerPath = $EditorPath
    $ClientPath = $EditorPath
    # The project test graph retains both engine-pak formats and a writable
    # command-line-selected local cache, without changing registry settings.
    $EditorCacheGraph = 'fpstrueEditorTestDDC'
    $EditorArguments = @(
        ('"' + $ProjectPath + '"'), '-NoLiveCoding', ('-ddc=' + $EditorCacheGraph),
        '-ini:Game:[StartupActions]:bAddPacks=False',
        ('-LocalDataCachePath="' + $LocalCacheDirectory + '"')
    )
}
else {
    if (-not $ClientExecutable -or (-not $IsRemote -and -not $ServerExecutable)) {
        throw 'Packaged mode requires a Development ClientExecutable and, for Local mode, a Development ServerExecutable.'
    }
    if ($IsRemote -and $ServerExecutable) { throw 'RemoteLinux selects its verified Server ELF from RemotePackageRoot; do not also supply ServerExecutable.' }
    $LocalExecutables = @($ClientExecutable)
    if (-not $IsRemote) { $LocalExecutables += $ServerExecutable }
    foreach ($Executable in $LocalExecutables) {
        if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
            throw "Packaged executable is missing: $Executable"
        }
    }
    if (-not $IsRemote) { $ServerPath = (Resolve-Path -LiteralPath $ServerExecutable).Path }
    $ClientPath = (Resolve-Path -LiteralPath $ClientExecutable).Path
    # Bootstrap launchers spawn another process, which would escape our strict
    # owned-PID cleanup. Direct target binaries keep lifetime and identity clear.
    if ((-not $IsRemote -and $ServerPath -notmatch '[\\/]Binaries[\\/]Win64[\\/]fpstrueServer(?:-Win64-Development)?\.exe$') -or
        $ClientPath -notmatch '[\\/]Binaries[\\/]Win64[\\/]fpstrue(?:-Win64-Development)?\.exe$') {
        throw 'Use the direct fpstrueServer.exe and fpstrue.exe (or -Win64-Development variants) inside each package Binaries\Win64 directory. Editor, Shipping and bootstrap executables are not accepted.'
    }
    $LocalTargets = @(@{ Role = 'Client'; Path = $ClientPath; Name = 'fpstrue'; Type = 'Game' })
    if (-not $IsRemote) { $LocalTargets += @{ Role = 'Server'; Path = $ServerPath; Name = 'fpstrueServer'; Type = 'Server' } }
    foreach ($Target in $LocalTargets) {
        $ExecutableName = [IO.Path]::GetFileName($Target.Path)
        $ReceiptPath = Join-Path (Split-Path -Parent $Target.Path) ([IO.Path]::GetFileNameWithoutExtension($Target.Path) + '.target')
        if (-not (Test-Path -LiteralPath $ReceiptPath -PathType Leaf)) {
            $ReceiptPath = Join-Path (Split-Path -Parent $Target.Path) ($Target.Name + '.target')
        }
        if (-not (Test-Path -LiteralPath $ReceiptPath -PathType Leaf)) {
            throw "$($Target.Role) package requires its genuine Development build receipt adjacent to the executable: $ReceiptPath"
        }
        $Receipt = Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json
        # Receipts contain source-tree macros/paths, not archive paths. Match the
        # target and recorded executable relative suffix after staging relocation.
        $ExecutableProducts = @($Receipt.BuildProducts | Where-Object {
            $_.Type -eq 'Executable' -and ([string]$_.Path).Replace('\', '/') -like ('*/Binaries/Win64/' + $ExecutableName)
        })
        if ($Receipt.TargetName -ne $Target.Name -or $Receipt.Configuration -ne 'Development' -or
            $Receipt.Platform -ne 'Win64' -or $ExecutableProducts.Count -ne 1 -or
            ($Receipt.TargetType -and $Receipt.TargetType -ne $Target.Type)) {
            throw "$($Target.Role) build receipt does not describe the expected Win64 Development $($Target.Type) executable: $ReceiptPath"
        }
        $PackageReceipts[$Target.Role] = [pscustomobject]@{
            Path = $ReceiptPath; TargetName = $Receipt.TargetName
            TargetType = $Receipt.TargetType; Configuration = $Receipt.Configuration
            Executable = $Target.Path; Sha256 = (Get-FileHash -LiteralPath $Target.Path -Algorithm SHA256).Hash
            ReceiptSha256 = (Get-FileHash -LiteralPath $ReceiptPath -Algorithm SHA256).Hash
        }
    }
}
if ($Map -notmatch '^/Game/[A-Za-z0-9_/]+(?:\.[A-Za-z0-9_]+)?$') {
    throw 'Map must be a /Game package path without travel options or console commands.'
}
if ($PacketLagVarianceMs -gt 0 -and $PacketLagMs -eq 0) {
    throw 'PacketLagVarianceMs requires a nonzero PacketLagMs.'
}

$RunId = (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$OutputDirectory = Join-Path $ProjectRoot ('Saved\Networking\PvpSmoke_' + $RunId)
$SharedArguments = @(
    '-Unattended', '-NullRHI', '-NoSound', '-NoSplash', '-NoSteam', '-PvpSmokeTest',
    # UE 5.5 NetDriver::InitPacketSimulationSettings parses these command-line
    # settings directly. Lag is per outgoing direction, not a claimed RTT.
    ('-PktLag=' + $PacketLagMs), ('-PktLagVariance=' + $PacketLagVarianceMs),
    ('-PktLoss=' + $PacketLossPercent)
)
if ($IncludePve) { $SharedArguments += '-PvpSmokePve' }
if ($IncludeReconnect) { $SharedArguments += '-PvpSmokeReconnect' }
if ($IncludeReconnect -and $ReconnectMode -eq 'TransportOutage') { $SharedArguments += '-PvpSmokeLiveReconnect' }
if ($WeakPolicyCorrectness) { $SharedArguments += '-PvpWeakPolicyCorrectness' }
if ($MovingHit) { $SharedArguments += '-PvpSmokeMovingHit' }
$ClientSharedArguments = @($SharedArguments)
if ($RenderedClients) {
    # This removes NullRHI only from clients. RenderOffscreen still renders the
    # real game viewport/Slate without capturing or manipulating the desktop.
    $ClientSharedArguments = @($SharedArguments | Where-Object { $_ -ne '-NullRHI' }) + @(
        '-dx12', '-Windowed', '-ResX=1280', '-ResY=720', '-ForceRes', '-PvpSmokeVisual'
    )
    if (-not $VisibleClients) { $ClientSharedArguments += '-RenderOffscreen' }
}
$ServerLog = Join-Path $OutputDirectory 'Server.log'
$ServerArguments = $EditorArguments + @(
    ('"' + $Map + '?game=' + $SmokeConfig.GameMode + '"'),
    ('-port=' + $Port), ('-abslog="' + $ServerLog + '"')
) + $SharedArguments
if ($ExecutionMode -eq 'Editor') { $ServerArguments += '-server' }
# UE5.5 disables performance threading on dedicated servers unless explicitly opted in.
# Keep this on the server only; reference baselines preserve the old launch separately.
$ServerArguments += '-useperfthreads'
if ($IsRemote) {
    $ServerPath = $RemoteExecutable
    $RemoteRunDirectory = '/var/tmp/fpstrue-smoke-' + $RunId
    $RemoteLog = $RemoteRunDirectory + '/Server.log'
    $RemoteUnit = 'fpstrue-pvp-smoke-' + $RunId + '.service'
    $ServerArguments = @(
        ($Map + '?game=' + $SmokeConfig.GameMode), ('-port=' + $Port), ('-abslog=' + $RemoteLog),
        ('-UserDir=' + $RemoteRunDirectory + '/User'), '-useperfthreads'
    ) + $SharedArguments
}
$ServerArguments += '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:bEnableTargetLatencyCompensation=' + $(if($TargetLatencyCompensation -eq 'Enabled'){'True'}else{'False'})
if ($EnableNetTrace -and $IsRemote) { throw 'Native trace collection currently requires Local mode; a remote trace must be explicitly collected and validated separately.' }
$TracePaths = @{}
if ($EnableNetTrace) {
    $TracePaths.Server = Join-Path $OutputDirectory 'Trace\Server.utrace'
    $ServerArguments += Get-SmokeTraceArguments $TracePaths.Server $NetTraceVerbosity
}
function Get-SmokeBuildFingerprint {
    $MapFile = Join-Path $ProjectRoot ('Content/' + $Map.Substring(6) + '.umap')
    $Paths = @($ProjectPath, $MapFile, (Join-Path $ProjectRoot 'Binaries\Win64\UnrealEditor-fpstrue.dll')) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
    $Paths += @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'Source'), (Join-Path $ProjectRoot 'Config') -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.cpp', '.cs', '.ini') } | Select-Object -ExpandProperty FullName)
    return @($Paths | Sort-Object -Unique | ForEach-Object {
        [pscustomobject]@{ Path = $_; Sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    })
}
$ConnectAddress = if ($IsRemote) { $ServerAddress } else { '127.0.0.1' }
$ClientCommands = @()
for ($ClientIndex = 1; $ClientIndex -le $SmokeConfig.ClientCount; $ClientIndex++) {
    $ClientLog = Join-Path $OutputDirectory ('Client' + $ClientIndex + '.log')
    $ClientCommands += [pscustomobject]@{
        Name = 'Client' + $ClientIndex
        Log = $ClientLog
        UserDirectory = Join-Path $OutputDirectory ('PrivateUsers\Client' + $ClientIndex)
        VisualDirectory = if ($RenderedClients) { Join-Path $OutputDirectory ('Visual\Client' + $ClientIndex) } else { $null }
        Arguments = $EditorArguments + @(
            ($ConnectAddress + ':' + $Port),
            ('-abslog="' + $ClientLog + '"'),
            ('-UserDir="' + (Join-Path $OutputDirectory ('PrivateUsers\Client' + $ClientIndex)) + '"'),
            ('-PvpSmokeClient=' + $ClientIndex)
        ) + $ClientSharedArguments
    }
    if ($RenderedClients) {
        $ClientCommands[-1].Arguments += '-PvpSmokeCaptureDir="' + $ClientCommands[-1].VisualDirectory + '"'
    }
    if ($VisibleClients) {
        $ClientCommands[-1].Arguments += if ($ClientIndex -eq 1) { @('-WinX=30', '-WinY=60') } else { @('-WinX=650', '-WinY=160') }
    }
    if ($ExecutionMode -eq 'Editor') { $ClientCommands[-1].Arguments += '-game' }
    if ($EnableNetTrace) {
        $TracePaths[$ClientCommands[-1].Name] = Join-Path $OutputDirectory ('Trace\' + $ClientCommands[-1].Name + '.utrace')
        $ClientCommands[-1].Arguments += Get-SmokeTraceArguments $TracePaths[$ClientCommands[-1].Name] $NetTraceVerbosity
    }
}

if ($DryRun) {
    [pscustomobject]@{
        ExecutionMode = $ExecutionMode
        ServerMode = $ServerMode
        ServerExecutable = $ServerPath
        ClientExecutable = $ClientPath
        ExpectedConfiguration = 'Development'
        PackageReceipts = $PackageReceipts
        Remote = if ($IsRemote) { @{
            SshTarget = $SshTarget; PackageRoot = $RemotePackageRoot; ExpectedServerSha256 = $RemoteServerSha256
            Receipt = $RemoteReceipt; RunUser = $RemoteRunUser; Unit = $RemoteUnit
            RuntimeLimitSeconds = $TimeoutSeconds + 30; MemoryMax = '900M'; StrictHostKeyChecking = 'yes'
            Preflight = 'Existing ordinary user, free UDP port, Linux Development Server receipt, ELF/hash and staged content; no SSH is executed during DryRun'
        } } else { $null }
        Server = $ServerArguments
        Clients = $ClientCommands
        TimeoutSeconds = $TimeoutSeconds
        ServerRequired = $SmokeConfig.ServerMarkers
        ClientRequired = 'Both READY; one same client STATE PASS, MOVEMENT PASS and COMPLETE PASS; one same observer client REMOTE_SPRINT PASS and REMOTE_AIM PASS'
        OutputDirectory = $OutputDirectory
        DeepChecks = $DeepChecks
        IncludePve = $IncludePve
        IncludeReconnect = $IncludeReconnect
        ReconnectMode = $ReconnectMode
        Trace = @{ Enabled = [bool]$EnableNetTrace; Paths = $TracePaths; MaxTotalMiB = $TraceMaxMiB; DiagnosticOnly = $true }
        RenderedClients = [bool]$RenderedClients
        VisibleClients = [bool]$VisibleClients
        MovingHit = [bool]$MovingHit
        TargetLatencyCompensationRequested = $TargetLatencyCompensation
        MovingHitStatus = if ($MovingHit) { 'Requested' } else { 'NotRequested' }
        MemoryRequired = if ($DeepChecks) { 'Server: at least two distinct player Pawns with populated history; every diagnostic row must have at most 32*64 reserved shapes, shape stride at most 96 bytes, history heap capacity at most 32*64*stride, and no first-person main AnimInstance or component tick. Every original/restored client needs a PVP_MEMORY row and zero history capacity. Optional post-process instances are reported, not required to be absent. This is shape-buffer heap accounting, not process RAM or a memory high-water measurement.' } else { 'Not gated without DeepChecks.' }
        VisualRequired = if ($RenderedClients) { 'Both initial clients and, when requested, the restored client must emit matching PVP_VISUAL_READY/CAPTURE markers, then produce actual 1280x720 PNGs in their own capture directories. PNG dimensions/hash are checked; human visual review is separate.' } else { $null }
        PveRequired = if ($IncludePve) { $SmokeConfig.PveServerMarkers + @('Both CLIENT_PVE_REPLICATED PASS; one CLIENT_PVE_COMPLETE PASS') } else { @() }
        PerformanceIntegrationRequired = if ($IncludePve) { 'Dedicated server must start/populate Gameplay-only scheduling; each original and restored client must start/apply its own populated presentation budget without server Gameplay viewpoints. Dedicated server must never apply render/sharing work. Sharing follower markers are optional diagnostics, not a one-enemy smoke gate. NullRHI application markers do not prove rendered frames.' } else { $null }
        ReconnectRequired = if ($IncludeReconnect -and $ReconnectMode -eq 'TransportOutage') { 'Both original client processes remain alive. Map server LIVE_OUTAGE ResumeId to the actual owned client, independent of filename or login order; require real timeout, automatic retry, retention/offline damage, identity restoration and a new rendered capture when requested.' } elseif ($IncludeReconnect) { 'After original PvP/PvE gates, hard-stop the mapped observer client, retain/offline-damage and restart the same private UserDir; correlate server/client identities. Keep original filenames and store role mapping explicitly.' } else { $null }
        Scope = if ($IsRemote) { 'Cross-machine functional smoke plan; no remote process, SSH or output directory was created' } elseif ($RenderedClients) { 'Local rendered-client functional/capture plan; no process or output directory was created; Editor mode is not packaged acceptance' } else { 'NullRHI local functional network smoke only; Editor mode does not validate independently packaged executables' }
    } | ConvertTo-Json -Depth 6
    return
}

# Do not take a port from another project or stop an existing UE process.
if (-not $IsRemote) {
    $UdpListeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveUdpListeners()
    if (@($UdpListeners | Where-Object { $_.Port -eq $Port }).Count -gt 0) {
        throw "UDP port $Port is already in use. Choose another Port or close its owner yourself."
    }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
if ($EnableNetTrace) { $null = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'Trace') }
# Resume tickets are credentials. Each owned client gets a fresh directory that
# is accessible only to the current Windows identity. Never read, print, copy
# into reports, or pass the ticket itself through a URL or command line.
foreach ($ClientCommand in $ClientCommands) {
    $null = New-Item -ItemType Directory -Path $ClientCommand.UserDirectory
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $Acl = [Security.AccessControl.DirectorySecurity]::new()
    $Acl.SetOwner($Identity)
    $Acl.SetAccessRuleProtection($true, $false)
    $Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($Identity,
        [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $ClientCommand.UserDirectory -AclObject $Acl
    if ($RenderedClients) { $null = New-Item -ItemType Directory -Path $ClientCommand.VisualDirectory -Force }
}
$Started = [Collections.Generic.List[object]]::new()
$RunClock = [Diagnostics.Stopwatch]::StartNew()
$Succeeded = $false
$FailureReason = $null
$Evidence = @{}
$RemoteOwned = $false
$RemoteActiveState = 'not-started'
$LastRemotePoll = -10.0
$ReconnectStage = 'NotStarted'
$ReconnectCommand = $null
$ReconnectResumeId = $null
$ReconnectBaseline = $null
$ReconnectOfflineDamageIndex = -1
$ReconnectStoppedUtc = $null
$VisualCaptures = @{}
$ShotLedgers = @{}
$MemoryAccounting = $null
$OriginalReconnectClientPid = $null
$ReconnectClientName = $null
$ClientRoleMap = $null
$InputFingerprint = @(Get-SmokeBuildFingerprint)
$InputFingerprintJson = $InputFingerprint | ConvertTo-Json -Depth 3 -Compress

# These diagnostics measure only the allocated capacity of historical collision
# shape buffers. They are not process working set, asset/GPU memory or a sampled
# high-water mark. Keep limits independent of one particular PhysicsAsset.
function Get-SmokeMemoryEndpoint([string]$Text, [string]$Role) {
    $Rows = @([regex]::Matches($Text, 'PVP_MEMORY\s+[^\r\n]+'))
    $Pawns = @{}
    $PopulatedPawns = @{}
    $Violations = [Collections.Generic.List[string]]::new()
    $MalformedRows = 0
    $ValidRows = 0
    $MaxHistoryBytes = [long]0
    $MaxReservedShapes = [long]0
    $MaxShapeStride = [long]0
    $MaxViewmodelAnim = [long]0
    $MaxViewmodelTick = [long]0
    $PostProcessRows = 0
    $MaxViewmodelPostProcess = [long]0
    foreach ($Row in $Rows) {
        $Fields = @{}
        foreach ($Field in [regex]::Matches($Row.Value, '\b(Pawn|NetMode|HistoryBytes|ReservedShapes|ShapeStride|ViewmodelAnim|ViewmodelTick|ViewmodelPostProcess)=(\S+)')) {
            $Fields[$Field.Groups[1].Value] = $Field.Groups[2].Value
        }
        $Numbers = @{}
        $WellFormed = $Fields.ContainsKey('Pawn')
        foreach ($Name in @('NetMode', 'HistoryBytes', 'ReservedShapes', 'ShapeStride', 'ViewmodelAnim', 'ViewmodelTick')) {
            $Value = [long]0
            if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -notmatch '^\d+$' -or
                -not [long]::TryParse($Fields[$Name], [ref]$Value)) {
                $WellFormed = $false
            }
            else { $Numbers[$Name] = $Value }
        }
        if ($Fields.ContainsKey('ViewmodelPostProcess')) {
            $Value = [long]0
            if ($Fields['ViewmodelPostProcess'] -notmatch '^[01]$' -or
                -not [long]::TryParse($Fields['ViewmodelPostProcess'], [ref]$Value)) { $WellFormed = $false }
            else {
                $PostProcessRows++
                $MaxViewmodelPostProcess = [Math]::Max($MaxViewmodelPostProcess, $Value)
            }
        }
        if (-not $WellFormed) {
            $MalformedRows++
            $Violations.Add('Malformed or out-of-range diagnostic row')
            continue
        }
        $ValidRows++
        $Pawns[$Fields['Pawn']] = $true
        $MaxHistoryBytes = [Math]::Max($MaxHistoryBytes, $Numbers['HistoryBytes'])
        $MaxReservedShapes = [Math]::Max($MaxReservedShapes, $Numbers['ReservedShapes'])
        $MaxShapeStride = [Math]::Max($MaxShapeStride, $Numbers['ShapeStride'])
        $MaxViewmodelAnim = [Math]::Max($MaxViewmodelAnim, $Numbers['ViewmodelAnim'])
        $MaxViewmodelTick = [Math]::Max($MaxViewmodelTick, $Numbers['ViewmodelTick'])
        $ExpectedNetMode = if ($Role -eq 'Server') { 1 } else { 3 }
        if ($Numbers['NetMode'] -ne $ExpectedNetMode) { $Violations.Add('Unexpected NetMode for ' + $Fields['Pawn']) }
        if ($Numbers['ShapeStride'] -lt 1 -or $Numbers['ShapeStride'] -gt 96 -or
            $Numbers['ReservedShapes'] -gt (32 * 64) -or
            $Numbers['HistoryBytes'] -gt (32 * 64 * [decimal]$Numbers['ShapeStride'])) {
            $Violations.Add('Historical shape-buffer bound exceeded for ' + $Fields['Pawn'])
        }
        if ($Numbers['ViewmodelAnim'] -notin @(0, 1) -or $Numbers['ViewmodelTick'] -notin @(0, 1)) {
            $Violations.Add('Invalid viewmodel boolean for ' + $Fields['Pawn'])
        }
        if ($Role -eq 'Server') {
            if ($Numbers['HistoryBytes'] -gt 0 -and $Numbers['ReservedShapes'] -gt 0) { $PopulatedPawns[$Fields['Pawn']] = $true }
            if ($Numbers['ViewmodelAnim'] -ne 0 -or $Numbers['ViewmodelTick'] -ne 0) {
                $Violations.Add('Dedicated-server viewmodel main animation or tick enabled for ' + $Fields['Pawn'])
            }
        }
        elseif ($Numbers['HistoryBytes'] -ne 0) { $Violations.Add('Client allocated authority-only hit history for ' + $Fields['Pawn']) }
    }
    if ($Role -eq 'Server' -and $PopulatedPawns.Count -lt 2) { $Violations.Add('Fewer than two distinct Pawns with populated server history') }
    if ($Role -ne 'Server' -and $ValidRows -lt 1) { $Violations.Add('No valid client memory diagnostic') }
    return [pscustomobject]@{
        Role = $Role; ObservedRows = $Rows.Count; ValidRows = $ValidRows; MalformedRows = $MalformedRows
        DistinctPawns = $Pawns.Count; PopulatedHistoryPawns = $PopulatedPawns.Count
        MaxHistoryBytes = $MaxHistoryBytes; MaxReservedShapes = $MaxReservedShapes; MaxShapeStride = $MaxShapeStride
        MaxViewmodelMainAnimInstances = $MaxViewmodelAnim; MaxViewmodelTickEnabled = $MaxViewmodelTick
        PostProcessObservedRows = $PostProcessRows
        MaxViewmodelPostProcessInstances = if ($PostProcessRows -gt 0) { $MaxViewmodelPostProcess } else { $null }
        ViolationCount = $Violations.Count; ViolationExamples = @($Violations | Select-Object -Unique -First 8)
        Verified = $Violations.Count -eq 0
    }
}

# Account for every predicted round, not only one successful exchange. Weapon
# NetGUID separates independently spawned guns whose local shot/session numbers
# may coincide. A rejected request is reported separately from a collision miss.
function Get-SmokeShotIdentity([string]$Record) {
    $Identity = [regex]::Match($Record, 'ShotId=(\d+)\s+Session=(\d+)\s+Generation=(\d+)(?=\s|$)')
    $Weapon = [regex]::Match($Record, '\bWeaponNetGuid=(\d+)(?=\s|$)')
    if (-not $Identity.Success -or -not $Weapon.Success) { return $null }
    [uint32]$Shot = 0
    [uint32]$Session = 0
    [uint32]$Generation = 0
    [uint64]$WeaponGuid = 0
    if (-not [uint32]::TryParse($Identity.Groups[1].Value, [ref]$Shot) -or $Shot -eq 0 -or $Shot -gt [int]::MaxValue -or
        -not [uint32]::TryParse($Identity.Groups[2].Value, [ref]$Session) -or $Session -eq 0 -or
        -not [uint32]::TryParse($Identity.Groups[3].Value, [ref]$Generation) -or $Generation -eq 0 -or
        -not [uint64]::TryParse($Weapon.Groups[1].Value, [ref]$WeaponGuid) -or $WeaponGuid -eq 0) { return $null }
    return ('{0}:{1}:{2}:{3}' -f $WeaponGuid, $Generation, $Session, $Shot)
}

function Get-SmokeShotLedger([string]$ClientText, [string]$ServerText, [switch]$RequireShots) {
    $Predictions = @{}
    $Acknowledgements = @{}
    $ServerVerdicts = @{}
    $MissingPayloadFinalizations = @{}
    $MissingIdentity = 0
    $LastOutstanding = -1
    foreach ($Record in [regex]::Matches($ClientText, 'PVP_SMOKE\s+CLIENT_SHOT_(PRESENT|ACK)\s+[^\r\n]+')) {
        $Key = Get-SmokeShotIdentity $Record.Value
        if ($null -eq $Key) {
            $MissingIdentity++
            continue
        }
        if ($Record.Groups[1].Value -eq 'PRESENT') {
            $Count = [regex]::Match($Record.Value, '\bCount=(\S+)')
            [int]$PresentationCount = 0
            if (-not $Count.Success -or -not [int]::TryParse($Count.Groups[1].Value, [ref]$PresentationCount) -or $PresentationCount -ne 1) {
                $MissingIdentity++
                continue
            }
            if (-not $Predictions.ContainsKey($Key)) { $Predictions[$Key] = 0 }
            $Predictions[$Key] += $PresentationCount
        }
        else {
            $Result = [regex]::Match($Record.Value, '\bResult=(\S+)')
            $Outstanding = [regex]::Match($Record.Value, '\bOutstanding=(\S+)')
            [int]$ResultCode = -1
            [int]$OutstandingCount = -1
            if (-not $Result.Success -or -not [int]::TryParse($Result.Groups[1].Value, [ref]$ResultCode) -or $ResultCode -notin 0..10 -or
                -not $Outstanding.Success -or -not [int]::TryParse($Outstanding.Groups[1].Value, [ref]$OutstandingCount) -or $OutstandingCount -lt 0) {
                $MissingIdentity++
                continue
            }
            if (-not $Acknowledgements.ContainsKey($Key)) { $Acknowledgements[$Key] = @() }
            $Acknowledgements[$Key] += $ResultCode
            $LastOutstanding = $OutstandingCount
        }
    }
    foreach ($Record in [regex]::Matches($ServerText, 'PVP_SMOKE\s+SERVER_SHOT_(ACCEPT|REJECT)\s+[^\r\n]+')) {
        $Key = Get-SmokeShotIdentity $Record.Value
        $Result = [regex]::Match($Record.Value, '\bResult=(\S+)')
        [int]$ResultCode = -1
        if ($null -eq $Key -or -not $Result.Success -or -not [int]::TryParse($Result.Groups[1].Value, [ref]$ResultCode) -or
            $ResultCode -notin 0..10 -or ($Record.Groups[1].Value -eq 'ACCEPT') -ne ($ResultCode -eq 0)) { $MissingIdentity++; continue }
        if (-not $ServerVerdicts.ContainsKey($Key)) { $ServerVerdicts[$Key] = @() }
        $ServerVerdicts[$Key] += $ResultCode
    }
    foreach($Record in [regex]::Matches($ServerText,'PVP_SMOKE\s+SHOT_INPUT_HOLE_FINAL\s+[^\r\n]+')){
        $Key=Get-SmokeShotIdentity $Record.Value
        if($null -eq $Key -or $Record.Value -notmatch '\bResult=10\b' -or $Record.Value -notmatch '\bReason=MissingPayload\b'){$MissingIdentity++;continue}
        if(-not $MissingPayloadFinalizations.ContainsKey($Key)){$MissingPayloadFinalizations[$Key]=0}
        $MissingPayloadFinalizations[$Key]++
    }
    # ACKs are already scoped to this owner's log. Do not classify other
    # clients' independent weapons in the shared Server log as orphan requests.
    $OrphanAcknowledgements = @($Acknowledgements.Keys | Where-Object { -not $Predictions.ContainsKey($_) })
    $ObservedWeapons = @{}
    foreach ($Key in @($Predictions.Keys) + @($Acknowledgements.Keys)) { $ObservedWeapons[$Key.Split(':')[0]] = $true }
    $OrphanServerVerdicts = @($ServerVerdicts.Keys | Where-Object {
        $ObservedWeapons.ContainsKey($_.Split(':')[0]) -and -not $Predictions.ContainsKey($_) -and
        @($ServerVerdicts[$_] | Where-Object { $_ -notin @(1, 4) }).Count -gt 0
    })
    $Reasons = @{}
    $NamedReasons = @{}
    $ResultNames = @('Accepted', 'Queued', 'InvalidContext', 'InvalidIdentity', 'Duplicate', 'InvalidTime', 'InvalidAim', 'RateLimited', 'Busy', 'NoAmmo', 'Cancelled')
    $Missing = @()
    $Conflicts = @()
    $DuplicatePresentations = 0
    $Accepted = 0
    $Cancelled = 0
    $LostInputs = 0
    $UnexpectedRejections = 0
    foreach ($Key in $Predictions.Keys) {
        if ($Predictions[$Key] -ne 1) { $DuplicatePresentations++ }
        # Queued and Duplicate are not a new terminal verdict for the original
        # request. The original Accepted/rejection must still reach its owner.
        $Final = if ($Acknowledgements.ContainsKey($Key)) { @($Acknowledgements[$Key] | Where-Object { $_ -notin @(1, 4) } | Select-Object -Unique) } else { @() }
        $ServerFinal = if ($ServerVerdicts.ContainsKey($Key)) { @($ServerVerdicts[$Key] | Where-Object { $_ -notin @(1, 4) } | Select-Object -Unique) } else { @() }
        if ($Final.Count -eq 0) { $Missing += $Key; continue }
        if ($Final.Count -ne 1 -or $ServerFinal.Count -ne 1 -or $Final[0] -ne $ServerFinal[0]) {
            $Conflicts += $Key
            continue
        }
        $Result = [int]$Final[0]
        if(@($ServerVerdicts[$Key]|Where-Object {$_ -notin @(1,4)}).Count -ne 1){$Conflicts+=$Key}
        if($MissingPayloadFinalizations.ContainsKey($Key) -and $Result -ne 10){$Conflicts+=$Key}
        if (-not $Reasons.ContainsKey([string]$Result)) { $Reasons[[string]$Result] = 0 }
        $Reasons[[string]$Result]++
        $ResultName = if ($Result -lt $ResultNames.Count) { $ResultNames[$Result] } else { 'Unknown' + $Result }
        if (-not $NamedReasons.ContainsKey($ResultName)) { $NamedReasons[$ResultName] = 0 }
        $NamedReasons[$ResultName]++
        if ($Result -eq 0) {
            $Accepted++
        }
        elseif ($Result -eq 10) {
            $Cancelled++
            if($MissingPayloadFinalizations.ContainsKey($Key)){
                $LostInputs++
                if($MissingPayloadFinalizations[$Key] -ne 1){$Conflicts+=$Key}
            }
        }
        else { $UnexpectedRejections++ }
    }
    $MissingRequiredShots = [bool]$RequireShots -and $Predictions.Count -eq 0
    $Complete = $Missing.Count -eq 0 -and $Conflicts.Count -eq 0 -and $DuplicatePresentations -eq 0 -and
        $MissingIdentity -eq 0 -and $OrphanAcknowledgements.Count -eq 0 -and $OrphanServerVerdicts.Count -eq 0 -and -not $MissingRequiredShots -and
        ($Predictions.Count -eq 0 -or $LastOutstanding -eq 0)
    return [pscustomobject]@{
        Predicted = $Predictions.Count; Accepted = $Accepted; Cancelled = $Cancelled
        LostInputs = $LostInputs
        GameplayCancellations = $Cancelled-$LostInputs
        UnexpectedRejections = $UnexpectedRejections; ResultCounts = $Reasons; OutcomeCounts = $NamedReasons
        MissingFinalAck = $Missing.Count; ConflictingVerdicts = $Conflicts.Count
        DuplicatePresentations = $DuplicatePresentations; MissingStableIdentity = $MissingIdentity
        OrphanAcknowledgements = $OrphanAcknowledgements.Count
        OrphanAckExamples = @($OrphanAcknowledgements | Select-Object -First 12)
        OrphanServerVerdicts = $OrphanServerVerdicts.Count
        OrphanServerExamples = @($OrphanServerVerdicts | Select-Object -First 12)
        RequiredShots = [bool]$RequireShots; MissingRequiredShots = $MissingRequiredShots
        ExerciseStatus = if ($Predictions.Count -eq 0) { 'NotExercised' } else { 'Exercised' }
        LastOutstanding = $LastOutstanding
        MissingExamples = @($Missing | Select-Object -First 12)
        ConflictExamples = @($Conflicts | Select-Object -First 12)
        Complete = $Complete
        Clean = $UnexpectedRejections -eq 0 -and $LostInputs -eq 0
        Interpretation = 'Accepted is an executed shot, not necessarily a collision hit. Result 10 is a final cancellation, never acceptance; SHOT_INPUT_HOLE_FINAL MissingPayload identifies lost input and fails strict experience even when the reliable final ACK arrives. Gameplay cancellation is reported separately. Repeated batch submits are not extra predictions or verdicts.'
    }
}

# Keep "not requested" separate from a passed scenario. The target identity is
# essential: two unrelated shots or two different targets cannot form a pass.
function Get-SmokeMovingHitIdentity([string]$Line) {
    $Parts=@()
    foreach($Name in @('WeaponNetGuid','Generation','Session','ShotId','TargetNetGuid')){
        $MatchesForField=[regex]::Matches($Line,'\b'+$Name+'=(\d+)(?=\s|$)')
        [uint64]$Value=0
        if($MatchesForField.Count -ne 1 -or -not [uint64]::TryParse($MatchesForField[0].Groups[1].Value,[ref]$Value) -or $Value -eq 0 -or
            ($Name -notin @('WeaponNetGuid','TargetNetGuid') -and $Value -gt [uint32]::MaxValue)){return $null}
        $Parts+=$Value
    }
    return $Parts -join ':'
}
function Test-SmokeMovingHealthDecrease([string]$Line) {
    $Before=[regex]::Match($Line,'\bHealthBefore=([^\s]+)');$After=[regex]::Match($Line,'\bHealthAfter=([^\s]+)')
    [double]$BeforeValue=0;[double]$AfterValue=0
    return $Before.Success -and $After.Success -and
        [double]::TryParse($Before.Groups[1].Value,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$BeforeValue) -and
        [double]::TryParse($After.Groups[1].Value,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$AfterValue) -and
        -not [double]::IsNaN($BeforeValue) -and -not [double]::IsNaN($AfterValue) -and -not [double]::IsInfinity($BeforeValue) -and -not [double]::IsInfinity($AfterValue) -and
        $BeforeValue -gt $AfterValue -and $AfterValue -ge 0
}
function Get-SmokeNativeViewAlignment([string]$AimLine,[string]$RewindLine) {
    $AimValue=[regex]::Match($AimLine,'\bTargetViewTime=([^\s]+)')
    $ServerValue=[regex]::Match($RewindLine,'\bTargetTime=([^\s]+)')
    [double]$AimTime=0;[double]$ServerTime=0
    $Finite=$AimValue.Success -and $ServerValue.Success -and
        [double]::TryParse($AimValue.Groups[1].Value,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$AimTime) -and
        [double]::TryParse($ServerValue.Groups[1].Value,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$ServerTime) -and
        -not [double]::IsNaN($AimTime) -and -not [double]::IsNaN($ServerTime) -and -not [double]::IsInfinity($AimTime) -and -not [double]::IsInfinity($ServerTime)
    $Native=$AimLine -match '\bNativeView=1\b' -and $RewindLine -match '\bTimeSource=NativeView\b'
    $Delta=if($Finite){[math]::Round([math]::Abs($AimTime-$ServerTime)*1000.0,6)}else{$null}
    $Verified=$Finite -and $Native -and $Delta -le 1.1
    return [pscustomobject]@{Verified=[bool]$Verified;NativeView=[bool]$Native
        TargetViewTime=if($Finite){$AimTime}else{$null};ServerTargetTime=if($Finite){$ServerTime}else{$null}
        DeltaMilliseconds=if($Finite){[math]::Round($Delta,6)}else{$null};MaximumDeltaMilliseconds=1.1
        Status=if($Verified){'Verified'}else{'NotVerified'}}
}
function Get-SmokeMovingHitEvidence([bool]$Requested, [string]$ServerText, [string[]]$ClientTexts) {
    if (-not $Requested) { return [pscustomobject]@{ Status = 'NotRequested'; Passed = $null } }
    $ServerHits=@{};$ClientHits=@{};$AimInputs=@{};$AimRows=@{};$RewindRows=@{};$Invalid=0
    foreach($Record in [regex]::Matches($ServerText,'PVP_SMOKE\s+MOVING_HIT_SERVER PASS\b[^\r\n]*')){
        $Key=Get-SmokeMovingHitIdentity $Record.Value
        if(-not $Key -or -not (Test-SmokeMovingHealthDecrease $Record.Value)){$Invalid++;continue}
        if($ServerHits.ContainsKey($Key)){$Invalid++};$ServerHits[$Key]=$Record.Value
    }
    foreach($Record in [regex]::Matches($ServerText,'PVP_SMOKE\s+REWIND\b[^\r\n]*')){
        $Key=Get-SmokeMovingHitIdentity $Record.Value
        if(-not $Key){continue}
        if(-not $RewindRows.ContainsKey($Key)){$RewindRows[$Key]=@()};$RewindRows[$Key]+=$Record.Value
    }
    $PeerIndex=0
    foreach($Text in $ClientTexts){
        foreach($Record in [regex]::Matches($Text,'PVP_SMOKE\s+MOVING_HIT_CLIENT PASS\b[^\r\n]*')){
            $Key=Get-SmokeMovingHitIdentity $Record.Value
            if(-not $Key -or -not (Test-SmokeMovingHealthDecrease $Record.Value)){$Invalid++;continue}
            if(-not $ClientHits.ContainsKey($Key)){$ClientHits[$Key]=@()};$ClientHits[$Key]+=$PeerIndex
        }
        foreach($Record in [regex]::Matches($Text,'PVP_SMOKE\s+MOVING_HIT_AIM\b[^\r\n]*')){
            $Key=Get-SmokeMovingHitIdentity $Record.Value
            if(-not $Key){$Invalid++;continue};if(-not $AimInputs.ContainsKey($Key)){$AimInputs[$Key]=@()};$AimInputs[$Key]+=$PeerIndex
            if(-not $AimRows.ContainsKey($Key)){$AimRows[$Key]=@()};$AimRows[$Key]+=[pscustomobject]@{Peer=$PeerIndex;Line=$Record.Value}
        }
        ++$PeerIndex
    }
    $Matched=@($ServerHits.Keys|Where-Object {$Key=$_;$ClientHits.ContainsKey($Key) -and $AimInputs.ContainsKey($Key) -and @($ClientHits[$Key]|Where-Object {$_ -in $AimInputs[$Key]}).Count -gt 0})
    $NativeChecks=@()
    foreach($Key in $Matched){
        $Aims=@($AimRows[$Key]|Where-Object {$_.Peer -in $ClientHits[$Key]})
        $Rewinds=@($RewindRows[$Key])
        if($Aims.Count -eq 1 -and $Rewinds.Count -eq 1){$Check=Get-SmokeNativeViewAlignment $Aims[0].Line $Rewinds[0]}
        else{$Check=Get-SmokeNativeViewAlignment '' ''}
        $Check|Add-Member -NotePropertyName Identity -NotePropertyValue $Key
        $NativeChecks+=$Check
    }
    $NativeVerified=$Matched.Count -gt 0 -and @($NativeChecks|Where-Object {-not $_.Verified}).Count -eq 0
    $Passed=$Matched.Count -gt 0 -and $Invalid -eq 0 -and $NativeVerified
    return [pscustomobject]@{Status=if($Passed){'Passed'}elseif($Invalid -gt 0){'InvalidEvidence'}elseif($Matched.Count -gt 0){'NotVerified'}else{'MissingEvidence'};Passed=$Passed
        MatchedIdentities=$Matched;InvalidRows=$Invalid;ServerHits=$ServerHits.Count;ClientHits=$ClientHits.Count;AimInputs=$AimInputs.Count
        NativeViewAligned=$NativeVerified;NativeViewChecks=$NativeChecks
        Scope='One identical five-part identity across local aim, authoritative hit and client health reduction; NativeView=1 and server TimeSource=NativeView must align actual displayed TargetViewTime with TargetTime within 1.1 ms. Fallback or absent metadata is NotVerified. Single-shot controlled scenario, not a hit-rate estimate.'}
}

function Get-SmokeRequiredDeepEvidence([bool]$RequireMovingHit) {
    @('Server.DedicatedIdentity', 'Server.Displacement', 'Deep.BothClientClocks',
        'Deep.RemoteDisplacement', 'Deep.AcceptAckRewindSameShot', 'Deep.NoRepeatedPresentation',
        'Deep.AllPredictionsFinal', 'Deep.NoUnexpectedShotRejections',
        'Server.MemoryAccounting', 'Deep.ClientMemoryAccounting')
    if ($RequireMovingHit) { 'Deep.MovingHit' }
}

# A distinct policy check does not turn the strict zero-rejection gate green.
# Every stale denial needs admission-time evidence AND an unchanged mutation
# boundary; future/identity/rate/unknown denials are never waived.
function Get-SmokeWeakPolicyEvidence([string]$ServerText,[string[]]$ClientTexts,[bool]$RequireInjectedInput=$false) {
    $Failures = [Collections.Generic.List[string]]::new()
    $TimeRows = @{}; $Boundaries = @{}; $Commits = @{}
    foreach ($item in @(@{Marker='SHOT_TIME_REJECT';Table=$TimeRows},@{Marker='SHOT_REJECT_BOUNDARY';Table=$Boundaries},@{Marker='SHOT_COMMIT';Table=$Commits})) {
        foreach ($record in [regex]::Matches($ServerText, ('PVP_SMOKE\s+'+$item.Marker+'\s+[^\r\n]+'))) {
            $key = Get-SmokeShotIdentity $record.Value
            if ($key) { $item.Table[$key] = $record.Value }
        }
    }
    $Rejected = 0; $MeasuredStale = 0; $Accepted = 0; $FinalOutcomes=@{}
    foreach ($record in [regex]::Matches($ServerText, 'PVP_SMOKE\s+SERVER_SHOT_(ACCEPT|REJECT)\s+[^\r\n]+')) {
        $key = Get-SmokeShotIdentity $record.Value
        if (-not $key) { $Failures.Add('Server verdict lacks a valid stable identity.'); continue }
        $result = [regex]::Match($record.Value,'\bResult=(\d+)').Groups[1].Value
        if ($result -in @('1','4')) { continue }
        $FinalOutcomes[$key]=$result
        if ($result -eq '0') {
            $Accepted++
            $commit = $Commits[$key]
            if (-not $commit -or $commit -notmatch '\bAmmoBefore=(\d+) AmmoAfter=(\d+)\b' -or [int]$Matches[2] -ne [int]$Matches[1]-1) {
                $Failures.Add('Accepted shot lacks exactly-one-ammo commit: '+$key)
            }
            continue
        }
        $Rejected++
        if ($result -ne '5') { $Failures.Add('Non-stale terminal rejection: '+$key+' Result='+$result); continue }
        $time = $TimeRows[$key]; $boundary = $Boundaries[$key]
        if (-not $time -or $time -notmatch '\bStage=(?:Admission|Execution) Reason=Stale AgeMs=([\d.]+) AllowedAgeMs=([\d.]+)\b' -or
            [double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture) -le [double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)) {
            $Failures.Add('Rejection lacks measured stale timing: '+$key)
        } else { $MeasuredStale++ }
        if (-not $boundary -or $boundary -notmatch '\bAmmoBefore=(\d+) AmmoAfter=(\d+) ReserveBefore=(\d+) ReserveAfter=(\d+) ExecutedBefore=(\d+) ExecutedAfter=(\d+)\b' -or
            $Matches[1] -ne $Matches[2] -or $Matches[3] -ne $Matches[4] -or $Matches[5] -ne $Matches[6]) {
            $Failures.Add('Rejection mutation boundary is missing or changed: '+$key)
        }
        $parts = $key.Split(':')
        if ($ServerText -match ('PVP_SMOKE\s+REWIND ShotId='+$parts[3]+'\b[^\r\n]*WeaponNetGuid='+$parts[0]+'\b')) {
            $Failures.Add('Rejected shot entered authoritative trace: '+$key)
        }
    }
    $Injected=@{};$InjectedRefunds=0
    foreach($text in $ClientTexts){
        foreach($record in [regex]::Matches($text,'PVP_SMOKE\s+POLICY_STALE_INPUT\s+[^\r\n]+')){
            $key=Get-SmokeShotIdentity $record.Value
            if(-not $key -or $record.Value -notmatch '\bOffsetSeconds=1(?:\.0+)?\b'){$Failures.Add('Injected stale input lacks its stable identity/offset.');continue}
            if($Injected.ContainsKey($key)){$Failures.Add('Stale input was injected more than once: '+$key);continue}
            $Injected[$key]=$true
            if($FinalOutcomes[$key] -ne '5'){$Failures.Add('The deterministic stale input did not receive InvalidTime: '+$key)}
            $Prediction=@([regex]::Matches($text,'PVP_SMOKE\s+CLIENT_AMMO_PREDICT\s+[^\r\n]+')|Where-Object {(Get-SmokeShotIdentity $_.Value) -eq $key})
            $Reconcile=@([regex]::Matches($text,'PVP_SMOKE\s+CLIENT_AMMO_RECONCILE\s+[^\r\n]+')|Where-Object {(Get-SmokeShotIdentity $_.Value) -eq $key -and $_.Value -match '\bResult=5\b'})
            if($Prediction.Count -ne 1 -or $Reconcile.Count -ne 1){$Failures.Add('Injected input lacks one real prediction/rejection reconciliation: '+$key);continue}
            $Fields=@{};foreach($Field in [regex]::Matches($Reconcile[0].Value,'(\w+)=([^\s]+)')){$Fields[$Field.Groups[1].Value]=$Field.Groups[2].Value}
            if($Fields.BaseAmmo -notmatch '^\d+$' -or $Fields.DisplayAmmo -notmatch '^\d+$' -or
                $Fields.PendingUnincludedInputs -notmatch '^\d+$' -or $Fields.RejectedStillPending -ne '0'){
                $Failures.Add('Injected rejection has no observed selective-refund evidence: '+$key);continue
            }
            if([int]$Fields.DisplayAmmo -ne [math]::Max(0,[int]$Fields.BaseAmmo-[int]$Fields.PendingUnincludedInputs)){
                $Failures.Add('Injected rejection failed to refund itself while preserving later pending shots: '+$key)
            }else{$InjectedRefunds++}
        }
    }
    $Settled = 0
    foreach ($text in $ClientTexts) {
        if ($text -notmatch 'PVP_SMOKE\s+CLIENT_PVE_COMPLETE PASS') { continue }
        $row = [regex]::Matches($text,'PVP_SMOKE\s+WEAK_POLICY_SETTLED Ammo=(\d+) Reserve=(\d+) Outstanding=0 Firing=0 Reloading=0 Aiming=0 Sprinting=0\b') | Select-Object -Last 1
        $ack = [regex]::Matches($text,'PVP_SMOKE\s+CLIENT_SHOT_ACK[^\r\n]*Result=\d+ Ammo=(\d+)[^\r\n]*Outstanding=0\b') | Select-Object -Last 1
        if ($null -eq $row -or $null -eq $ack -or $row.Groups[1].Value -ne $ack.Groups[1].Value) {
            $Failures.Add('Owner did not settle to final acknowledged ammunition and released actions.')
        } else { $Settled++ }
    }
    if ($Accepted -lt 1) { $Failures.Add('No real accepted shot was exercised.') }
    if ($Rejected -lt 1) { $Failures.Add('Stale policy was not exercised; a no-rejection run is not this boundary test.') }
    if ($Settled -lt 1) { $Failures.Add('No active owner settled after real PvE combat.') }
    if($RequireInjectedInput -and $Injected.Count -lt 1){$Failures.Add('No deterministic stale input was exercised; natural packet loss cannot substitute for this case.')}
    [pscustomobject]@{Passed=($Failures.Count-eq 0);Accepted=$Accepted;TerminalRejected=$Rejected;MeasuredStaleRejected=$MeasuredStale;SettledOwners=$Settled;Failures=@($Failures);
        InjectedInputs=$Injected.Count;InjectedInputsRefunded=$InjectedRefunds;RequiresDeterministicInjection=$RequireInjectedInput
        Scope='Admission/execution-age policy and rejection cleanup only. The explicit stale input must receive InvalidTime without authority mutation and must refund only itself on the owning client. Not zero-rejection experience acceptance, transport optimization, hit-rate or per-packet retransmission attribution.'}
}

function Test-SmokeFailureLog([string]$Text) {
    return $Text -match $SmokeConfig.Failure -or $Text -match $SmokeConfig.ExplicitOom
}

function Invoke-SmokeSsh([string]$Command) {
    # OpenSSH receives paths as separate native arguments; no key contents,
    # interactive password prompt, agent fallback or trust-on-first-use bypass.
    $PreviousPreference = $ErrorActionPreference
    $PreviousOutputEncoding = $OutputEncoding
    try {
        $ErrorActionPreference = 'Continue'
        # Windows PS 5 removes nested argv quotes and appends CRLF to pipeline
        # objects. Encode the validated UTF-8/LF script for stdin transport;
        # decoding discards only transport whitespace, not shell source quotes.
        $OutputEncoding = [Text.UTF8Encoding]::new($false)
        $EncodedCommand = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Command.Replace("`r`n", "`n")))
        $Lines = @($EncodedCommand | & $SshPath @SshArguments $SshTarget 'base64 --decode --ignore-garbage | bash -se' 2>&1)
        $Code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $PreviousPreference
        $OutputEncoding = $PreviousOutputEncoding
    }
    $Text = ($Lines | ForEach-Object { [string]$_ }) -join "`n"
    if ($Code -ne 0) { throw "SSH command failed (exit $Code): $Text" }
    return $Text
}

function Update-RemoteSmokeLog([switch]$Force) {
    if (-not $IsRemote -or -not $RemoteOwned) { return }
    if (-not $Force -and $RunClock.Elapsed.TotalSeconds - $script:LastRemotePoll -lt 1.0) { return }
    $script:LastRemotePoll = $RunClock.Elapsed.TotalSeconds
    # Read only this invocation's absolute log and exact systemd unit. No grep
    # over shared journals and no process-name based lifetime management.
    $Text = Invoke-SmokeSsh ("if test -f '$RemoteLog'; then cat '$RemoteLog'; fi; pvp_unit_state=`$(systemctl show '$RemoteUnit' --property=ActiveState --value 2>/dev/null || true); printf '\nPVP_REMOTE_UNIT_STATE=%s\n' `"`${pvp_unit_state:-inactive}`"")
    $State = [regex]::Match($Text, '(?m)^PVP_REMOTE_UNIT_STATE=([a-z-]+)\s*$')
    if (-not $State.Success) { throw 'Remote unit state could not be read safely.' }
    $script:RemoteActiveState = $State.Groups[1].Value
    $LogText = [regex]::Replace($Text, '(?m)^PVP_REMOTE_UNIT_STATE=[a-z-]+\s*$', '')
    [IO.File]::WriteAllText($ServerLog, $LogText, [Text.UTF8Encoding]::new($false))
}

function Start-RemoteSmokeServer {
    # Deployment/user/firewall provisioning is deliberately outside this test.
    # Inspect the actual package before reserving a unique, bounded service.
    $PreflightCommand = @"
set -eu
test `"`$(id -u)`" -eq 0
test `"`$(uname -m)`" = x86_64
test `"`$(id -u '$RemoteRunUser')`" -ne 0
awk '/MemAvailable:/ { if (`$2 < 1048576) { print "Insufficient available memory for an isolated 900M server test"; exit 92 } }' /proc/meminfo
test -x '$RemoteExecutable'
test -r '$RemoteReceipt'
test -d '$RemotePackageRoot/Engine'
test -d '$RemotePackageRoot/fpstrue/Content'
test -n `"`$(find '$RemotePackageRoot/fpstrue/Content' -maxdepth 2 -type f \( -name '*.pak' -o -name '*.utoc' -o -name 'AssetRegistry.bin' \) -print -quit)`"
test `"`$(od -An -tx1 -N4 '$RemoteExecutable' | tr -d ' \n')`" = 7f454c46
command -v systemd-run >/dev/null
command -v ss >/dev/null
if ss -H -lun 'sport = :$Port' | grep -q .; then echo 'Remote UDP port is already in use'; exit 91; fi
test `"`$(systemctl show '$RemoteUnit' --property=LoadState --value)`" = not-found
test ! -e '$RemoteRunDirectory'
printf 'PVP_REMOTE_SHA256='
sha256sum '$RemoteExecutable' | cut -d ' ' -f 1
printf 'PVP_REMOTE_RECEIPT_SHA256='
sha256sum '$RemoteReceipt' | cut -d ' ' -f 1
printf 'PVP_REMOTE_RECEIPT_BEGIN\n'
cat '$RemoteReceipt'
printf '\nPVP_REMOTE_RECEIPT_END\n'
"@
    $Preflight = Invoke-SmokeSsh ($PreflightCommand.Replace("`r`n", "`n"))
    $HashMatch = [regex]::Match($Preflight, '(?m)^PVP_REMOTE_SHA256=([a-fA-F0-9]{64})\s*$')
    if (-not $HashMatch.Success -or $HashMatch.Groups[1].Value -ne $RemoteServerSha256) {
        throw 'The deployed Server ELF SHA-256 does not match the explicitly requested build.'
    }
    $ReceiptHashMatch = [regex]::Match($Preflight, '(?m)^PVP_REMOTE_RECEIPT_SHA256=([a-fA-F0-9]{64})\s*$')
    if (-not $ReceiptHashMatch.Success) { throw 'The remote receipt did not provide its actual file SHA-256.' }
    $ReceiptMatch = [regex]::Match($Preflight, '(?s)PVP_REMOTE_RECEIPT_BEGIN\s*(\{.*?\})\s*PVP_REMOTE_RECEIPT_END')
    if (-not $ReceiptMatch.Success) { throw 'The remote package did not provide a readable build receipt.' }
    $Receipt = $ReceiptMatch.Groups[1].Value | ConvertFrom-Json
    $Products = @($Receipt.BuildProducts | Where-Object {
        $_.Type -eq 'Executable' -and ([string]$_.Path).Replace('\', '/') -like '*/Binaries/Linux/fpstrueServer'
    })
    if ($Receipt.TargetName -ne 'fpstrueServer' -or $Receipt.Configuration -ne 'Development' -or
        $Receipt.Platform -ne 'Linux' -or $Products.Count -ne 1 -or
        ($Receipt.TargetType -and $Receipt.TargetType -ne 'Server')) {
        throw 'The deployed receipt is not a Linux Development fpstrueServer target.'
    }
    $PackageReceipts['Server'] = [pscustomobject]@{
        Path = $RemoteReceipt; TargetName = $Receipt.TargetName; TargetType = $Receipt.TargetType
        Platform = $Receipt.Platform; Configuration = $Receipt.Configuration; Sha256 = $HashMatch.Groups[1].Value
        Executable = $RemoteExecutable; ReceiptSha256 = $ReceiptHashMatch.Groups[1].Value
    }
    # Mark cleanup ownership before a launch whose SSH response could be lost.
    # The unpredictable unit and run directory were verified not to exist.
    $script:RemoteOwned = $true
    $RuntimeLimit = $TimeoutSeconds + 30
    $ArgumentsText = $ServerArguments -join ' '
    $LaunchCommand = "set -eu; install -d -o '$RemoteRunUser' -m 700 '$RemoteRunDirectory'; systemd-run --unit='$RemoteUnit' --uid='$RemoteRunUser' --property='WorkingDirectory=$RemotePackageRoot' --property='RuntimeMaxSec=$RuntimeLimit' --property='MemoryMax=900M' --property='KillMode=control-group' --property='Restart=no' --property='NoNewPrivileges=yes' --collect -- '$RemoteExecutable' $ArgumentsText"
    $null = Invoke-SmokeSsh $LaunchCommand
    Update-RemoteSmokeLog -Force
}

function Start-SmokeProcess([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$LogPath) {
    $WorkingDirectory = if ($ExecutionMode -eq 'Editor') { $ProjectRoot } else { Split-Path -Parent $Executable }
    $WindowStyle = if ($VisibleClients -and $Name -like 'Client*') { 'Normal' } else { 'Hidden' }
    $Process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory -WindowStyle $WindowStyle -PassThru
    $Started.Add([pscustomobject]@{
        Name = $Name; Process = $Process; Id = $Process.Id
        StartedUtc = $Process.StartTime.ToUniversalTime(); Log = $LogPath; Executable = $Executable
        ExpectedExit = $false
    })
}

function Read-SmokeLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return [string](Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue)
}

# Rendering is not inferred from network readiness. The game requests a real
# viewport screenshot only after its camera, weapon, HUD and replicated enemies
# are ready, and emits CAPTURE after the engine has finished writing it. Keep
# fixed names inside this invocation's directories; never trust a log-supplied
# arbitrary file path or collect the private resume credential directory.
function Get-SmokeVisualCapture([object]$Command, [int]$Restored, [string]$ResumeId) {
    $CaptureKey = $Command.Name + ':' + $Restored + ':' + $ResumeId
    if ($VisualCaptures.ContainsKey($CaptureKey)) { return $VisualCaptures[$CaptureKey] }
    if ($ResumeId -notmatch '^\d+$') { return $null }
    $Text = Read-SmokeLog $Command.Log
    $Identity = 'Restored=' + $Restored + '\s+ResumeId=' + [regex]::Escape($ResumeId) + '\b'
    $Ready = [regex]::Match($Text, 'PVP_VISUAL_READY\s+PASS\s+' + $Identity)
    $Capture = [regex]::Match($Text, 'PVP_VISUAL_CAPTURE\s+PASS\s+' + $Identity +
        '\s+File=(\S+)\s+Width=(\d+)\s+Height=(\d+)\b')
    if (-not $Ready.Success -or -not $Capture.Success) { return $null }
    if ($Capture.Index -le $Ready.Index) { throw "$($Command.Name) captured before its own visual readiness marker." }
    $ExpectedName = if ($Restored -eq 1) { 'restored.png' } else { 'initial.png' }
    if ($Capture.Groups[1].Value -cne $ExpectedName) { throw "$($Command.Name) reported an unexpected screenshot filename." }
    $CapturePath = Join-Path $Command.VisualDirectory $ExpectedName
    if (-not (Test-Path -LiteralPath $CapturePath -PathType Leaf)) { throw "$($Command.Name) reported a screenshot that does not exist: $CapturePath" }
    $File = Get-Item -LiteralPath $CapturePath
    if ($File.Length -lt 1024 -or $File.Length -gt 16MB) { throw "$($Command.Name) screenshot size is outside the expected bounded PNG range." }
    $Bytes = [IO.File]::ReadAllBytes($CapturePath)
    $PngSignature = @(137, 80, 78, 71, 13, 10, 26, 10)
    for ($Index = 0; $Index -lt $PngSignature.Count; $Index++) {
        if ($Bytes[$Index] -ne $PngSignature[$Index]) { throw "$($Command.Name) screenshot is not a PNG." }
    }
    if ([Text.Encoding]::ASCII.GetString($Bytes, 12, 4) -cne 'IHDR' -or
        [Text.Encoding]::ASCII.GetString($Bytes, $Bytes.Length - 8, 4) -cne 'IEND') {
        throw "$($Command.Name) screenshot does not contain a complete PNG header/end marker."
    }
    [uint64]$Width = 0
    [uint64]$Height = 0
    for ($Index = 0; $Index -lt 4; $Index++) {
        $Width = $Width * 256 + $Bytes[16 + $Index]
        $Height = $Height * 256 + $Bytes[20 + $Index]
    }
    if ($Width -ne 1280 -or $Height -ne 720 -or $Capture.Groups[2].Value -ne [string]$Width -or
        $Capture.Groups[3].Value -ne [string]$Height) {
        throw "$($Command.Name) screenshot dimensions do not match the requested 1280x720 viewport and capture marker."
    }
    $Artifact = [pscustomobject]@{
        Client = $Command.Name; Restored = $Restored; ResumeId = $ResumeId
        Path = $CapturePath; Width = $Width; Height = $Height; Bytes = $File.Length
        Sha256 = (Get-FileHash -LiteralPath $CapturePath -Algorithm SHA256).Hash
        ReadyMarkerIndex = $Ready.Index; CaptureMarkerIndex = $Capture.Index
    }
    $VisualCaptures[$CaptureKey] = $Artifact
    return $Artifact
}

# Parse only non-secret state evidence. A bare PASS or a previous connection's
# READY cannot prove that this run recovered the retained participant.
function Get-SmokeResumeRecords([string]$Text, [string]$EventName) {
    foreach ($Line in [regex]::Matches($Text, '(?m)^.*PVP_RESUME\s+' + [regex]::Escape($EventName) + '\b[^\r\n]*')) {
        $Fields = @{}
        foreach ($Field in [regex]::Matches($Line.Value, '(\w+)=([^\s]+)')) { $Fields[$Field.Groups[1].Value] = $Field.Groups[2].Value }
        if ($Fields.ResumeId -notmatch '^\d+$' -or $Fields.Health -notmatch '^\d+(?:\.\d+)?$' -or
            $Fields.Ammo -notmatch '^\d+$' -or $Fields.Kills -notmatch '^\d+$' -or
            $Fields.Deaths -notmatch '^\d+$' -or $Fields.Dead -notmatch '^[01]$' -or
            $Fields.PawnNetGuid -notmatch '^\d+$' -or $Fields.StateNetGuid -notmatch '^\d+$' -or
            -not $Fields.Pawn -or $Fields.Pawn -in @('None', 'nullptr', 'NULL')) { continue }
        [uint64]$PawnNetGuid = 0
        [uint64]$StateNetGuid = 0
        if (-not [uint64]::TryParse($Fields.PawnNetGuid, [ref]$PawnNetGuid) -or
            -not [uint64]::TryParse($Fields.StateNetGuid, [ref]$StateNetGuid)) { continue }
        [pscustomobject]@{
            ResumeId = $Fields.ResumeId; Restored = $Fields.Restored; Pawn = $Fields.Pawn; PS = $Fields.PS
            PawnNetGuid = $PawnNetGuid; StateNetGuid = $StateNetGuid
            LogIndex = $Line.Index
            Health = [double]::Parse($Fields.Health, [Globalization.CultureInfo]::InvariantCulture)
            Ammo = [int]$Fields.Ammo; Kills = [int]$Fields.Kills; Deaths = [int]$Fields.Deaths; Dead = [int]$Fields.Dead
        }
    }
}

function Test-SmokeResumeIdentity([object]$Baseline, [object]$Server, [object]$Client) {
    # FNames are local diagnostics, not a cross-process identity. Keep the
    # server's same-object check and independently require matching real GUIDs.
    return $Server.Pawn -eq $Baseline.Pawn -and $Server.PS -eq $Baseline.PS -and
        $Baseline.PawnNetGuid -ne 0 -and $Baseline.StateNetGuid -ne 0 -and
        $Server.PawnNetGuid -eq $Baseline.PawnNetGuid -and $Server.StateNetGuid -eq $Baseline.StateNetGuid -and
        $Client.PawnNetGuid -eq $Server.PawnNetGuid -and $Client.StateNetGuid -eq $Server.StateNetGuid
}

function Stop-OwnedSmokeClient([object]$Entry) {
    $Current = Get-Process -Id $Entry.Id -ErrorAction SilentlyContinue
    if ($null -eq $Current -or $Current.StartTime.ToUniversalTime() -ne $Entry.StartedUtc -or
        -not [string]::Equals($Current.Path, $Entry.Executable, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cannot safely identify the owned $($Entry.Name) process for the hard-disconnect scenario."
    }
    # Mark only this exact process as an intentional exit, never every Client2
    # process or every executable sharing its filename.
    $Entry.ExpectedExit = $true
    try { Stop-Process -Id $Current.Id -Force -ErrorAction Stop }
    catch { $Entry.ExpectedExit = $false; throw }
}

function Assert-SmokeRunning {
    if ($EnableNetTrace) {
        $TraceBytes = 0L
        foreach ($TracePath in $TracePaths.Values) {
            if (Test-Path -LiteralPath $TracePath -PathType Leaf) { $TraceBytes += (Get-Item -LiteralPath $TracePath).Length }
        }
        if ($TraceBytes -gt ([long]$TraceMaxMiB * 1MB)) { throw "Owned native traces exceeded the $TraceMaxMiB MiB diagnostic budget; capture stopped and retained as incomplete evidence." }
    }
    Update-RemoteSmokeLog
    if ($IsRemote -and $RemoteOwned -and $RemoteActiveState -notin @('activating', 'active', 'reloading')) {
        throw "The owned remote Server unit is $RemoteActiveState before acceptance completed. Read $ServerLog."
    }
    if ($IsRemote -and (Test-SmokeFailureLog (Read-SmokeLog $ServerLog))) {
        throw "Remote Server reported a smoke failure or crash. Read $ServerLog."
    }
    if ($IncludePve -and (Read-SmokeLog $ServerLog) -match $SmokeConfig.ServerForbiddenRender) {
        throw "Dedicated Server unexpectedly applied render or animation-sharing work. Read $ServerLog."
    }
    foreach ($Entry in $Started) {
        $Entry.Process.Refresh()
        if ($Entry.Process.HasExited -and -not $Entry.ExpectedExit) {
            throw "$($Entry.Name) exited before all acceptance checks passed. Read $($Entry.Log)."
        }
        if (Test-SmokeFailureLog (Read-SmokeLog $Entry.Log)) {
            throw "$($Entry.Name) reported a smoke failure or crash. Read $($Entry.Log)."
        }
        if ($ExecutionMode -eq 'Packaged' -and
            (Read-SmokeLog $Entry.Log) -match 'LogInit: Build Configuration:\s*(\w+)' -and
            $Matches[1] -ne 'Development') {
            throw "$($Entry.Name) is not a Development package. Read $($Entry.Log)."
        }
    }
    if ($RunClock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
        throw "Timed out after $TimeoutSeconds seconds; missing evidence: $($Evidence | ConvertTo-Json -Compress)."
    }
}

$MovingHitEvidence = Get-SmokeMovingHitEvidence ([bool]$MovingHit) '' @()
try {
    if ($IsRemote) { Start-RemoteSmokeServer }
    else { Start-SmokeProcess 'Server' $ServerPath $ServerArguments $ServerLog }
    while ((Read-SmokeLog $ServerLog) -notmatch ('IpNetDriver listening on port ' + $Port + '\b')) {
        Assert-SmokeRunning
        Start-Sleep -Milliseconds $SmokeConfig.PollMilliseconds
    }
    foreach ($ClientCommand in $ClientCommands) {
        Start-SmokeProcess $ClientCommand.Name $ClientPath $ClientCommand.Arguments $ClientCommand.Log
    }

    while (-not $Succeeded) {
        Update-RemoteSmokeLog
        $ServerText = Read-SmokeLog $ServerLog
        $CurrentClientTexts=@{}
        foreach($Command in $ClientCommands){$CurrentClientTexts[$Command.Name]=Read-SmokeLog $Command.Log}
        $ClientRoleMap=Get-SmokeClientRoleMap $CurrentClientTexts
        foreach ($Marker in $SmokeConfig.ServerMarkers) {
            $Evidence['Server.' + $Marker] = $ServerText -match ('PVP_SMOKE[^\r\n]*PASS=' + [regex]::Escape($Marker) + '\b')
        }
        $CompletedClients = 0
        $MovementObservers = 0
        foreach ($ClientCommand in $ClientCommands) {
            $ClientText = Read-SmokeLog $ClientCommand.Log
            $Evidence[$ClientCommand.Name + '.Ready'] = $ClientText -match $SmokeConfig.ClientReady
            $Evidence[$ClientCommand.Name + '.State'] = $ClientText -match $SmokeConfig.ClientState
            $Evidence[$ClientCommand.Name + '.Movement'] = $ClientText -match $SmokeConfig.ClientMovement
            $Evidence[$ClientCommand.Name + '.Complete'] = $ClientText -match $SmokeConfig.ClientComplete
            $Evidence[$ClientCommand.Name + '.RemoteSprint'] = $ClientText -match $SmokeConfig.ClientRemoteSprint
            $Evidence[$ClientCommand.Name + '.RemoteAim'] = $ClientText -match $SmokeConfig.ClientRemoteAim
            if ($Evidence[$ClientCommand.Name + '.State'] -and $Evidence[$ClientCommand.Name + '.Movement'] -and
                $Evidence[$ClientCommand.Name + '.Complete']) {
                # A completion marker with no actual firing is not an active
                # combat completion, even when optional deep checks are disabled.
                $ActiveLedger = Get-SmokeShotLedger $ClientText $ServerText -RequireShots
                $ShotLedgers[$ClientCommand.Name] = $ActiveLedger
                if (-not $ActiveLedger.MissingRequiredShots) { $CompletedClients++ }
            }
            # A single observer must receive both movement states; two partial logs cannot satisfy this check.
            if ($Evidence[$ClientCommand.Name + '.RemoteSprint'] -and $Evidence[$ClientCommand.Name + '.RemoteAim']) { $MovementObservers++ }
        }
        $MissingServer = @($SmokeConfig.ServerMarkers | Where-Object { -not $Evidence['Server.' + $_] })
        $MissingClients = @($ClientCommands | Where-Object { -not $Evidence[$_.Name + '.Ready'] })
        $MissingDeep = 0
        if ($DeepChecks) {
            $ServerMemory = Get-SmokeMemoryEndpoint $ServerText 'Server'
            $ClientMemory = @{}
            $AllClientMemoryVerified = $true
            $Evidence['Server.DedicatedIdentity'] = $ServerText -match 'PVP_SMOKE[^\r\n]*SERVER_IDENTITY[^\r\n]*Dedicated=1\b[^\r\n]*LocalPlayers=0\b'
            $Evidence['Server.Displacement'] = $ServerText -match 'PVP_SMOKE[^\r\n]*SERVER_DISPLACEMENT PASS\b'
            $ClockClients = 0
            $DisplacementObservers = 0
            $AcceptedShots = [regex]::Matches($ServerText, 'SERVER_SHOT_ACCEPT[^\r\n]*ShotId=(\d+)\s+Session=(\d+)\s+Generation=(\d+)[^\r\n]*Result=0\b[^\r\n]*FireTime=(-?\d+(?:\.\d+)?)\b[^\r\n]*WeaponNetGuid=(\d+)\b')
            $RewindShots = [regex]::Matches($ServerText, 'REWIND[^\r\n]*ShotId=(\d+)[^\r\n]*FireTime=(-?\d+(?:\.\d+)?)\b[^\r\n]*Hit=(\S+)[^\r\n]*Bone=(\S+)[^\r\n]*WeaponNetGuid=(\d+)\b')
            $MatchedShot = $false
            $PresentationOnce = $true
            $AllPredictionsFinal = $true
            $NoUnexpectedRejections = $true
            foreach ($ClientCommand in $ClientCommands) {
                $ClientText = Read-SmokeLog $ClientCommand.Log
                $ClientMemory[$ClientCommand.Name] = Get-SmokeMemoryEndpoint $ClientText $ClientCommand.Name
                $AllClientMemoryVerified = $AllClientMemoryVerified -and $ClientMemory[$ClientCommand.Name].Verified
                # One client actively completes the scripted combat while the
                # other observes it; only an actual completing client must shoot.
                $RequiresShots = $Evidence[$ClientCommand.Name + '.State'] -and
                    $Evidence[$ClientCommand.Name + '.Movement'] -and $Evidence[$ClientCommand.Name + '.Complete']
                $Ledger = Get-SmokeShotLedger $ClientText $ServerText -RequireShots:$RequiresShots
                $ShotLedgers[$ClientCommand.Name] = $Ledger
                $AllPredictionsFinal = $AllPredictionsFinal -and $Ledger.Complete
                $NoUnexpectedRejections = $NoUnexpectedRejections -and $Ledger.Clean
                $Clock = $ClientText -match 'CLOCK_SAMPLE PASS[^\r\n]*RTT=[0-9.]+[^\r\n]*Uncertainty=[0-9.]+'
                $RemoteDisplacement = $ClientText -match 'CLIENT_REMOTE_DISPLACEMENT PASS\b'
                $Evidence[$ClientCommand.Name + '.ClockSample'] = $Clock
                $Evidence[$ClientCommand.Name + '.RemoteDisplacement'] = $RemoteDisplacement
                if ($Clock) { $ClockClients++ }
                if ($RemoteDisplacement) { $DisplacementObservers++ }
                foreach ($Shot in $AcceptedShots) {
                    $ShotId = $Shot.Groups[1].Value
                    $Session = $Shot.Groups[2].Value
                    $Generation = $Shot.Groups[3].Value
                    $FireTime = $Shot.Groups[4].Value
                    $WeaponGuid = $Shot.Groups[5].Value
                    $Identity = 'ShotId=' + $ShotId + '\s+Session=' + $Session + '\s+Generation=' + $Generation + '\b'
                    $Ack = $ClientText -match ('CLIENT_SHOT_ACK\s+' + $Identity + '[^\r\n]*Result=0\b[^\r\n]*Predicted=1\b[^\r\n]*WeaponNetGuid=' + $WeaponGuid + '\b')
                    $Predicted = $ClientText -match ('CLIENT_SHOT_PRESENT\s+' + $Identity + '\s+Count=1\b[^\r\n]*WeaponNetGuid=' + $WeaponGuid + '\b')
                    $Rewind = @($RewindShots | Where-Object {
                        $_.Groups[1].Value -eq $ShotId -and $_.Groups[2].Value -eq $FireTime -and
                        $_.Groups[5].Value -eq $WeaponGuid -and $WeaponGuid -ne '0' -and
                        $_.Groups[3].Value -notin @('None', 'none', 'NULL', 'nullptr') -and
                        $_.Groups[4].Value -notin @('None', 'none', 'NULL', 'nullptr')
                    }).Count -gt 0
                    if ($Ack -and $Rewind -and $Predicted) { $MatchedShot = $true }
                }
                # Count events for each full identity, rather than accepting a
                # cumulative counter or requiring unreliable multicasts to arrive.
                $Presentations = [regex]::Matches($ClientText, 'CLIENT_SHOT_PRESENT\s+ShotId=(\d+)\s+Session=(\d+)\s+Generation=(\d+)\s+Count=(\d+)[^\r\n]*WeaponNetGuid=(\d+)\b')
                $Counts = @{}
                foreach ($Presentation in $Presentations) {
                    $Identity = $Presentation.Groups[5].Value + ':' + $Presentation.Groups[1].Value + ':' + $Presentation.Groups[2].Value + ':' + $Presentation.Groups[3].Value
                    if (-not $Counts.ContainsKey($Identity)) { $Counts[$Identity] = 0 }
                    $Counts[$Identity] += [int]$Presentation.Groups[4].Value
                    if ($Counts[$Identity] -gt 1) { $PresentationOnce = $false }
                }
            }
            if ($null -ne $ReconnectCommand) {
                $ClientMemory[$ReconnectCommand.Name] = Get-SmokeMemoryEndpoint (Read-SmokeLog $ReconnectCommand.Log) $ReconnectCommand.Name
                $AllClientMemoryVerified = $AllClientMemoryVerified -and $ClientMemory[$ReconnectCommand.Name].Verified
            }
            $MemoryAccounting = [pscustomobject]@{
                Scope = 'Allocated heap capacity of historical collision-shape buffers only; not process RAM, retained asset/GPU memory or a sampled high-water mark. Dedicated-server main viewmodel animation/tick suppression is checked independently; a retained post-process instance does not imply animation evaluation.'
                Limits = @{ HistoryFrames = 32; ShapesPerFrame = 64; MaxReservedShapesPerPawn = 2048; MaxShapeStrideBytes = 96 }
                Server = $ServerMemory; Clients = $ClientMemory
                Verified = $ServerMemory.Verified -and $AllClientMemoryVerified
            }
            $Evidence['Server.MemoryAccounting'] = $ServerMemory.Verified
            $Evidence['Deep.ClientMemoryAccounting'] = $AllClientMemoryVerified
            $Evidence['Deep.BothClientClocks'] = $ClockClients -eq $SmokeConfig.ClientCount
            $Evidence['Deep.RemoteDisplacement'] = $DisplacementObservers -gt 0
            $Evidence['Deep.AcceptAckRewindSameShot'] = $MatchedShot
            $Evidence['Deep.NoRepeatedPresentation'] = $PresentationOnce
            $Evidence['Deep.AllPredictionsFinal'] = $AllPredictionsFinal
            $Evidence['Deep.NoUnexpectedShotRejections'] = $NoUnexpectedRejections
            $TargetCompensationValues=@([regex]::Matches($ServerText,'PVP_SMOKE\s+REWIND\b[^\r\n]*\bTargetLatencyCompensation=([01])\b')|ForEach-Object {[int]$_.Groups[1].Value}|Select-Object -Unique)
            $TargetCompensationReadback=[pscustomobject]@{Requested=$TargetLatencyCompensation;Observed=$TargetCompensationValues
                MatchesRequest=$TargetCompensationValues.Count -eq 1 -and $TargetCompensationValues[0] -eq $(if($TargetLatencyCompensation -eq 'Enabled'){1}else{0})}
            $Evidence['Server.TargetLatencyCompensation']=$TargetCompensationReadback.MatchesRequest
            $MovingHitEvidence = Get-SmokeMovingHitEvidence ([bool]$MovingHit) $ServerText @(
                $ClientCommands | ForEach-Object { Read-SmokeLog $_.Log })
            $Evidence['Deep.MovingHit'] = $MovingHitEvidence.Passed
            $RequiredDeepEvidence = @(Get-SmokeRequiredDeepEvidence ([bool]$MovingHit))
            if($MovingHit -or $TargetLatencyCompensation -eq 'Disabled'){$RequiredDeepEvidence+='Server.TargetLatencyCompensation'}
            if ($WeakPolicyCorrectness) {
                $PolicyEvidence = Get-SmokeWeakPolicyEvidence $ServerText @($ClientCommands | ForEach-Object { Read-SmokeLog $_.Log }) $true
                $Evidence['Deep.WeakPolicyCorrectness'] = $PolicyEvidence.Passed
                $RequiredDeepEvidence = @($RequiredDeepEvidence | Where-Object { $_ -ne 'Deep.NoUnexpectedShotRejections' }) + 'Deep.WeakPolicyCorrectness'
                if ($PolicyEvidence.Accepted -gt 0 -and $PolicyEvidence.TerminalRejected -eq 0 -and $PolicyEvidence.SettledOwners -gt 0 -and $AllPredictionsFinal) {
                    throw 'WeakPolicyCorrectness was not exercised: all requests in this sample were accepted. Preserve it as a no-rejection sample, not a stale-boundary pass.'
                }
            }
            foreach ($Key in $RequiredDeepEvidence) {
                if (-not $Evidence[$Key]) { $MissingDeep++ }
            }
        }
        $MissingConfiguration = 0
        if ($ExecutionMode -eq 'Packaged') {
            foreach ($Entry in $Started) {
                $Evidence[$Entry.Name + '.DevelopmentPackage'] = (Read-SmokeLog $Entry.Log) -match 'LogInit: Build Configuration:\s*Development\b'
                if (-not $Evidence[$Entry.Name + '.DevelopmentPackage']) { $MissingConfiguration++ }
            }
        }
        if ($IsRemote) {
            $Evidence['Server.DevelopmentPackage'] = $ServerText -match 'LogInit: Build Configuration:\s*Development\b'
            if (-not $Evidence['Server.DevelopmentPackage']) { $MissingConfiguration++ }
        }
        $Evidence['Roles.Mapped']=$ClientRoleMap.Valid
        $Evidence['Roles.ActiveGameplayComplete']=$ClientRoleMap.Valid -and $Evidence[$ClientRoleMap.ActiveClient+'.State'] -and $Evidence[$ClientRoleMap.ActiveClient+'.Movement'] -and $Evidence[$ClientRoleMap.ActiveClient+'.Complete']
        $Evidence['Roles.ObserverReceivedModes']=$ClientRoleMap.Valid -and $Evidence[$ClientRoleMap.ObserverClient+'.RemoteSprint'] -and $Evidence[$ClientRoleMap.ObserverClient+'.RemoteAim']
        $PvpPassed = $MissingServer.Count -eq 0 -and $MissingClients.Count -eq 0 -and
            $CompletedClients -gt 0 -and $MovementObservers -gt 0 -and $MissingConfiguration -eq 0 -and $MissingDeep -eq 0 -and
            $Evidence['Roles.ActiveGameplayComplete'] -and $Evidence['Roles.ObserverReceivedModes']
        $PvePassed = -not $IncludePve
        if ($IncludePve) {
            $MissingPve = 0
            foreach ($Marker in $SmokeConfig.PveServerMarkers) {
                $Evidence['Server.' + $Marker] = $ServerText -match ('PVP_SMOKE[^\r\n]*PASS=' + [regex]::Escape($Marker) + '\b')
                if (-not $Evidence['Server.' + $Marker]) { $MissingPve++ }
            }
            $Evidence['Server.PerformanceStarted'] = $ServerText -match $SmokeConfig.ServerPerformanceStart
            $Evidence['Server.PerformancePopulated'] = $ServerText -match $SmokeConfig.ServerPerformancePopulated
            $Evidence['Server.NoRenderOrSharing'] = $ServerText -notmatch $SmokeConfig.ServerForbiddenRender
            foreach ($Key in @('Server.PerformanceStarted', 'Server.PerformancePopulated', 'Server.NoRenderOrSharing')) {
                if (-not $Evidence[$Key]) { $MissingPve++ }
            }
            $PveObservers = 0
            $PveCompleters = 0
            $PerformanceClients = 0
            foreach ($ClientCommand in $ClientCommands) {
                $ClientText = Read-SmokeLog $ClientCommand.Log
                $Evidence[$ClientCommand.Name + '.PveReplicated'] = $ClientText -match 'PVP_SMOKE[^\r\n]*CLIENT_PVE_REPLICATED PASS\b'
                $Evidence[$ClientCommand.Name + '.PveComplete'] = $ClientText -match 'PVP_SMOKE[^\r\n]*CLIENT_PVE_COMPLETE PASS\b'
                $Evidence[$ClientCommand.Name + '.PerformanceStarted'] = $ClientText -match $SmokeConfig.ClientPerformanceStart
                $Evidence[$ClientCommand.Name + '.PerformanceApplied'] = $ClientText -match $SmokeConfig.ClientPerformanceApplied
                $Evidence[$ClientCommand.Name + '.SharingActive'] = $ClientText -match $SmokeConfig.ClientSharingActive
                if ($Evidence[$ClientCommand.Name + '.PveReplicated']) { $PveObservers++ }
                if ($Evidence[$ClientCommand.Name + '.PveComplete']) { $PveCompleters++ }
                if ($Evidence[$ClientCommand.Name + '.PerformanceStarted'] -and $Evidence[$ClientCommand.Name + '.PerformanceApplied']) { $PerformanceClients++ }
            }
            $PvePassed = $MissingPve -eq 0 -and $PveObservers -eq $SmokeConfig.ClientCount -and $PveCompleters -gt 0 -and
                $PerformanceClients -eq $SmokeConfig.ClientCount -and $ClientRoleMap.Valid -and $Evidence[$ClientRoleMap.ActiveClient+'.PveComplete']
        }
        $InitialVisualPassed = -not $RenderedClients
        if ($RenderedClients) {
            $InitialCaptures = 0
            foreach ($ClientCommand in $ClientCommands) {
                $InitialReady = @(Get-SmokeResumeRecords (Read-SmokeLog $ClientCommand.Log) 'CLIENT_READY' |
                    Where-Object { $_.Restored -eq '0' } | Select-Object -Last 1)
                $Capture = if ($InitialReady.Count -gt 0) { Get-SmokeVisualCapture $ClientCommand 0 $InitialReady[0].ResumeId } else { $null }
                $Evidence[$ClientCommand.Name + '.RenderedCapture'] = $null -ne $Capture
                if ($null -ne $Capture) { $InitialCaptures++ }
            }
            $InitialVisualPassed = $InitialCaptures -eq $SmokeConfig.ClientCount
        }
        $ResumePassed = -not $IncludeReconnect
        if ($IncludeReconnect -and $PvpPassed -and $PvePassed -and $InitialVisualPassed) {
            if ($ReconnectStage -eq 'NotStarted') {
                $TargetName=$ClientRoleMap.ObserverClient
                if($ReconnectMode -eq 'TransportOutage'){
                    $Outage=[regex]::Match($ServerText,'PVP_SMOKE\s+LIVE_OUTAGE\s+Started\b[^\r\n]*\bResumeId=(\d+)\b')
                    $TargetName=if($Outage.Success){@($ClientRoleMap.Players|Where-Object PlayerId -eq ([int]$Outage.Groups[1].Value)|Select-Object -ExpandProperty Client)}else{$null}
                    if($Outage.Success -and @($TargetName).Count -ne 1){throw 'The server outage ResumeId does not map to exactly one original owned client.'}
                }
                $ClientToRestart = $ClientCommands | Where-Object { $_.Name -eq $TargetName } | Select-Object -First 1
                if($null -ne $ClientToRestart){
                $ReconnectClientName=$ClientToRestart.Name
                $InitialReady = @(Get-SmokeResumeRecords (Read-SmokeLog $ClientToRestart.Log) 'CLIENT_READY' |
                    Where-Object { $_.Restored -eq '0' } | Select-Object -Last 1)
                if ($InitialReady.Count -eq 0) { throw "$ReconnectClientName has no confirmed initial handshake identity for the reconnect scenario." }
                # Existence/size only: the runner never reads the ticket bytes.
                $TicketFiles = @(Get-ChildItem -LiteralPath $ClientToRestart.UserDirectory -Filter 'PvpResume.dat' -Recurse -File)
                if ($TicketFiles.Count -ne 1 -or $TicketFiles[0].Length -le 0 -or $TicketFiles[0].Length -gt 4096) {
                    throw "$ReconnectClientName has not persisted exactly one bounded resume credential under its private UserDir."
                }
                $ReconnectResumeId = $InitialReady[0].ResumeId
                $OwnedClient = $Started | Where-Object { $_.Name -eq $ReconnectClientName } | Select-Object -First 1
                if ($null -eq $OwnedClient) { throw "The owned $ReconnectClientName process is missing before the reconnect scenario." }
                $OriginalReconnectClientPid = $OwnedClient.Id
                if ($ReconnectMode -eq 'TransportOutage') {
                    $ReconnectCommand = $ClientToRestart
                    $Evidence['Reconnect.OwnedClientHardStopped'] = $false
                }
                else {
                    Stop-OwnedSmokeClient $OwnedClient
                    $ReconnectStoppedUtc = [DateTime]::UtcNow
                    $Evidence['Reconnect.OwnedClientHardStopped'] = $true
                }
                $ReconnectStage = 'WaitingForServerRetention'
                }
            }
            if ($ReconnectStage -eq 'WaitingForServerRetention') {
                $Retained = @(Get-SmokeResumeRecords $ServerText 'SERVER_RETAIN' |
                    Where-Object { $_.ResumeId -eq $ReconnectResumeId } | Select-Object -Last 1)
                if ($Retained.Count -gt 0) {
                    $ReconnectBaseline = $Retained[0]
                    $Evidence['Reconnect.ServerRetainedParticipant'] = $true
                    if ($ReconnectMode -eq 'TransportOutage') {
                        $ReconnectStage = 'WaitingForOfflineDamage'
                    }
                    else {
                    # Editor cold start can consume most of the real grace
                    # period. Start it now, but do not accept recovery unless
                    # genuine offline damage was recorded before the claim.
                    $ResumeName=$ReconnectClientName+'Resume'
                    $ReconnectLog = Join-Path $OutputDirectory ($ResumeName+'.log')
                    $ReconnectVisualDirectory = if ($RenderedClients) { Join-Path $OutputDirectory ('Visual\'+$ResumeName) } else { $null }
                    $ReconnectCommand = [pscustomobject]@{
                        Name = $ResumeName; Log = $ReconnectLog; UserDirectory = $ClientToRestart.UserDirectory
                        VisualDirectory = $ReconnectVisualDirectory
                        Arguments = @($ClientToRestart.Arguments | Where-Object { $_ -notmatch '^-(?:abslog|PvpSmokeCaptureDir|tracefile)=' }) +
                            @('-PvpSmokeRestoreOnly', ('-abslog="' + $ReconnectLog + '"'))
                    }
                    if ($RenderedClients) {
                        $null = New-Item -ItemType Directory -Path $ReconnectVisualDirectory -Force
                        $ReconnectCommand.Arguments += '-PvpSmokeCaptureDir="' + $ReconnectVisualDirectory + '"'
                    }
                    if ($EnableNetTrace) {
                        $TracePaths[$ResumeName] = Join-Path $OutputDirectory ('Trace\'+$ResumeName+'.utrace')
                        $ReconnectCommand.Arguments += '-tracefile="' + $TracePaths[$ResumeName] + '"'
                    }
                    Start-SmokeProcess $ReconnectCommand.Name $ClientPath $ReconnectCommand.Arguments $ReconnectCommand.Log
                    $ReconnectStage = 'WaitingForOfflineDamage'
                    }
                }
            }
            if ($ReconnectStage -eq 'WaitingForOfflineDamage') {
                $OfflineDamage = @([regex]::Matches($ServerText,
                    'PVP_SMOKE[^\r\n]*PASS=ResumeOfflineDamage\b[^\r\n]*ResumeId=' + [regex]::Escape($ReconnectResumeId) +
                    '\b[^\r\n]*Before=(\d+(?:\.\d+)?)\s+After=(\d+(?:\.\d+)?)\b') | Where-Object {
                        $_.Index -gt $ReconnectBaseline.LogIndex -and
                        [double]::Parse($_.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) -gt
                        [double]::Parse($_.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
                    })
                if ($OfflineDamage.Count -gt 0) {
                    $Evidence['Reconnect.OfflinePawnRemainsDamageable'] = $true
                    $ReconnectOfflineDamageIndex = $OfflineDamage[0].Index
                    $ReconnectStage = 'WaitingForRestoredState'
                }
                elseif (@(Get-SmokeResumeRecords $ServerText 'SERVER_ACCEPT' |
                    Where-Object { $_.ResumeId -eq $ReconnectResumeId -and $_.Restored -eq '1' }).Count -gt 0) {
                    throw 'The participant was restored before the required real offline AI damage occurred.'
                }
            }
            if ($ReconnectStage -eq 'WaitingForRestoredState') {
                $Accepted = @(Get-SmokeResumeRecords $ServerText 'SERVER_ACCEPT' |
                    Where-Object { $_.ResumeId -eq $ReconnectResumeId -and $_.Restored -eq '1' } | Select-Object -Last 1)
                $Restored = @(Get-SmokeResumeRecords (Read-SmokeLog $ReconnectCommand.Log) 'CLIENT_READY' |
                    Where-Object { $_.ResumeId -eq $ReconnectResumeId -and $_.Restored -eq '1' } | Select-Object -Last 1)
                if ($Accepted.Count -gt 0 -and $Restored.Count -gt 0) {
                    $ServerState = $Accepted[0]
                    $ClientState = $Restored[0]
                    $Evidence['Reconnect.OfflineDamageBeforeRestore'] = $ReconnectOfflineDamageIndex -gt $ReconnectBaseline.LogIndex -and
                        $ReconnectOfflineDamageIndex -lt $ServerState.LogIndex
                    $Evidence['Reconnect.SamePawnAndPlayerState'] = Test-SmokeResumeIdentity $ReconnectBaseline $ServerState $ClientState
                    $Evidence['Reconnect.HealthNotRolledBack'] = $ServerState.Health -lt $ReconnectBaseline.Health -and
                        $ClientState.Health -le $ServerState.Health + 0.01 -and $ClientState.Dead -eq $ServerState.Dead
                    $Evidence['Reconnect.AmmoAndScoreRetained'] = $ServerState.Ammo -eq $ReconnectBaseline.Ammo -and
                        $ServerState.Kills -eq $ReconnectBaseline.Kills -and $ServerState.Deaths -ge $ReconnectBaseline.Deaths -and
                        $ClientState.Ammo -eq $ServerState.Ammo -and $ClientState.Kills -eq $ServerState.Kills -and
                        $ClientState.Deaths -eq $ServerState.Deaths
                    $RecoveryText = Read-SmokeLog $ReconnectCommand.Log
                    $Evidence['Reconnect.ClockResampled'] = $RecoveryText -match 'CLOCK_SAMPLE PASS\b'
                    if ($ReconnectMode -eq 'TransportOutage') {
                        $LiveEvidence = Get-SmokeLiveReconnectEvidence $ServerText $RecoveryText $OriginalReconnectClientPid $ReconnectResumeId $ClientState.LogIndex
                        $Evidence['Reconnect.ClockResampled'] = $LiveEvidence.ClockResampled
                        $Evidence['Reconnect.TransportOutageStarted'] = $LiveEvidence.OutageStarted
                        $Evidence['Reconnect.TransportOutageRestored'] = $LiveEvidence.OutageRestored
                        $Evidence['Reconnect.ActualEngineTimeout'] = $LiveEvidence.ActualEngineTimeout
                        $Evidence['Reconnect.SameClientProcess'] = $LiveEvidence.SameClientProcess -and @($Started | Where-Object { $_.Name -eq ($ReconnectClientName+'Resume') }).Count -eq 0
                        $Evidence['Reconnect.AutomaticRetryTransport'] = $LiveEvidence.AutomaticRetryTransport -and $Evidence['Reconnect.SameClientProcess']
                    }
                    $RestoredPerformancePassed = -not $IncludePve
                    if ($IncludePve) {
                        $RestoredText = Get-SmokeRecoveryPerformanceText (Read-SmokeLog $ReconnectCommand.Log) $ReconnectMode $OriginalReconnectClientPid
                        $Evidence['Reconnect.PerformanceStarted'] = $RestoredText -match $SmokeConfig.ClientPerformanceStart
                        $Evidence['Reconnect.PerformanceApplied'] = $RestoredText -match $SmokeConfig.ClientPerformanceApplied
                        $Evidence['Reconnect.SharingActive'] = $RestoredText -match $SmokeConfig.ClientSharingActive
                        $RestoredPerformancePassed = $Evidence['Reconnect.PerformanceStarted'] -and $Evidence['Reconnect.PerformanceApplied']
                    }
                    $StateRestored = $Evidence['Reconnect.OfflineDamageBeforeRestore'] -and
                        $Evidence['Reconnect.SamePawnAndPlayerState'] -and $Evidence['Reconnect.HealthNotRolledBack'] -and
                        $Evidence['Reconnect.AmmoAndScoreRetained']
                    if (-not $StateRestored) { throw 'Reconnect was acknowledged, but retained identity, health, ammo or score checks failed.' }
                    $RestoredVisualPassed = -not $RenderedClients
                    if ($RenderedClients) {
                        $Capture = Get-SmokeVisualCapture $ReconnectCommand 1 $ReconnectResumeId
                        $Evidence['Reconnect.RenderedCapture'] = $null -ne $Capture
                        $RestoredVisualPassed = $null -ne $Capture
                    }
                    $ResumePassed = $StateRestored -and $Evidence['Reconnect.ClockResampled'] -and $RestoredVisualPassed -and $RestoredPerformancePassed
                    if ($ReconnectMode -eq 'TransportOutage') { $ResumePassed = $ResumePassed -and $Evidence['Reconnect.AutomaticRetryTransport'] }
                    if ($ResumePassed) { $ReconnectStage = 'Passed' }
                }
            }
            if ($ReconnectStage -eq 'Passed') { $ResumePassed = $true }
        }
        $Succeeded = $PvpPassed -and $PvePassed -and $ResumePassed -and $InitialVisualPassed
        # Fail markers invalidate a run even when a previous phase printed PASS.
        if (Test-SmokeFailureLog $ServerText) { throw "Server reported a failure; see $ServerLog." }
        if ($IsRemote -and $RemoteActiveState -notin @('activating', 'active', 'reloading')) {
            throw "The owned remote Server unit is $RemoteActiveState at acceptance; see $ServerLog."
        }
        foreach ($Entry in $Started) {
            if (Test-SmokeFailureLog (Read-SmokeLog $Entry.Log)) { throw "$($Entry.Name) reported a failure; see $($Entry.Log)." }
        }
        if ($Succeeded) {
            # Acceptance uses fresh remote evidence and live owned processes,
            # not a cached active state from before the final marker arrived.
            Update-RemoteSmokeLog -Force
            Assert-SmokeRunning
        }
        if (-not $Succeeded) {
            Assert-SmokeRunning
            Start-Sleep -Milliseconds $SmokeConfig.PollMilliseconds
        }
    }
}
catch {
    $Succeeded = $false
    $FailureReason = $_.Exception.Message
}
finally {
    if ($Succeeded) {
        try {
            # Recheck before intentionally stopping any process. A crash in the
            # last acceptance interval must not be misreported as our cleanup.
            Update-RemoteSmokeLog -Force
            Assert-SmokeRunning
        }
        catch {
            $Succeeded = $false
            $FailureReason = "Final live-process verification failed: $($_.Exception.Message)"
        }
    }
    foreach ($Entry in $Started) {
        $Current = Get-Process -Id $Entry.Id -ErrorAction SilentlyContinue
        if ($null -eq $Current) { continue }
        # Guard against PID reuse and never terminate a process at another path.
        try {
            if ($Current.StartTime.ToUniversalTime() -eq $Entry.StartedUtc -and
                [string]::Equals($Current.Path, $Entry.Executable, [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $Current.Id -Force -ErrorAction Stop
                if(-not $Entry.Process.WaitForExit(10000)){throw 'Owned peer did not exit within the bounded cleanup wait.'}
            }
        }
        catch {
            # A process can exit between lookup and inspection. Preserve reports
            # and never kill another process without a verified identity.
            if ($null -ne (Get-Process -Id $Entry.Id -ErrorAction SilentlyContinue)) {
                $Succeeded = $false
                if (-not $FailureReason) { $FailureReason = "Could not safely clean up $($Entry.Name), PID $($Entry.Id): $($_.Exception.Message)" }
            }
        }
    }
    if ($IsRemote -and $RemoteOwned) {
        $RemoteCleanupErrors = @()
        try {
            Update-RemoteSmokeLog -Force
            if ($RemoteActiveState -notin @('activating', 'active', 'reloading')) {
                throw "The owned remote Server is $RemoteActiveState before the requested stop."
            }
            if (Test-SmokeFailureLog (Read-SmokeLog $ServerLog)) {
                throw 'Remote Server final pre-stop log contains a failure or explicit OOM.'
            }
        }
        catch { $RemoteCleanupErrors += "Before-stop log: $($_.Exception.Message)" }
        try {
            $null = Invoke-SmokeSsh ("set -eu; pvp_unit_load=`$(systemctl show '$RemoteUnit' --property=LoadState --value 2>/dev/null || true); if test `"`$pvp_unit_load`" != not-found; then systemctl stop '$RemoteUnit'; fi")
        }
        catch { $RemoteCleanupErrors += "Exact-unit stop: $($_.Exception.Message)" }
        try {
            # Keep this run's server/journal evidence even after a crash or
            # timeout. No other unit, package or remote log is removed.
            Update-RemoteSmokeLog -Force
            $Journal = Invoke-SmokeSsh ("journalctl --unit='$RemoteUnit' --no-pager --output=cat")
            [IO.File]::WriteAllText((Join-Path $OutputDirectory 'Server.journal.log'), $Journal, [Text.UTF8Encoding]::new($false))
            # SIGTERM/status=143 or systemd's generic exit-code after our exact
            # stop is expected. Only gameplay/fatal assertions or explicit OOM
            # diagnostics invalidate the collected shutdown tail.
            $FinalServerText = Read-SmokeLog $ServerLog
            if ((Test-SmokeFailureLog $FinalServerText) -or (Test-SmokeFailureLog $Journal)) {
                throw 'Collected remote shutdown logs contain a failure, fatal assertion or explicit OOM.'
            }
        }
        catch {
            $RemoteCleanupErrors += "After-stop evidence: $($_.Exception.Message)"
        }
        if ($RemoteCleanupErrors.Count -gt 0) {
            $Succeeded = $false
            $CleanupFailure = "Could not completely collect/stop the owned remote unit ${RemoteUnit}: $($RemoteCleanupErrors -join '; ')"
            if ($FailureReason) { $FailureReason += '; ' + $CleanupFailure } else { $FailureReason = $CleanupFailure }
        }
    }
    $LatencyEvidence = $null
    if (Test-Path -LiteralPath $ServerLog -PathType Leaf) {
        try { $LatencyEvidence = & (Join-Path $PSScriptRoot 'MeasureFpsShotLatency.ps1') -RunDirectory $OutputDirectory -SameHostUtc:(!$IsRemote) | ConvertFrom-Json }
        catch { $LatencyEvidence = @{ Error=$_.Exception.Message; Scope='Latency evidence unavailable; never substitute zero latency.' } }
    }
    if ($DeepChecks -and $null -ne $LatencyEvidence) {
        $ActivePredictionClients = @($LatencyEvidence.Clients | Where-Object { $_.Predicted -gt 0 })
        $Evidence['Deep.AmmoPredictedAndSettled'] = $ActivePredictionClients.Count -gt 0 -and @($ActivePredictionClients | Where-Object {
            $null -eq $_.PredictionEvidence -or -not $_.PredictionEvidence.Clean -or
            $_.PredictionEvidence.ImmediatePredictedDeductions -lt 1 -or $_.PredictionEvidence.SettledAuthoritativeAmmoEvents -lt 1
        }).Count -eq 0
        $Evidence['Deep.ReloadPredictedAndPromoted'] = @($ActivePredictionClients | Where-Object {
            $null -ne $_.PredictionEvidence -and $_.PredictionEvidence.Clean -and $_.PredictionEvidence.ConfirmedReloadsWithoutReplay -gt 0
        }).Count -gt 0
        if (-not $Evidence['Deep.AmmoPredictedAndSettled'] -or -not $Evidence['Deep.ReloadPredictedAndPromoted']) {
            $Succeeded = $false
            $FailureReason = ($FailureReason + ' Owning-client ammo deduction/settlement or predicted reload promotion evidence is missing or inconsistent.').Trim()
        }
    }
    $InputVerificationError = $null
    try { $InputsUnchanged = $InputFingerprintJson -ceq ((@(Get-SmokeBuildFingerprint)) | ConvertTo-Json -Depth 3 -Compress) }
    catch { $InputsUnchanged = $false; $InputVerificationError = $_.Exception.Message }
    if (-not $InputsUnchanged) {
        $Succeeded = $false
        $FailureReason = ($FailureReason + ' Build/source/configuration inputs changed during the run.').Trim()
    }
    $TraceArtifacts = @()
    foreach ($TraceName in $TracePaths.Keys) {
        $TracePath = $TracePaths[$TraceName]
        $TraceArtifacts += Get-SmokeTraceArtifact $TraceName $TracePath
    }
    if ($EnableNetTrace -and @($TraceArtifacts | Where-Object { -not $_.Complete }).Count -gt 0) {
        $Succeeded = $false
        $FailureReason = ($FailureReason + ' Requested native trace artifacts are missing or empty.').Trim()
    }
    $Report = [pscustomobject]@{
        Passed = $Succeeded; Failure = $FailureReason
        AcceptanceMode = if ($WeakPolicyCorrectness) { 'WeakPolicyCorrectness' } else { 'StrictExperience' }
        StrictZeroUnexpectedRejectionsPassed = $Evidence['Deep.NoUnexpectedShotRejections']
        WeakPolicyEvidence = if ($WeakPolicyCorrectness) { $PolicyEvidence } else { $null }
        ExecutionMode = $ExecutionMode
        ServerMode = $ServerMode; ServerAddress = $ConnectAddress
        DeepChecks = $DeepChecks
        IncludePve = $IncludePve
        IncludeReconnect = $IncludeReconnect
        ReconnectMode = $ReconnectMode
        RenderedClients = [bool]$RenderedClients
        VisibleClients = [bool]$VisibleClients
        MovingHit = [bool]$MovingHit
        TargetLatencyCompensation = $TargetCompensationReadback
        TargetLatencyCompensationRequested = $TargetLatencyCompensation
        MovingHitStatus = $MovingHitEvidence.Status
        MovingHitEvidence = $MovingHitEvidence
        MovingHitScope = if ($MovingHit) { 'Explicit development-only zero-spread single-shot scenario: standing shooter and sprinting laterally moving target. Requires the same five-part identity across local aim, authoritative hit and client health reduction. Requested target-latency policy is read back from server REWIND records. Not a hit-rate benchmark or full render-time reconstruction.' } else { 'NotRequested; ordinary smoke uses authored production spread.' }
        ShotAccounting = $ShotLedgers
        ShotLatency = $LatencyEvidence
        MemoryAccounting = $MemoryAccounting
        ClientRenderer = if ($VisibleClients) { 'D3D12Windowed' } elseif ($RenderedClients) { 'D3D12Offscreen' } else { 'NullRHI' }
        ServerRenderer = 'NullRHI'
        VisualArtifacts = @($VisualCaptures.Values | Sort-Object Client)
        RenderedRecoveryVerified = [bool]($RenderedClients -and $IncludeReconnect -and $Succeeded -and $Evidence['Reconnect.RenderedCapture'])
        VisualQualityReviewed = $false
        ReconnectStage = $ReconnectStage
        ReconnectMethod = if ($IncludeReconnect -and $ReconnectMode -eq 'TransportOutage') { 'SameOwnedProcessRealConnectionPacketOutageAndAutomaticRetry' } elseif ($IncludeReconnect) { 'OwnedProcessHardStopAndRestartWithPrivateUserDirectory' } else { 'NotRequested' }
        AutomaticRetryTransportVerified = [bool]($Succeeded -and $ReconnectMode -eq 'TransportOutage' -and $Evidence['Reconnect.AutomaticRetryTransport'])
        ClientRoleMap = $ClientRoleMap
        ReconnectClient = $ReconnectClientName
        ReconnectResumeId = $ReconnectResumeId
        ReconnectTargetRole = if($ReconnectClientName -and $ReconnectClientName -eq $ClientRoleMap.ActiveClient){'Active'}elseif($ReconnectClientName -and $ReconnectClientName -eq $ClientRoleMap.ObserverClient){'Observer'}else{'Unknown'}
        OriginalReconnectClientPid = $OriginalReconnectClientPid
        OriginalPhaseAcceptance = @{Pvp=$PvpPassed;Pve=$PvePassed;InitialVisual=$InitialVisualPassed;Resume=$ResumePassed}
        HardDisconnectUtc = $ReconnectStoppedUtc
        InputsUnchanged = $InputsUnchanged
        InputVerificationError = $InputVerificationError
        FingerprintScope = 'Before/after stability of listed source/configuration/map/module inputs, not proof that an existing DLL was built from those sources. Build provenance is separate.'
        InputFingerprint = $InputFingerprint
        NativeTrace = @{ Enabled = [bool]$EnableNetTrace; Verbosity = $NetTraceVerbosity; MaxTotalMiB = $TraceMaxMiB
            Artifacts = $TraceArtifacts; DiagnosticOnly = $true
            Interpretation = 'Raw native trace evidence; packet/bunch to Shot identity correlation still requires analysis. Diagnostic overhead is not a no-trace performance baseline.' }
        RemoteUnit = if ($IsRemote) { $RemoteUnit } else { $null }
        RemoteLogDirectory = if ($IsRemote) { $RemoteRunDirectory } else { $null }
        ServerExecutable = $ServerPath; ClientExecutable = $ClientPath
        ExpectedConfiguration = 'Development'
        PackageReceipts = $PackageReceipts
        ElapsedSeconds = [Math]::Round($RunClock.Elapsed.TotalSeconds, 2)
        Map = $Map; GameMode = $SmokeConfig.GameMode; Port = $Port
        Network = @{ OutgoingLagMs = $PacketLagMs; LagVarianceMs = $PacketLagVarianceMs; LossPercent = $PacketLossPercent }
        Evidence = $Evidence
        Logs = @($Started | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Path = $_.Log; Executable = $_.Executable } }) +
            $(if ($IsRemote) { @([pscustomobject]@{ Name = 'Server'; Path = $ServerLog; Executable = $RemoteExecutable }) } else { @() })
        Scope = if ($RenderedClients -and $IsRemote) { 'Cross-machine functional smoke against a hash/receipt-verified Linux Server, with real rendered-client readiness and PNG artifacts; pixel quality requires separate inspection; not capacity or performance acceptance' } elseif ($RenderedClients) { 'Local functional smoke with real rendered-client readiness and PNG artifacts; pixel quality requires separate inspection; not cross-machine, capacity or performance acceptance' } elseif ($IsRemote) { 'Cross-machine NullRHI functional smoke against a hash/receipt-verified Linux Server; not visual, capacity or performance acceptance' } else { 'NullRHI local functional network smoke; not cross-machine, visual, capacity or performance acceptance' }
    }
    $ReportJson = $Report | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'Report.json'), $ReportJson, [Text.UTF8Encoding]::new($false))
    Write-Output $ReportJson
}
if (-not $Succeeded) { throw $FailureReason }
