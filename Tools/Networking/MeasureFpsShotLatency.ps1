<#
Read-only shot diagnosis. Prediction-to-ACK is a same-client wall interval, NOT
RTT. Cross-process UTC intervals require explicit SameHostUtc. Server world-time
intervals are subtracted only within the same shot. Missing evidence stays null.
This diagnostic never changes gameplay timing budgets or acceptance gates.
#>
[CmdletBinding()]
param([string]$RunDirectory, [switch]$SameHostUtc, [switch]$SelfTest)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture

function Parse-Stamp([string]$line) {
    if ($line -notmatch '^\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]') { return $null }
    $stamp = [datetime]::MinValue
    if (-not [datetime]::TryParseExact($Matches[1], 'yyyy.MM.dd-HH.mm.ss:fff', $culture,
        [Globalization.DateTimeStyles]::None, [ref]$stamp)) { return $null }
    return $stamp
}
function Text-Field([string]$line, [string]$field) {
    if ($line -match ('(?:^|\s)' + [regex]::Escape($field) + '=([^\s]+)')) { return $Matches[1] }
    return $null
}
function Number-Field([string]$line, [string]$field) {
    $token = Text-Field $line $field
    $value = 0.0
    if ($null -ne $token -and [double]::TryParse($token, [Globalization.NumberStyles]::Float, $culture, [ref]$value) -and
        -not [double]::IsNaN($value) -and -not [double]::IsInfinity($value)) { return $value }
    return $null
}
function Identity([string]$line) {
    $parts = @()
    foreach ($field in @('WeaponNetGuid', 'Generation', 'Session', 'ShotId')) {
        $token = Text-Field $line $field
        $value = [uint64]0
        if ($null -eq $token -or $token -notmatch '^\d+$' -or -not [uint64]::TryParse($token, [ref]$value) -or
            $value -eq 0 -or ($field -ne 'WeaponNetGuid' -and $value -gt [uint32]::MaxValue)) { return $null }
        $parts += $value.ToString($culture)
    }
    return $parts -join '/'
}
function Percentile([object[]]$values, [double]$fraction) {
    $valid = @($values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($valid.Count -eq 0) { return $null }
    return [math]::Round([double]$valid[[math]::Max(0, [math]::Ceiling($valid.Count * $fraction) - 1)], 3)
}
function Result-Field([string]$line) {
    $value = Number-Field $line 'Result'
    if ($null -ne $value -and $value -eq [math]::Floor($value) -and $value -ge 0 -and $value -le 10) { return $value }
    return $null
}
function Add-ShotEvent([hashtable]$table, [string]$key, [object]$event) {
    if (-not $table.ContainsKey($key)) { $table[$key] = [Collections.Generic.List[object]]::new() }
    $table[$key].Add($event)
}
function Get-OnlyEvent([object[]]$events) {
    if ($events.Count -eq 1) { return $events[0] }
    return $null
}
function Elapsed-Ms($start, $end, [double]$scale = 1) {
    if ($null -eq $start -or $null -eq $end) { return $null }
    $delta = if ($start -is [datetime] -and $end -is [datetime]) { ($end - $start).TotalMilliseconds } else { ($end - $start) * $scale }
    # Invalid ordering is missing evidence, not a zero/negative latency sample.
    if ($delta -lt 0) { return $null }
    return [math]::Round($delta, 3)
}
function Client-Interval($startEvent, $endEvent) {
    $start = Number-Field $startEvent.Line 'LocalSeconds'; $end = Number-Field $endEvent.Line 'LocalSeconds'
    if ($null -ne $start -and $null -ne $end) {
        return [pscustomobject]@{Ms=(Elapsed-Ms $start $end 1000);Source='SameClientMonotonicSeconds'}
    }
    return [pscustomobject]@{Ms=(Elapsed-Ms $startEvent.Stamp $endEvent.Stamp);Source='LegacySameClientLogUtc'}
}
function Get-LatencyEvidence([string[]]$ServerLines, [hashtable]$ClientLines, [bool]$AllowSameHostUtc) {
    $server = @{}; $invalidServer = 0
    foreach ($line in $ServerLines) {
        if ($line -notmatch '\bPVP_SMOKE (SERVER_SHOT_ACCEPT|SERVER_SHOT_REJECT|SHOT_TIME_REJECT|SHOT_ADMISSION|SHOT_EXECUTION|SHOT_ACK_SEND)\b') { continue }
        $kind = $Matches[1]; $key = Identity $line
        if (-not $key) { $invalidServer++; continue }
        Add-ShotEvent $server $key ([pscustomobject]@{Kind=$kind;Line=$line;Stamp=(Parse-Stamp $line)})
    }
    $clients = @()
    foreach ($client in ($ClientLines.Keys | Sort-Object)) {
        $events = @{}; $clocks = @(); $invalidClient = 0
        foreach ($line in $ClientLines[$client]) {
            if ($line -match '\bPVP_SMOKE CLOCK_SAMPLE PASS\b') {
                $clocks += [pscustomobject]@{Line=$line;Stamp=(Parse-Stamp $line)}
            }
            if ($line -notmatch '\bPVP_SMOKE (CLIENT_SHOT_PRESENT|CLIENT_SHOT_SUBMIT|CLIENT_SHOT_ACK)\b') { continue }
            $kind = $Matches[1]; $key = Identity $line
            if (-not $key) { $invalidClient++; continue }
            Add-ShotEvent $events $key ([pscustomobject]@{Kind=$kind;Line=$line;Stamp=(Parse-Stamp $line)})
        }
        $rows = @(
            foreach ($key in ($events.Keys | Sort-Object)) {
                $shotEvents = @($events[$key]); $serverEvents = @($server[$key]) | Where-Object { $null -ne $_ }
                $presentations = @($shotEvents | Where-Object Kind -eq 'CLIENT_SHOT_PRESENT')
                if ($presentations.Count -eq 0) { continue } # Never invent a prediction for an orphan ACK.
                $presentation = Get-OnlyEvent $presentations
                $submits = @($shotEvents | Where-Object Kind -eq 'CLIENT_SHOT_SUBMIT')
                $acks = @($shotEvents | Where-Object Kind -eq 'CLIENT_SHOT_ACK')
                $terminalAcks = @($acks | Where-Object {
                    $code = Result-Field $_.Line
                    $null -ne $code -and $code -notin @(1,4)
                })
                $ackResults = @($terminalAcks | ForEach-Object { Number-Field $_.Line 'Result' } | Select-Object -Unique)
                $ackConflict = $ackResults.Count -gt 1
                $final = if ($ackResults.Count -eq 1) {
                    $terminalAcks | Sort-Object @{Expression={
                        $local=Number-Field $_.Line 'LocalSeconds'
                        if($null -ne $local){$local}else{[double]::PositiveInfinity}
                    }},Stamp | Select-Object -First 1
                } else { $null }
                $verdicts = @($serverEvents | Where-Object {
                    $_.Kind -in @('SERVER_SHOT_ACCEPT','SERVER_SHOT_REJECT') -and (Result-Field $_.Line) -notin @($null,1,4)
                })
                $verdict = Get-OnlyEvent $verdicts
                $admissions = @($serverEvents | Where-Object Kind -eq 'SHOT_ADMISSION')
                $executions = @($serverEvents | Where-Object Kind -eq 'SHOT_EXECUTION')
                $sends = @($serverEvents | Where-Object Kind -eq 'SHOT_ACK_SEND')
                $timeRejects = @($serverEvents | Where-Object Kind -eq 'SHOT_TIME_REJECT')
                $admission = Get-OnlyEvent $admissions; $execution = Get-OnlyEvent $executions
                $send = Get-OnlyEvent $sends; $timeDiagnostic = Get-OnlyEvent $timeRejects
                $clock = $null
                if ($presentation -and $presentation.Stamp) {
                    $clock = $clocks | Where-Object { $_.Stamp -and $_.Stamp -le $presentation.Stamp } | Sort-Object Stamp | Select-Object -Last 1
                }
                $firstSubmit = $null; $attempts = @(); $invalidAttempts = 0; $maxAttempts = @()
                foreach ($submit in $submits) {
                    $attempt = Number-Field $submit.Line 'Attempt'
                    if ($null -eq $attempt -or $attempt -lt 1 -or $attempt -ne [math]::Floor($attempt)) { $invalidAttempts++; continue }
                    $attempts += $attempt
                    if ($attempt -eq 1 -and $null -eq $firstSubmit) { $firstSubmit = $submit }
                    $limit = Number-Field $submit.Line 'MaxAttempts'
                    if ($null -ne $limit) { $maxAttempts += $limit }
                }
                $limits = @($maxAttempts | Select-Object -Unique)
                $retryBound = if ($limits.Count -eq 1 -and $limits[0] -ge 1 -and $limits[0] -eq [math]::Floor($limits[0])) { $limits[0] } else { $null }
                $retryValid = $null
                if ($submits.Count -gt 0 -and $null -ne $retryBound) {
                    $ordered = @($attempts | Sort-Object -Unique)
                    $retryValid = $invalidAttempts -eq 0 -and $maxAttempts.Count -eq $submits.Count -and $ordered.Count -eq $submits.Count -and $ordered[0] -eq 1
                    if ($retryValid) {
                        for ($index=0; $index -lt $ordered.Count; $index++) {
                            if ($ordered[$index] -ne $index+1 -or $ordered[$index] -gt $retryBound) { $retryValid = $false; break }
                        }
                    }
                }
                $fireTime = Number-Field $verdict.Line 'FireTime'; $serverTime = Number-Field $verdict.Line 'ServerTime'
                $admissionTime = Number-Field $admission.Line 'ServerTime'; $executionTime = Number-Field $execution.Line 'ServerTime'
                $rtt = Number-Field $clock.Line 'RTT'
                $queueWait = Number-Field $execution.Line 'QueueWaitMs'
                $queueSource = if ($null -ne $queueWait) { 'ExplicitServerQueueWaitMs' } else { $null }
                if ($null -ne $queueWait -and $queueWait -lt 0) { $queueWait=$null; $queueSource=$null }
                $derivedQueue = Elapsed-Ms $admissionTime $executionTime 1000
                if ($null -eq $queueWait -and $null -ne $derivedQueue) { $queueWait=$derivedQueue; $queueSource='SameServerWorldTimeDifference' }
                $predictionToAck = Client-Interval $presentation $final
                $predictionToSubmit = Client-Interval $presentation $firstSubmit
                $submitToPresentation = Client-Interval $firstSubmit $presentation
                $submitTimingStatus = if (-not $firstSubmit -or -not $presentation) { 'MissingInstrumentation' }
                    elseif ($null -ne $submitToPresentation.Ms -and $null -eq $predictionToSubmit.Ms) { 'ConstructionPrecedesPresentationNotApplicable' }
                    elseif ($null -ne $predictionToSubmit.Ms) { 'MeasuredPredictionToConstruction' } else { 'InvalidOrMissingClockEvidence' }
                $issues = @()
                if ($presentations.Count -ne 1) { $issues += 'DuplicatePrediction' }
                if ($ackConflict) { $issues += 'ConflictingTerminalAcknowledgements' }
                if ($verdicts.Count -gt 1) { $issues += 'MultipleTerminalServerVerdicts' }
                if ($admissions.Count -gt 1 -or $executions.Count -gt 1 -or $sends.Count -gt 1 -or $timeRejects.Count -gt 1) { $issues += 'AmbiguousServerPhaseEvents' }
                if ($predictionToAck.Source -eq 'LegacySameClientLogUtc' -and $final -and $presentation -and $final.Stamp -and $presentation.Stamp -and $final.Stamp -lt $presentation.Stamp) { $issues += 'AcknowledgementBeforePredictionTimestamp' }
                if ($null -ne $admissionTime -and $null -ne $executionTime -and $executionTime -lt $admissionTime) { $issues += 'ExecutionBeforeAdmissionServerTime' }
                if ($final -and $verdict -and (Number-Field $final.Line 'Result') -ne (Number-Field $verdict.Line 'Result')) { $issues += 'ServerClientVerdictMismatch' }
                if ($retryValid -eq $false) { $issues += 'InvalidOrExceededSubmitAttemptBound' }
                if (@($acks | Where-Object { $null -eq (Result-Field $_.Line) }).Count -gt 0) { $issues += 'InvalidAcknowledgementResult' }
                if ($null -ne $fireTime -and $null -ne $serverTime) { $verdictAge = [math]::Round(($serverTime-$fireTime)*1000,3) } else { $verdictAge=$null }
                [pscustomobject]@{
                    Identity=$key;Result=$(if($final){Number-Field $final.Line 'Result'}else{$null});FinalAck=($null -ne $final)
                    PredictionCount=$presentations.Count;TerminalAckCount=$terminalAcks.Count;ServerTerminalVerdictCount=$verdicts.Count
                    PredictionToAckMs=$predictionToAck.Ms;PredictionToAckSource=$predictionToAck.Source
                    PredictionToSubmitMs=$predictionToSubmit.Ms;PredictionToSubmitSource=$predictionToSubmit.Source
                    SubmitToPresentationMs=$submitToPresentation.Ms;SubmitToPresentationSource=$submitToPresentation.Source;SubmitTimingStatus=$submitTimingStatus
                    PredictionToAdmissionSameHostWallMs=$(if($AllowSameHostUtc){Elapsed-Ms $presentation.Stamp $admission.Stamp}else{$null})
                    SubmitToAdmissionSameHostWallMs=$(if($AllowSameHostUtc){Elapsed-Ms $firstSubmit.Stamp $admission.Stamp}else{$null})
                    AdmissionToExecutionMs=$queueWait;AdmissionToExecutionSource=$queueSource
                    ExecutionToAckSameHostWallMs=$(if($AllowSameHostUtc){Elapsed-Ms $execution.Stamp $final.Stamp}else{$null})
                    ServerAckSendToClientAckSameHostWallMs=$(if($AllowSameHostUtc){Elapsed-Ms $send.Stamp $final.Stamp}else{$null})
                    ServerVerdictAgeMs=$verdictAge
                    AdmissionAgeMs=(Number-Field $admission.Line 'AgeMs');AdmissionAllowedAgeMs=(Number-Field $admission.Line 'AllowedAgeMs')
                    ExecutionAgeMs=(Number-Field $execution.Line 'AgeMs');ExecutionAllowedAgeMs=(Number-Field $execution.Line 'AllowedAgeMs')
                    TimeRejectionStage=(Text-Field $timeDiagnostic.Line 'Stage')
                    MeasuredRejectAgeMs=(Number-Field $timeDiagnostic.Line 'AgeMs');AllowedRejectAgeMs=(Number-Field $timeDiagnostic.Line 'AllowedAgeMs')
                    TimeRejectReason=(Text-Field $timeDiagnostic.Line 'Reason');ServerPingMs=(Number-Field $timeDiagnostic.Line 'PingMs')
                    PredictionToTimeRejectionSameHostWallMs=$(if($AllowSameHostUtc){Elapsed-Ms $presentation.Stamp $timeDiagnostic.Stamp}else{$null})
                    AdmissionPendingBefore=(Number-Field $admission.Line 'PendingBefore');AdmissionPendingAfter=(Number-Field $admission.Line 'PendingAfter')
                    ExecutionPendingBefore=(Number-Field $execution.Line 'PendingBefore');ExecutionPendingAfter=(Number-Field $execution.Line 'PendingAfter')
                    AdmissionPlannedDelayMs=(Number-Field $admission.Line 'QueueDelayMs');VerdictPending=(Number-Field $verdict.Line 'Pending')
                    AdmissionFrameDeltaMs=(Number-Field $admission.Line 'FrameDeltaMs');ExecutionFrameDeltaMs=(Number-Field $execution.Line 'FrameDeltaMs')
                    ConnectionInLostTotal=(Number-Field $timeDiagnostic.Line 'InLostTotal');ConnectionOutLostTotal=(Number-Field $timeDiagnostic.Line 'OutLostTotal')
                    NearestPriorClientClockRttMs=$(if($null -ne $rtt){[math]::Round($rtt*1000,3)}else{$null})
                    NearestPriorClockUncertaintySeconds=(Number-Field $clock.Line 'Uncertainty');NearestPriorClockOffsetSeconds=(Number-Field $clock.Line 'Offset')
                    NearestPriorClockSequence=(Number-Field $clock.Line 'Sequence');PriorClockSampleAgeMs=(Elapsed-Ms $clock.Stamp $presentation.Stamp)
                    ObservedSubmitAttempts=$submits.Count;RetryAttemptsObserved=$(if($attempts.Count){[math]::Max(0,($attempts | Measure-Object -Maximum).Maximum-1)}else{$null})
                    DeclaredMaxSubmitAttempts=$retryBound;SubmitAttemptBoundValid=$retryValid
                    ObservedRetryDelaysMs=@($submits | Where-Object { (Number-Field $_.Line 'Attempt') -gt 1 } | ForEach-Object { Number-Field $_.Line 'RetryDelayMs' })
                    ConfirmedPerShotRetransmission=$null;ReliableHeadOfLineBlockingConfirmed=$null;ServerFrameStallCausalityConfirmed=$null
                    DiagnosticIssues=$issues
                }
            }
        )
        $count=$rows.Count; $accepted=@($rows | Where-Object Result -eq 0).Count; $missing=@($rows | Where-Object { -not $_.FinalAck }).Count
        $rejected=$count-$accepted-$missing
        $orphans=@($events.Keys | Where-Object { @($events[$_] | Where-Object Kind -eq 'CLIENT_SHOT_PRESENT').Count -eq 0 }).Count
        $clients += [pscustomobject]@{
            Client=$client;Predicted=$count;Accepted=$accepted;Rejected=$rejected;MissingFinalAck=$missing
            InvalidIdentityEvents=$invalidClient;OrphanShotIdentities=$orphans;ShotsWithDiagnosticIssues=@($rows | Where-Object { $_.DiagnosticIssues.Count -gt 0 }).Count
            AdmissionTimeRejections=@($rows | Where-Object TimeRejectionStage -eq 'Admission').Count
            ExecutionTimeRejections=@($rows | Where-Object TimeRejectionStage -eq 'Execution').Count
            TimeRejectionsWithUnknownStage=@($rows | Where-Object { $_.Result -eq 5 -and $_.TimeRejectionStage -notin @('Admission','Execution') }).Count
            RejectionPercent=$(if($count){[math]::Round(100*$rejected/$count,3)}else{$null})
            TimeoutPercent=$(if($count){[math]::Round(100*$missing/$count,3)}else{$null})
            PredictionToAckP50Ms=(Percentile @($rows | ForEach-Object PredictionToAckMs) 0.5)
            PredictionToAckP95Ms=(Percentile @($rows | ForEach-Object PredictionToAckMs) 0.95)
            PredictionToAckMaxMs=(Percentile @($rows | ForEach-Object PredictionToAckMs) 1)
            AdmissionToExecutionP95Ms=(Percentile @($rows | ForEach-Object AdmissionToExecutionMs) 0.95)
            PredictionToAdmissionSamples=@($rows | Where-Object { $null -ne $_.PredictionToAdmissionSameHostWallMs }).Count
            AdmissionToExecutionSamples=@($rows | Where-Object { $null -ne $_.AdmissionToExecutionMs }).Count
            ExecutionToAckSamples=@($rows | Where-Object { $null -ne $_.ExecutionToAckSameHostWallMs }).Count
            ExplicitSubmitInstrumentationShots=@($rows | Where-Object ObservedSubmitAttempts -gt 0).Count
            RetryBoundViolations=@($rows | Where-Object SubmitAttemptBoundValid -eq $false).Count
            Shots=$rows
        }
    }
    return [pscustomobject]@{
        SchemaVersion=2;SameHostUtcComparisonEnabled=$AllowSameHostUtc;InvalidServerIdentityEvents=$invalidServer;Clients=$clients
        Scope='Prediction-to-ACK is same-client monotonic latency when LocalSeconds exists, otherwise legacy log UTC; not RTT. LocalSeconds is never subtracted across processes. CLIENT_SHOT_SUBMIT is request construction complete before the same-stack reliable GAS call, not a socket/NIC send timestamp. It can precede PRESENT; negative prediction-to-submit stays null. Cross-process wall intervals require explicitly same-host logs and include dispatch/scheduling, not isolated one-way network delay. Admission-to-execution uses explicit queue duration or same-server world time. Rejected admission does not invent an admitted event. Verdict age is signed and includes clock uncertainty/execution wait. FrameDelta is only co-occurring frame length, not stall causality. Null means absent, ambiguous or invalid evidence. Attempt=MaxAttempts=1 means no app retry observed. App attempts are not packet retransmissions; bursts cannot prove reliable HOL. MissingFinalAck/TimeoutPercent is end-of-run absence, not an instrumented timeout. No gameplay/strict-experience gate is evaluated here.'
    }
}

if ($SelfTest) {
    $checks=[Collections.Generic.List[string]]::new()
    function Assert-Latency([string]$Name,[bool]$Condition) { if(-not $Condition){throw "Shot latency SelfTest failed: $Name"}; $checks.Add($Name) }
    $id='ShotId=1 Session=2 Generation=3 WeaponNetGuid=7'
    $present="[2026.10.19-00.00.00:100] PVP_SMOKE CLIENT_SHOT_PRESENT $id"
    $ack="[2026.10.19-00.00.00:400] PVP_SMOKE CLIENT_SHOT_ACK $id Result=0"
    $verdict="[2026.10.19-00.00.00:300] PVP_SMOKE SERVER_SHOT_ACCEPT $id Result=0 FireTime=10.0 ServerTime=10.2 Pending=0"
    $legacy=Get-LatencyEvidence @($verdict) @{Client1=@($present,$ack)} $false
    $shot=$legacy.Clients[0].Shots[0]
    Assert-Latency 'Legacy same-client prediction-to-ACK retained; not RTT' ($shot.PredictionToAckMs -eq 300 -and $shot.ServerVerdictAgeMs -eq 200)
    Assert-Latency 'Missing phases stay null; no HOL/retransmission fabricated' ($null -eq $shot.AdmissionToExecutionMs -and $null -eq $shot.ExecutionToAckSameHostWallMs -and $null -eq $shot.ConfirmedPerShotRetransmission -and $null -eq $shot.ReliableHeadOfLineBlockingConfirmed)
    $admit="[2026.10.19-00.00.00:250] PVP_SMOKE SHOT_ADMISSION $id ServerTime=10.15 AgeMs=150 AllowedAgeMs=220 PendingBefore=0 PendingAfter=1 QueueDelayMs=50 FrameDeltaMs=16"
    $exec="[2026.10.19-00.00.00:300] PVP_SMOKE SHOT_EXECUTION $id ServerTime=10.2 QueueWaitMs=50 AgeMs=200 AllowedAgeMs=300 FrameDeltaMs=50"
    $submit="[2026.10.19-00.00.00:110] PVP_SMOKE CLIENT_SHOT_SUBMIT $id Attempt=1 MaxAttempts=3 RetryDelayMs=0"
    $phases=Get-LatencyEvidence @($verdict,$exec,$admit) @{Client1=@($ack,$present,$submit)} $true
    $shot=$phases.Clients[0].Shots[0]
    Assert-Latency 'Out-of-order file events match identities and phase times' ($shot.PredictionToAdmissionSameHostWallMs -eq 150 -and $shot.AdmissionToExecutionMs -eq 50 -and $shot.ExecutionToAckSameHostWallMs -eq 100)
    Assert-Latency 'Frame delta is not promoted to causal proof' ($shot.ExecutionFrameDeltaMs -eq 50 -and $null -eq $shot.ServerFrameStallCausalityConfirmed)
    $remote=Get-LatencyEvidence @($verdict,$exec,$admit) @{Client1=@($present,$ack)} $false
    Assert-Latency 'Remote phases keep only same-server queue timing' ($remote.Clients[0].Shots[0].AdmissionToExecutionMs -eq 50 -and $null -eq $remote.Clients[0].Shots[0].PredictionToAdmissionSameHostWallMs -and $null -eq $remote.Clients[0].Shots[0].ExecutionToAckSameHostWallMs)
    foreach ($stage in @('Admission','Execution')) {
        $reject="PVP_SMOKE SHOT_TIME_REJECT $id Stage=$stage Reason=Stale AgeMs=350 AllowedAgeMs=300"
        $r=Get-LatencyEvidence @($verdict.Replace('ACCEPT','REJECT').Replace('Result=0','Result=5'),$reject) @{Client1=@($present,$ack.Replace('Result=0','Result=5'))} $false
        Assert-Latency "$stage rejection remains rejection, not strict success" ($r.Clients[0].Rejected -eq 1 -and $r.Clients[0].Shots[0].TimeRejectionStage -eq $stage)
    }
    $pending=Get-LatencyEvidence @() @{Client1=@($present,$ack.Replace('Result=0','Result=1'),$ack.Replace('Result=0','Result=4'))} $false
    Assert-Latency 'Queued/duplicate ACKs are not terminal' ($pending.Clients[0].MissingFinalAck -eq 1 -and $null -eq $pending.Clients[0].Shots[0].PredictionToAckMs)
    $conflict=Get-LatencyEvidence @($verdict) @{Client1=@($present,$ack,$ack.Replace('Result=0','Result=5'))} $false
    Assert-Latency 'Conflicting terminal ACKs not silently overwritten' ($conflict.Clients[0].MissingFinalAck -eq 1 -and $conflict.Clients[0].Shots[0].DiagnosticIssues -contains 'ConflictingTerminalAcknowledgements')
    $duplicate=Get-LatencyEvidence @($verdict) @{Client1=@($present,$ack,$ack)} $false
    Assert-Latency 'Identical ACK repeat preserves one result' ($duplicate.Clients[0].Accepted -eq 1 -and $duplicate.Clients[0].Shots[0].PredictionToAckMs -eq 300)
    $early=Get-LatencyEvidence @($verdict) @{Client1=@($present,$ack.Replace(':400]',':050]'))} $false
    Assert-Latency 'Negative wall intervals stay null' ($null -eq $early.Clients[0].Shots[0].PredictionToAckMs -and $early.Clients[0].Shots[0].DiagnosticIssues -contains 'AcknowledgementBeforePredictionTimestamp')
    $bad=Get-LatencyEvidence @($verdict) @{Client1=@($present.Replace('WeaponNetGuid=7','WeaponNetGuid=0'),$ack.Replace('Generation=3','Generation=3.5'))} $false
    Assert-Latency 'Unstable/fractional identities cannot cross-join' ($bad.Clients[0].Predicted -eq 0 -and $bad.Clients[0].InvalidIdentityEvents -eq 2)
    $separate=Get-LatencyEvidence @($verdict,$verdict.Replace('Generation=3','Generation=4'),$verdict.Replace('Session=2','Session=3')) @{Client1=@($present,$ack,$present.Replace('Generation=3','Generation=4'),$ack.Replace('Generation=3','Generation=4'),$present.Replace('Session=2','Session=3'),$ack.Replace('Session=2','Session=3'))} $false
    Assert-Latency 'Equal ShotIds in other generations/sessions stay distinct' ($separate.Clients[0].Predicted -eq 3 -and $separate.Clients[0].Accepted -eq 3)
    $retry=$submit.Replace('Attempt=1','Attempt=2').Replace('RetryDelayMs=0','RetryDelayMs=80')
    $bounded=Get-LatencyEvidence @($verdict) @{Client1=@($present,$submit,$retry,$ack)} $false
    Assert-Latency 'Bounded app resubmit observable, not packet retransmission' ($bounded.Clients[0].Shots[0].SubmitAttemptBoundValid -eq $true -and $bounded.Clients[0].Shots[0].RetryAttemptsObserved -eq 1 -and $null -eq $bounded.Clients[0].Shots[0].ConfirmedPerShotRetransmission)
    $overflow=Get-LatencyEvidence @($verdict) @{Client1=@($present,$submit,$retry.Replace('Attempt=2','Attempt=4'),$ack)} $false
    Assert-Latency 'Exceeded/missing retry sequence flagged' ($overflow.Clients[0].RetryBoundViolations -eq 1)
    $missing=Get-LatencyEvidence @($verdict) @{Client1=@($present)} $false
    Assert-Latency 'Missing ACK has no invented timer/delay' ($missing.Clients[0].MissingFinalAck -eq 1 -and $null -eq $missing.Clients[0].PredictionToAckP95Ms)
    $ambiguous=Get-LatencyEvidence @($verdict,$admit,$admit,$exec) @{Client1=@($present,$ack)} $true
    Assert-Latency 'Ambiguous admission cannot yield cross-process timing' ($null -eq $ambiguous.Clients[0].Shots[0].PredictionToAdmissionSameHostWallMs -and $ambiguous.Clients[0].Shots[0].DiagnosticIssues -contains 'AmbiguousServerPhaseEvents')
    $monotonic=Get-LatencyEvidence @($verdict) @{Client1=@(($present+' LocalSeconds=100.0'),($ack.Replace(':400]',':050]')+' LocalSeconds=100.3'))} $false
    Assert-Latency 'Same-client monotonic time is preferred despite wall-clock correction' ($monotonic.Clients[0].Shots[0].PredictionToAckMs -eq 300 -and $monotonic.Clients[0].Shots[0].PredictionToAckSource -eq 'SameClientMonotonicSeconds')
    Assert-Latency 'Wall-clock correction is not a phase failure with valid monotonic time' ($monotonic.Clients[0].Shots[0].DiagnosticIssues.Count -eq 0)
    $prePresentation=Get-LatencyEvidence @($verdict) @{Client1=@(($submit.Replace(':110]',':090]')+' LocalSeconds=99.99'),($present+' LocalSeconds=100.0'),($ack+' LocalSeconds=100.3'))} $false
    $preShot=$prePresentation.Clients[0].Shots[0]
    Assert-Latency 'Build then reliable call then PRESENT is legal and measured in forward order' ($preShot.SubmitToPresentationMs -eq 10 -and $null -eq $preShot.PredictionToSubmitMs -and $preShot.SubmitTimingStatus -eq 'ConstructionPrecedesPresentationNotApplicable' -and $preShot.DiagnosticIssues.Count -eq 0)
    $invalidResult=Get-LatencyEvidence @($verdict) @{Client1=@($present,$ack.Replace('Result=0','Result=0.5'))} $false
    Assert-Latency 'Invalid result is not accepted as terminal ACK' ($invalidResult.Clients[0].MissingFinalAck -eq 1 -and $invalidResult.Clients[0].Shots[0].DiagnosticIssues -contains 'InvalidAcknowledgementResult')
    $negativeAge=Get-LatencyEvidence @($verdict.Replace('ServerTime=10.2','ServerTime=9.9')) @{Client1=@($present,$ack)} $false
    Assert-Latency 'Future-request signed verdict age is preserved, not clamped' ($negativeAge.Clients[0].Shots[0].ServerVerdictAgeMs -eq -100)
    [pscustomobject]@{Mode='SelfTest';Passed=$true;Checks=$checks.Count;Scenarios=@($checks);Scope='Offline synthetic production-parser checks; no UE, network, credentials or disk writes. Not gameplay acceptance.'} | ConvertTo-Json -Depth 4
    return
}
if ([string]::IsNullOrWhiteSpace($RunDirectory)) { throw 'RunDirectory is required unless -SelfTest is used.' }
$run=(Resolve-Path -LiteralPath $RunDirectory).Path
$clientLogs=@{}
foreach ($file in (Get-ChildItem -LiteralPath $run -Filter 'Client*.log' -File)) { $clientLogs[$file.BaseName]=@(Get-Content -LiteralPath $file.FullName) }
$report=Get-LatencyEvidence @(Get-Content -LiteralPath (Join-Path $run 'Server.log')) $clientLogs ([bool]$SameHostUtc)
$report | Add-Member -NotePropertyName Directory -NotePropertyValue $run
$report | ConvertTo-Json -Depth 9
