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
    [switch]$RenderedClients,
    [switch]$VisibleClients,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
if ($VisibleClients -and -not $RenderedClients) {
    throw 'VisibleClients requires -RenderedClients; visible acceptance must use the real renderer.'
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
if ($RenderedClients -and -not $IncludePve) {
    throw 'RenderedClients verifies replicated enemies as well as the local player; keep IncludePve enabled.'
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
if ($IsRemote) {
    $ServerPath = $RemoteExecutable
    $RemoteRunDirectory = '/var/tmp/fpstrue-smoke-' + $RunId
    $RemoteLog = $RemoteRunDirectory + '/Server.log'
    $RemoteUnit = 'fpstrue-pvp-smoke-' + $RunId + '.service'
    $ServerArguments = @(
        ($Map + '?game=' + $SmokeConfig.GameMode), ('-port=' + $Port), ('-abslog=' + $RemoteLog),
        ('-UserDir=' + $RemoteRunDirectory + '/User')
    ) + $SharedArguments
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
        RenderedClients = [bool]$RenderedClients
        VisibleClients = [bool]$VisibleClients
        VisualRequired = if ($RenderedClients) { 'Both initial clients and, when requested, the restored client must emit matching PVP_VISUAL_READY/CAPTURE markers, then produce actual 1280x720 PNGs in their own capture directories. PNG dimensions/hash are checked; human visual review is separate.' } else { $null }
        PveRequired = if ($IncludePve) { $SmokeConfig.PveServerMarkers + @('Both CLIENT_PVE_REPLICATED PASS; one CLIENT_PVE_COMPLETE PASS') } else { @() }
        PerformanceIntegrationRequired = if ($IncludePve) { 'Dedicated server must start/populate Gameplay-only scheduling; each original and restored client must start/apply its own populated presentation budget without server Gameplay viewpoints. Dedicated server must never apply render/sharing work. Sharing follower markers are optional diagnostics, not a one-enemy smoke gate. NullRHI application markers do not prove rendered frames.' } else { $null }
        ReconnectRequired = if ($IncludeReconnect) { 'After the original PvP/PvE gates, hard-stop only owned Client2; wait for retention and offline damage, restart it with the same private UserDir, then correlate SERVER_RETAIN/SERVER_ACCEPT/CLIENT_READY by ResumeId and preserved Pawn/PS/state. The restart has a separate log and never substitutes for original PvP evidence.' } else { $null }
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
    if ($VisualCaptures.ContainsKey($Command.Name)) { return $VisualCaptures[$Command.Name] }
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
    $VisualCaptures[$Command.Name] = $Artifact
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
    Update-RemoteSmokeLog
    if ($IsRemote -and $RemoteOwned -and $RemoteActiveState -notin @('activating', 'active', 'reloading')) {
        throw "The owned remote Server unit is $RemoteActiveState before acceptance completed. Read $ServerLog."
    }
    if ($IsRemote -and ((Read-SmokeLog $ServerLog) -match $SmokeConfig.Failure -or
        (Read-SmokeLog $ServerLog) -match $SmokeConfig.ExplicitOom)) {
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
        if ((Read-SmokeLog $Entry.Log) -match $SmokeConfig.Failure -or (Read-SmokeLog $Entry.Log) -match $SmokeConfig.ExplicitOom) {
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
                $Evidence[$ClientCommand.Name + '.Complete']) { $CompletedClients++ }
            # A single observer must receive both movement states; two partial logs cannot satisfy this check.
            if ($Evidence[$ClientCommand.Name + '.RemoteSprint'] -and $Evidence[$ClientCommand.Name + '.RemoteAim']) { $MovementObservers++ }
        }
        $MissingServer = @($SmokeConfig.ServerMarkers | Where-Object { -not $Evidence['Server.' + $_] })
        $MissingClients = @($ClientCommands | Where-Object { -not $Evidence[$_.Name + '.Ready'] })
        $MissingDeep = 0
        if ($DeepChecks) {
            $Evidence['Server.DedicatedIdentity'] = $ServerText -match 'PVP_SMOKE[^\r\n]*SERVER_IDENTITY[^\r\n]*Dedicated=1\b[^\r\n]*LocalPlayers=0\b'
            $Evidence['Server.Displacement'] = $ServerText -match 'PVP_SMOKE[^\r\n]*SERVER_DISPLACEMENT PASS\b'
            $ClockClients = 0
            $DisplacementObservers = 0
            $AcceptedShots = [regex]::Matches($ServerText, 'SERVER_SHOT_ACCEPT[^\r\n]*ShotId=(\d+)\s+Session=(\d+)\s+Generation=(\d+)[^\r\n]*Result=0\b[^\r\n]*FireTime=(-?\d+(?:\.\d+)?)\b')
            $RewindShots = [regex]::Matches($ServerText, 'REWIND[^\r\n]*ShotId=(\d+)[^\r\n]*FireTime=(-?\d+(?:\.\d+)?)\b[^\r\n]*Hit=(\S+)[^\r\n]*Bone=(\S+)')
            $MatchedShot = $false
            $PresentationOnce = $true
            foreach ($ClientCommand in $ClientCommands) {
                $ClientText = Read-SmokeLog $ClientCommand.Log
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
                    $Identity = 'ShotId=' + $ShotId + '\s+Session=' + $Session + '\s+Generation=' + $Generation + '\b'
                    $Ack = $ClientText -match ('CLIENT_SHOT_ACK\s+' + $Identity + '[^\r\n]*Result=0\b[^\r\n]*Predicted=1\b')
                    $Predicted = $ClientText -match ('CLIENT_SHOT_PRESENT\s+' + $Identity + '\s+Count=1\b')
                    $Rewind = @($RewindShots | Where-Object {
                        $_.Groups[1].Value -eq $ShotId -and $_.Groups[2].Value -eq $FireTime -and
                        $_.Groups[3].Value -notin @('None', 'none', 'NULL', 'nullptr') -and
                        $_.Groups[4].Value -notin @('None', 'none', 'NULL', 'nullptr')
                    }).Count -gt 0
                    if ($Ack -and $Rewind -and $Predicted) { $MatchedShot = $true }
                }
                # Count events for each full identity, rather than accepting a
                # cumulative counter or requiring unreliable multicasts to arrive.
                $Presentations = [regex]::Matches($ClientText, 'CLIENT_SHOT_PRESENT\s+ShotId=(\d+)\s+Session=(\d+)\s+Generation=(\d+)\s+Count=(\d+)')
                $Counts = @{}
                foreach ($Presentation in $Presentations) {
                    $Identity = $Presentation.Groups[1].Value + ':' + $Presentation.Groups[2].Value + ':' + $Presentation.Groups[3].Value
                    if (-not $Counts.ContainsKey($Identity)) { $Counts[$Identity] = 0 }
                    $Counts[$Identity] += [int]$Presentation.Groups[4].Value
                    if ($Counts[$Identity] -gt 1) { $PresentationOnce = $false }
                }
            }
            $Evidence['Deep.BothClientClocks'] = $ClockClients -eq $SmokeConfig.ClientCount
            $Evidence['Deep.RemoteDisplacement'] = $DisplacementObservers -gt 0
            $Evidence['Deep.AcceptAckRewindSameShot'] = $MatchedShot
            $Evidence['Deep.NoRepeatedPresentation'] = $PresentationOnce
            foreach ($Key in @('Server.DedicatedIdentity', 'Server.Displacement', 'Deep.BothClientClocks',
                'Deep.RemoteDisplacement', 'Deep.AcceptAckRewindSameShot', 'Deep.NoRepeatedPresentation')) {
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
        $PvpPassed = $MissingServer.Count -eq 0 -and $MissingClients.Count -eq 0 -and
            $CompletedClients -gt 0 -and $MovementObservers -gt 0 -and $MissingConfiguration -eq 0 -and $MissingDeep -eq 0
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
                $PerformanceClients -eq $SmokeConfig.ClientCount
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
                $ClientToRestart = $ClientCommands | Where-Object { $_.Name -eq 'Client2' } | Select-Object -First 1
                $InitialReady = @(Get-SmokeResumeRecords (Read-SmokeLog $ClientToRestart.Log) 'CLIENT_READY' |
                    Where-Object { $_.Restored -eq '0' } | Select-Object -Last 1)
                if ($InitialReady.Count -eq 0) { throw 'Client2 has no confirmed initial handshake identity for the reconnect scenario.' }
                # Existence/size only: the runner never reads the ticket bytes.
                $TicketFiles = @(Get-ChildItem -LiteralPath $ClientToRestart.UserDirectory -Filter 'PvpResume.dat' -Recurse -File)
                if ($TicketFiles.Count -ne 1 -or $TicketFiles[0].Length -le 0 -or $TicketFiles[0].Length -gt 4096) {
                    throw 'Client2 has not persisted exactly one bounded resume credential under its private UserDir.'
                }
                $ReconnectResumeId = $InitialReady[0].ResumeId
                $OwnedClient = $Started | Where-Object { $_.Name -eq 'Client2' } | Select-Object -First 1
                if ($null -eq $OwnedClient) { throw 'The owned Client2 process is missing before the reconnect scenario.' }
                Stop-OwnedSmokeClient $OwnedClient
                $ReconnectStoppedUtc = [DateTime]::UtcNow
                $Evidence['Reconnect.OwnedClientHardStopped'] = $true
                $ReconnectStage = 'WaitingForServerRetention'
            }
            if ($ReconnectStage -eq 'WaitingForServerRetention') {
                $Retained = @(Get-SmokeResumeRecords $ServerText 'SERVER_RETAIN' |
                    Where-Object { $_.ResumeId -eq $ReconnectResumeId } | Select-Object -Last 1)
                if ($Retained.Count -gt 0) {
                    $ReconnectBaseline = $Retained[0]
                    $Evidence['Reconnect.ServerRetainedParticipant'] = $true
                    # Editor cold start can consume most of the real grace
                    # period. Start it now, but do not accept recovery unless
                    # genuine offline damage was recorded before the claim.
                    $ReconnectLog = Join-Path $OutputDirectory 'Client2Resume.log'
                    $ReconnectVisualDirectory = if ($RenderedClients) { Join-Path $OutputDirectory 'Visual\Client2Resume' } else { $null }
                    $ReconnectCommand = [pscustomobject]@{
                        Name = 'Client2Resume'; Log = $ReconnectLog; UserDirectory = $ClientToRestart.UserDirectory
                        VisualDirectory = $ReconnectVisualDirectory
                        Arguments = @($ClientToRestart.Arguments | Where-Object { $_ -notmatch '^-(?:abslog|PvpSmokeCaptureDir)=' }) +
                            @('-PvpSmokeRestoreOnly', ('-abslog="' + $ReconnectLog + '"'))
                    }
                    if ($RenderedClients) {
                        $null = New-Item -ItemType Directory -Path $ReconnectVisualDirectory -Force
                        $ReconnectCommand.Arguments += '-PvpSmokeCaptureDir="' + $ReconnectVisualDirectory + '"'
                    }
                    Start-SmokeProcess $ReconnectCommand.Name $ClientPath $ReconnectCommand.Arguments $ReconnectCommand.Log
                    $ReconnectStage = 'WaitingForOfflineDamage'
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
                    $Evidence['Reconnect.ClockResampled'] = (Read-SmokeLog $ReconnectCommand.Log) -match 'CLOCK_SAMPLE PASS\b'
                    $RestoredPerformancePassed = -not $IncludePve
                    if ($IncludePve) {
                        $RestoredText = Read-SmokeLog $ReconnectCommand.Log
                        $Evidence['Client2Resume.PerformanceStarted'] = $RestoredText -match $SmokeConfig.ClientPerformanceStart
                        $Evidence['Client2Resume.PerformanceApplied'] = $RestoredText -match $SmokeConfig.ClientPerformanceApplied
                        $Evidence['Client2Resume.SharingActive'] = $RestoredText -match $SmokeConfig.ClientSharingActive
                        $RestoredPerformancePassed = $Evidence['Client2Resume.PerformanceStarted'] -and $Evidence['Client2Resume.PerformanceApplied']
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
                    if ($ResumePassed) { $ReconnectStage = 'Passed' }
                }
            }
            if ($ReconnectStage -eq 'Passed') { $ResumePassed = $true }
        }
        $Succeeded = $PvpPassed -and $PvePassed -and $ResumePassed -and $InitialVisualPassed
        # Fail markers invalidate a run even when a previous phase printed PASS.
        if ($ServerText -match $SmokeConfig.Failure -or $ServerText -match $SmokeConfig.ExplicitOom) { throw "Server reported a failure; see $ServerLog." }
        if ($IsRemote -and $RemoteActiveState -notin @('activating', 'active', 'reloading')) {
            throw "The owned remote Server unit is $RemoteActiveState at acceptance; see $ServerLog."
        }
        foreach ($Entry in $Started) {
            if ((Read-SmokeLog $Entry.Log) -match $SmokeConfig.Failure -or (Read-SmokeLog $Entry.Log) -match $SmokeConfig.ExplicitOom) { throw "$($Entry.Name) reported a failure; see $($Entry.Log)." }
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
            if ((Read-SmokeLog $ServerLog) -match $SmokeConfig.Failure -or
                (Read-SmokeLog $ServerLog) -match $SmokeConfig.ExplicitOom) {
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
            if ($FinalServerText -match $SmokeConfig.Failure -or $FinalServerText -match $SmokeConfig.ExplicitOom -or
                $Journal -match $SmokeConfig.Failure -or $Journal -match $SmokeConfig.ExplicitOom) {
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
    $Report = [pscustomobject]@{
        Passed = $Succeeded; Failure = $FailureReason
        ExecutionMode = $ExecutionMode
        ServerMode = $ServerMode; ServerAddress = $ConnectAddress
        DeepChecks = $DeepChecks
        IncludePve = $IncludePve
        IncludeReconnect = $IncludeReconnect
        RenderedClients = [bool]$RenderedClients
        VisibleClients = [bool]$VisibleClients
        ClientRenderer = if ($VisibleClients) { 'D3D12Windowed' } elseif ($RenderedClients) { 'D3D12Offscreen' } else { 'NullRHI' }
        ServerRenderer = 'NullRHI'
        VisualArtifacts = @($VisualCaptures.Values | Sort-Object Client)
        RenderedRecoveryVerified = [bool]($RenderedClients -and $IncludeReconnect -and $Succeeded -and $Evidence['Reconnect.RenderedCapture'])
        VisualQualityReviewed = $false
        ReconnectStage = $ReconnectStage
        ReconnectMethod = if ($IncludeReconnect) { 'OwnedProcessHardStopAndRestartWithPrivateUserDirectory' } else { 'NotRequested' }
        AutomaticRetryTransportVerified = $false
        HardDisconnectUtc = $ReconnectStoppedUtc
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
