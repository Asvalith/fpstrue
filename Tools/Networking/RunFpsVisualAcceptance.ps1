<#
Two real rendering clients use the ordinary multiplayer entry, walk to the map's
real pickups, then exchange short fire/reload/aim/sprint actions through the
production Character and GAS inputs. No smoke auto-equipment, direct damage,
teleport, desktop control, or test-only gameplay RPC is used.
The script checks state and PNG artifacts. A person must still inspect the
images; it does not certify animation quality, audio, capacity or performance.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:\ueprojrct\fpstrue_safe2',
    [string]$EngineRoot = 'E:\program\ue554\UE_5.5',
    [string]$Map = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked',
    [ValidateRange(1,65535)][int]$Port = 17841,
    [ValidateRange(30,600)][int]$TimeoutSeconds = 180,
    [ValidateRange(0,1000)][int]$PacketLagMs = 0,
    [ValidateRange(0,1000)][int]$PacketLagVarianceMs = 0,
    [ValidateRange(0,100)][int]$PacketLossPercent = 0,
    [ValidateRange(1,2)][int]$FirstClient = 1,
    [ValidateRange(1,3)][int]$Round = 1,
    [ValidateRange(0,160)][int]$EnemyPopulation = 0,
    [switch]$FullGameplay,
    [switch]$FacingReview,
    [switch]$FootIKReview,
    [ValidateRange(1,240)][int]$FacingStartServerTime = 150,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if ($Map -notmatch '^/Game/[A-Za-z0-9_/]+$') { throw 'Map must be a package name without travel options.' }
if ($FullGameplay -and $EnemyPopulation -ne 1) { throw 'FullGameplay requires the explicit one-enemy functional profile; do not confuse it with capacity acceptance.' }
if ($FullGameplay -and $FacingReview) { throw 'Facing review and full death/respawn review require separate runs.' }
$project = Join-Path $ProjectRoot 'fpstrue.uproject'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$module = Join-Path $ProjectRoot 'Binaries\Win64\UnrealEditor-fpstrue.dll'
$mapFile = Join-Path $ProjectRoot ('Content/' + $Map.Substring(6) + '.umap')
$run = Join-Path $ProjectRoot ('Saved\Networking\NormalPair_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$graphics = 'Dual DX12 offscreen 1280x720, 30FPS cap, medium scalability, startup -noraytracing, GI/reflections/motion blur disabled, per-client streaming pool 768MB, VSM physical pages 1024 (medium bias/samples unchanged). Functional visual profile, not default-quality/performance acceptance.'
if ($DryRun) {
    [pscustomobject]@{Output=$run;Round=$Round;FirstClient=$FirstClient;Map=$map;Graphics=$graphics;NormalGameplay=$true;RenderedClients=2;Port=$Port} | ConvertTo-Json
    return
}
foreach ($path in @($project,$editor,$module,$mapFile)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file missing: $path" } }
if (@([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveUdpListeners() | Where-Object Port -eq $Port).Count) { throw 'Acceptance port is occupied.' }
$null = New-Item -ItemType Directory -Path $run
$owned = [Collections.Generic.List[object]]::new()
$failure = $null; $passed = $false
$moduleHash = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
$shared = @(('"' + $project + '"'),'-NoLiveCoding','-NoP4','-NoSplash','-NoSteam','-NoSound',
    '-ddc=fpstrueEditorTestDDC','-LocalDataCachePath="C:\UEBuild\FPS\RARAnimationCompletion_20261016\DDC"',
    ('-PktLag=' + $PacketLagMs),('-PktLagVariance=' + $PacketLagVarianceMs),('-PktLoss=' + $PacketLossPercent))
$shared += @('-PvpNormalPairWalk', ('-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyPopulation=' + $EnemyPopulation))
if ($FullGameplay) { $shared += '-PvpNormalFullGameplay' }
if ($FacingReview) { $shared += @('-PvpFacingReview', ('-PvpFacingStartServerTime=' + $FacingStartServerTime)) }
if ($FootIKReview) { $shared += '-PvpFootIKReview' }
function Read-AcceptanceLog([string]$name) {
    $path = Join-Path $run ($name + '.log')
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose(); $stream.Dispose() }
}
function Start-AcceptancePeer([string]$name,[string[]]$arguments) {
    $process = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
    $owned.Add([pscustomobject]@{Name=$name;Id=$process.Id;StartedUtc=$process.StartTime.ToUniversalTime();Process=$process})
}
function Assert-AcceptancePeers {
    foreach ($entry in $owned) {
        $entry.Process.Refresh()
        if ($entry.Process.HasExited) { throw ($entry.Name + ' exited before acceptance: ' + $entry.Process.ExitCode) }
        if ((Read-AcceptanceLog $entry.Name) -match '(?im)Fatal error:|Assertion failed:|Unhandled Exception:|Blueprint Runtime Error|LogScript: Warning: Accessed None|Ran out of memory allocating|Out of video memory|PVP_PICKUP_WALK FAIL|PVP_NORMAL_ROUTE FAIL') {
            throw ($entry.Name + ' reported a runtime or normal-pickup failure.')
        }
        if ((Read-AcceptanceLog $entry.Name) -match 'Virtual Shadow Map Page Pool overflow') {
            throw ($entry.Name + ' reported real virtual-shadow page allocation overflow; do not hide visual warnings.')
        }
    }
}
function New-PrivateUserDirectory([string]$path) {
    $null = New-Item -ItemType Directory -Path $path
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetOwner($identity); $acl.SetAccessRuleProtection($true,$false)
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($identity,[Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',[Security.AccessControl.PropagationFlags]::None,[Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $path -AclObject $acl
}
try {
    Start-AcceptancePeer 'Server' ($shared + @(('"' + $map + '?game=/Script/fpstrue.fpstruePvpGameMode"'),'-server','-NullRHI','-Unattended',
        ('-port=' + $Port),('-abslog="' + (Join-Path $run 'Server.log') + '"')))
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ((Read-AcceptanceLog 'Server') -notmatch ('IpNetDriver listening on port ' + $Port + '\b')) {
        Assert-AcceptancePeers
        if ($clock.Elapsed.TotalSeconds -gt 180) { throw 'Ordinary server did not listen within its cold-start allowance.' }
        Start-Sleep -Milliseconds 300
    }
    foreach ($index in @($FirstClient, (3 - $FirstClient))) {
        $name = 'Client' + $index
        $capture = Join-Path $run ('Visual\' + $name)
        $user = Join-Path $run ('PrivateUsers\' + $name)
        $null = New-Item -ItemType Directory -Path $capture -Force
        New-PrivateUserDirectory $user
        Start-AcceptancePeer $name ($shared + @(('127.0.0.1:' + $Port),'-game','-Multiprocess','-dx12','-Windowed','-RenderOffscreen',
            '-ResX=1280','-ResY=720','-ForceRes','-NoVSync','-noraytracing','-PvpPickupWalk','-PvpNormalPair','-PvpSmokeVisual','-PvpSmokeObserve',
            ('-PvpSmokeCaptureDir="' + $capture + '"'),('-UserDir="' + $user + '"'),('-abslog="' + (Join-Path $run ($name + '.log')) + '"'),
            '-ExecCmds="t.MaxFPS 30,sg.ViewDistanceQuality 1,sg.AntiAliasingQuality 1,sg.ShadowQuality 1,sg.GlobalIlluminationQuality 1,sg.ReflectionQuality 1,sg.PostProcessQuality 1,sg.TextureQuality 1,sg.EffectsQuality 1,sg.FoliageQuality 1,r.DynamicGlobalIlluminationMethod 0,r.ReflectionMethod 0,r.MotionBlurQuality 0,r.Streaming.PoolSize 768,r.Shadow.Virtual.MaxPhysicalPages 1024"'))
        # The first player must really pick its map weapon before the second walks;
        # this avoids a harness race where both choose the same unowned pickup.
        if ($index -eq $FirstClient) {
            $join = [Diagnostics.Stopwatch]::StartNew()
            while ((Read-AcceptanceLog $name) -notmatch 'PVP_PICKUP_WALK EQUIPPED') {
                Assert-AcceptancePeers
                if ($join.Elapsed.TotalSeconds -gt 240) { throw 'First ordinary client did not reach and equip its real map pickup.' }
                Start-Sleep -Milliseconds 300
            }
        }
    }
    [pscustomobject]@{Directory=$run;Round=$Round;FirstClient=$FirstClient;Processes=@($owned | Select-Object Name,Id);Graphics=$graphics} | ConvertTo-Json -Depth 4
    $clock.Restart()
    $completionPendingAt = $null
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        Assert-AcceptancePeers
        $complete = $true
        $clientLogs = @{Client1=(Read-AcceptanceLog 'Client1');Client2=(Read-AcceptanceLog 'Client2')}
        foreach ($name in @('Client1','Client2')) {
            $log = $clientLogs[$name]
            if ($log -notmatch 'PVP_NORMAL_PAIR ACTIONS_COMPLETE' -or $log -notmatch 'PVP_PICKUP_WALK PASS' -or $log -notmatch 'PVP_NORMAL_PAIR RENDEZVOUS') { $complete = $false }
            foreach ($phase in @('fire','reload','aim','sprint')) {
                if ($log -notmatch ('PVP_VISUAL_ACTION Phase=' + $phase + ' Remote=1\b')) { $complete = $false }
            }
            foreach ($phase in @('NormalRemoteWalk','NormalRemoteWalkStopIdle','NormalRemoteSprintMoving','NormalRemoteSprintStopIdle')) {
                if ($log -notmatch ('PVP_NORMAL_LOCOMOTION SAVED Phase=' + $phase + ' Remote=1\b')) { $complete = $false }
            }
            foreach ($phase in @('Walk','Sprint')) {
                if ($log -notmatch ('PVP_NORMAL_LOCOMOTION INPUT Phase=' + $phase + '\b.*Input=StandardW Released=1') -or
                    $log -notmatch ('PVP_NORMAL_LOCOMOTION STOP Phase=' + $phase + '\b.*Idle=1')) { $complete = $false }
            }
            foreach ($file in @('EmptyHands.png','PickupEquipped.png','NormalRemoteHolding.png','remote_fire.png','remote_reload.png','remote_aim.png','remote_sprint.png',
                'NormalRemoteWalk.png','NormalRemoteWalkStopIdle.png','NormalRemoteSprintMoving.png','NormalRemoteSprintStopIdle.png')) {
                $image = Get-Item -LiteralPath (Join-Path $run ('Visual\' + $name + '\' + $file)) -ErrorAction SilentlyContinue
                if ($null -eq $image -or $image.Length -le 0) { $complete = $false }
            }
            if ($FacingReview) {
                if ($log -notmatch 'PVP_FACING_REVIEW PASS\b.*RemoteLeft=1 RemoteRight=1 PitchUp75=1 PitchDown75=1') { $complete = $false }
                $frames = @(Get-ChildItem -LiteralPath (Join-Path $run ('Visual\' + $name)) -File -Filter 'FacingRemote_*.png' -ErrorAction SilentlyContinue | Where-Object Length -gt 0)
                if ($frames.Count -lt 40) { $complete = $false }
            }
            if ($FullGameplay) {
                if ($log -notmatch 'PVP_NORMAL_GAMEPLAY COMPLETE' -or $log -notmatch 'PVP_NORMAL_GAMEPLAY ENEMY_KILLED' -or $log -notmatch 'PVP_NORMAL_GAMEPLAY PLAYER_DIED') { $complete = $false }
                foreach ($file in @('NormalEnemyDeath.png','NormalEnemyDeathSettled.png','NormalPlayerDeath.png','NormalPlayerDeathSettled.png')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $run ('Visual\' + $name + '\' + $file)) -PathType Leaf)) { $complete = $false }
                }
            }
        }
        if ($FullGameplay) {
            $server = Read-AcceptanceLog 'Server'
            if ($server -notmatch 'PVP_NORMAL_GAMEPLAY ENEMY_DAMAGE' -or $server -notmatch 'PVP_NORMAL_GAMEPLAY SERVER_DEATH' -or $server -notmatch 'PVP_NORMAL_GAMEPLAY SERVER_RESPAWN') { $complete = $false }
            foreach ($file in @('NormalEnemyHit.png','NormalPlayerHit.png','NormalPlayerRespawn.png')) {
                if (-not (@('Client1','Client2') | Where-Object { Test-Path -LiteralPath (Join-Path $run ('Visual\' + $_ + '\' + $file)) -PathType Leaf })) { $complete = $false }
            }
        }
        if ($complete) { $passed = $true; break }
        if ($clientLogs.Client1 -match 'PVP_PICKUP_WALK PASS' -and $clientLogs.Client2 -match 'PVP_PICKUP_WALK PASS') {
            # Completion, log flushing and PNG delivery are not atomic. Recheck every unchanged requirement before failing.
            if ($null -eq $completionPendingAt) { $completionPendingAt = $clock.Elapsed.TotalSeconds }
            if ($clock.Elapsed.TotalSeconds - $completionPendingAt -ge 2.0) {
                throw 'Both gameplay sequences completed, but required remote animation evidence was missing; inspect per-client images and PVP_VISUAL_ACTION rows.'
            }
        } else {
            $completionPendingAt = $null
        }
        Start-Sleep -Milliseconds 400
    }
    if (-not $passed) { throw 'Normal dual-rendered pickup/action acceptance did not satisfy all per-client state and artifact checks.' }
    if ((Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash -ne $moduleHash) { throw 'The loaded module changed during acceptance.' }
} catch { $failure = $_.Exception.Message }
finally {
    foreach ($entry in $owned) {
        $process = Get-Process -Id $entry.Id -ErrorAction SilentlyContinue
        if ($null -ne $process -and $process.StartTime.ToUniversalTime() -eq $entry.StartedUtc -and
            [string]::Equals($process.Path,$editor,[StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $entry.Id -Force -ErrorAction SilentlyContinue }
    }
    $images = @(Get-ChildItem -LiteralPath (Join-Path $run 'Visual') -Recurse -File -Filter '*.png' -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{Path=$_.FullName;Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    [pscustomobject]@{Passed=($passed -and -not $failure);Failure=$failure;Round=$Round;FirstClient=$FirstClient;ModuleSHA256=$moduleHash;
        Network=@{OutgoingLagMs=$PacketLagMs;LagVarianceMs=$PacketLagVarianceMs;LossPercent=$PacketLossPercent};RenderedClients=2;
        NormalGameplayEntry=$true;AutomaticEquipment=$false;AvatarTeleport=$false;ServerNavigationRoute=$true;FullGameplay=[bool]$FullGameplay;EnemyPopulation=$EnemyPopulation;Graphics=$graphics;Images=$images;VisualQualityReviewed=$false;AudioReviewed=$false;
        FacingReview=[bool]$FacingReview;FacingStartServerTime=$FacingStartServerTime;
        Scope='Local Editor DS with two real rendered clients. Real CMC map pickup and production ability input methods; added locomotion segments hold/release standard W through PlayerInput and inspect remote walk/sprint/stop-Idle CPU-skinned sole clearance plus saved PNG. Optional facing review uses normal controller rotation input and at least 40 consecutive remote frames per peer. This is not physical keyboard hardware, packaged/public acceptance, audio or performance proof. PrivateUsers contains credentials and must not be shared.'} |
        ConvertTo-Json -Depth 7 | Out-File -LiteralPath (Join-Path $run 'Report.json') -Encoding utf8
    Write-Output ('VISUAL_ACCEPTANCE_REPORT ' + (Join-Path $run 'Report.json'))
}
if ($failure) { throw $failure }
