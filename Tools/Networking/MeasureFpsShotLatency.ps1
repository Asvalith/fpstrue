<#
Read-only shot accounting from a smoke evidence directory. UTC log timestamps
measure prediction-to-ACK wall latency on this host; they are not cross-machine
clock subtraction. ServerAge is the server's logged verdict time minus the
client's synchronized fire timestamp, including any server execution wait.
Missing threshold/packet diagnostics remain null, never reconstructed from a
client clock sample or labelled as confirmed retransmission.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$run = (Resolve-Path -LiteralPath $RunDirectory).Path
$culture = [Globalization.CultureInfo]::InvariantCulture
function Parse-Stamp([string]$line) {
    if ($line -notmatch '^\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]') { return $null }
    return [datetime]::ParseExact($Matches[1], 'yyyy.MM.dd-HH.mm.ss:fff', $culture)
}
function Identity([string]$line) {
    if ($line -match 'ShotId=(\d+) Session=(\d+) Generation=(\d+).*?WeaponNetGuid=(\d+)') {
        return ($Matches[4] + '/' + $Matches[3] + '/' + $Matches[2] + '/' + $Matches[1])
    }
    return $null
}
function Number-Field([string]$line,[string]$field) {
    if ($line -match ('\b' + [regex]::Escape($field) + '=(-?[\d.]+)')) { return [double]::Parse($Matches[1],$culture) }
    return $null
}
function Percentile([object[]]$values,[double]$fraction) {
    if ($values.Count -eq 0) { return $null }
    $sorted = @($values | Sort-Object)
    return [math]::Round([double]$sorted[[math]::Max(0,[math]::Ceiling($sorted.Count*$fraction)-1)],3)
}
$server = @{}; $admission = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $run 'Server.log'))) {
    if ($line -match 'PVP_SMOKE SHOT_TIME_REJECT\b') {
        $key = Identity $line
        if ($key) { $admission[$key] = $line }
    }
    if ($line -notmatch 'PVP_SMOKE SERVER_SHOT_(ACCEPT|REJECT)\b') { continue }
    $key = Identity $line
    if ($key) { $server[$key] = $line }
}
$clients = @()
foreach ($file in (Get-ChildItem -LiteralPath $run -Filter 'Client*.log' -File)) {
    $present = @{}; $ack = @{}; $clock = $null; $clockForShot = @{}
    foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
        if ($line -match 'PVP_SMOKE CLOCK_SAMPLE PASS') { $clock = $line }
        if ($line -notmatch 'PVP_SMOKE CLIENT_SHOT_(PRESENT|ACK)\b') { continue }
        $kind = $Matches[1]; $key = Identity $line
        if (-not $key) { continue }
        if ($kind -eq 'PRESENT') {
            if (-not $present.ContainsKey($key)) { $present[$key] = Parse-Stamp $line; $clockForShot[$key] = $clock }
        } else { $ack[$key] = [pscustomobject]@{Stamp=(Parse-Stamp $line);Result=(Number-Field $line 'Result')} }
    }
    $rows = @(
        foreach ($key in ($present.Keys | Sort-Object)) {
            $verdict = $server[$key]; $final = $ack[$key]; $timeDiagnostic = $admission[$key]
            $fireTime = Number-Field $verdict 'FireTime'; $serverTime = Number-Field $verdict 'ServerTime'
            $rtt = Number-Field $clockForShot[$key] 'RTT'
            [pscustomobject]@{
                Identity=$key;Result=$(if($final){$final.Result}else{$null});FinalAck=($null -ne $final)
                PredictionToAckMs=$(if($final -and $present[$key]){[math]::Round(($final.Stamp-$present[$key]).TotalMilliseconds,3)}else{$null})
                ServerVerdictAgeMs=$(if($null -ne $fireTime -and $null -ne $serverTime){[math]::Round(($serverTime-$fireTime)*1000,3)}else{$null})
                TimeRejectionStage=$(if($timeDiagnostic -match '\bStage=(\w+)'){$Matches[1]}else{$null})
                MeasuredRejectAgeMs=(Number-Field $timeDiagnostic 'AgeMs')
                AllowedRejectAgeMs=(Number-Field $timeDiagnostic 'AllowedAgeMs')
                ServerPingMs=(Number-Field $timeDiagnostic 'PingMs')
                TimeRejectReason=$(if($timeDiagnostic -match '\bReason=(\w+)'){$Matches[1]}else{$null})
                ConnectionInLostTotal=(Number-Field $timeDiagnostic 'InLostTotal')
                ConnectionOutLostTotal=(Number-Field $timeDiagnostic 'OutLostTotal')
                NearestPriorClientClockRttMs=$(if($null -ne $rtt){[math]::Round($rtt*1000,3)}else{$null})
                NearestPriorClockUncertaintySeconds=(Number-Field $clockForShot[$key] 'Uncertainty')
                NearestPriorClockOffsetSeconds=(Number-Field $clockForShot[$key] 'Offset')
                NearestPriorClockSequence=(Number-Field $clockForShot[$key] 'Sequence')
                ConfirmedPerShotRetransmission=$null
            }
        }
    )
    $count = $rows.Count
    $accepted = @($rows | Where-Object Result -eq 0).Count
    $missing = @($rows | Where-Object { -not $_.FinalAck }).Count
    $rejected = $count-$accepted-$missing
    $clients += [pscustomobject]@{
        Client=$file.BaseName;Predicted=$count;Accepted=$accepted;Rejected=$rejected;MissingFinalAck=$missing
        RejectionPercent=$(if($count){[math]::Round(100*$rejected/$count,3)}else{$null})
        TimeoutPercent=$(if($count){[math]::Round(100*$missing/$count,3)}else{$null})
        PredictionToAckP50Ms=(Percentile @($rows | Where-Object FinalAck | ForEach-Object PredictionToAckMs) 0.5)
        PredictionToAckP95Ms=(Percentile @($rows | Where-Object FinalAck | ForEach-Object PredictionToAckMs) 0.95)
        PredictionToAckMaxMs=(Percentile @($rows | Where-Object FinalAck | ForEach-Object PredictionToAckMs) 1.0)
        Shots=$rows
    }
}
[pscustomobject]@{
    Directory=$run;Clients=$clients
    Scope='Same-host UTC prediction-to-ACK latency, not remote relative clocks. Server verdict age includes execution wait. Per-shot retransmission is not measured by these logs; burst delivery alone cannot prove HOL. Null AllowedRejectAge/ServerPing means the original log did not record them. TimeRejectionStage distinguishes admission from queued execution. MissingFinalAck is end-of-run absence, not an instrumented request timer.'
} | ConvertTo-Json -Depth 8
