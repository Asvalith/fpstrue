<#
160 authoritative enemies, two network clients, one rendered client at a time.
Each case uses disposable user folders and the SAME DLL/map/config. Off/Screen/
WorldAlways/WorldEvent change UI only. The other client uses NullRHI to avoid
two local renderers competing for the same GPU; swap RenderRoles for coverage.
This is an Editor Development profile, not packaged capacity certification.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
    [string]$EngineRoot = 'E:/program/ue554/UE_5.5',
    [ValidateSet('Off','Screen','WorldAlways','WorldEvent')][string[]]$Modes = @('Off','Screen','WorldAlways','WorldEvent'),
    [ValidateSet('Client1','Client2')][string[]]$RenderRoles = @('Client1','Client2'),
    [ValidateSet('Static','HealthAndDeath')][string[]]$Workloads = @('Static','HealthAndDeath'),
    [ValidateRange(1,5)][int]$Repeat = 3,
    [ValidateRange(1024,65000)][int]$Port = 7893,
    [ValidateRange(120,600)][int]$TimeoutSeconds = 300,
    [string]$Map = '/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192',
    [string]$RunName = ('EnemyUI160_' + (Get-Date -Format 'yyyyMMdd_HHmmss')),
    [string]$OutputRoot = 'C:/UEBuild/FinalAcceptance20261018/FPS/Profiling',
    [switch]$Trace,
    [switch]$EnemyUIViewPerComponent,
    [switch]$SelfTest,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if ($RunName -notmatch '^[A-Za-z0-9_-]+$') { throw 'RunName must be a plain directory name.' }
if ($Map -notmatch '^/Game/[A-Za-z0-9_/]+$') { throw 'Map must be a package without travel options.' }

function Write-ProfileControl {
    param(
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Path,
        [Parameter(Mandatory)][ValidateSet('WAIT','START','STOP','EXIT')][string]$Command,
        [ValidateRange(1,200)][int]$MaxAttempts = 40,
        [ValidateRange(1,1000)][int]$RetryMilliseconds = 25
    )
    # Readers must see an entire old or new command, never a truncated file.
    # Keep the replacement on the same volume. UE may briefly deny delete
    # sharing while reading; retry that operation, not an in-place write.
    $Destination = [IO.Path]::GetFullPath($Path)
    $Directory = [IO.Path]::GetDirectoryName($Destination)
    if (-not [IO.Directory]::Exists($Directory)) { throw "Control directory does not exist: $Directory" }
    $Temporary = Join-Path $Directory ('.control-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($Temporary,$Command,[Text.UTF8Encoding]::new($false))
        for ($Attempt = 1; $Attempt -le $MaxAttempts; ++$Attempt) {
            try {
                if ([IO.File]::Exists($Destination)) { [IO.File]::Replace($Temporary,$Destination,[NullString]::Value) }
                else { [IO.File]::Move($Temporary,$Destination) }
                return
            } catch [IO.IOException] {
                if ($Attempt -eq $MaxAttempts) {
                    throw [IO.IOException]::new("Atomic control command $Command failed after $MaxAttempts attempts: $Destination",$_.Exception)
                }
                Start-Sleep -Milliseconds $RetryMilliseconds
            }
        }
    } finally {
        if ([IO.File]::Exists($Temporary)) { [IO.File]::Delete($Temporary) }
    }
}

function Get-ProfileViewArguments([string]$Role,[string]$RenderRole,[bool]$PerComponent) {
    if ($PerComponent -and $Role -eq $RenderRole) { '-EnemyUIViewPerComponent' }
}

function Invoke-ProfileSelfTest {
    $TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('EnemyUIProfileSelfTest_' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($TestRoot) | Out-Null
    $Jobs = @()
    $Checks = 0
    try {
        $ControlPath = Join-Path $TestRoot 'control.txt'
        Write-ProfileControl $ControlPath 'WAIT'
        if ([IO.File]::ReadAllText($ControlPath) -cne 'WAIT') { throw 'Initial atomic control creation failed.' }
        ++$Checks

        # Independent PowerShell processes exercise both UE-like short read
        # locks and delete-sharing readers while the same writer replaces data.
        for ($Index = 0; $Index -lt 3; ++$Index) {
            $ReadyPath = Join-Path $TestRoot ('reader' + $Index + '.ready')
            $Jobs += Start-Job -ArgumentList $ControlPath,$ReadyPath,$Index -ScriptBlock {
                param($ControlPath,$ReadyPath,$ReaderIndex)
                $ErrorActionPreference = 'Stop'
                $Reads = 0
                [IO.File]::WriteAllText($ReadyPath,'READY')
                for ($Iteration = 0; $Iteration -lt 600; ++$Iteration) {
                    $Share = if ($ReaderIndex -eq 0) { [IO.FileShare]::Read } else { [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete }
                    $Stream = $null
                    try {
                        $Stream = [IO.File]::Open($ControlPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,$Share)
                        $Reader = [IO.StreamReader]::new($Stream)
                        try {
                            $Text = $Reader.ReadToEnd()
                            if ($Text -cnotin @('WAIT','START','STOP','EXIT')) { throw "Partial/unknown control command: [$Text]" }
                            ++$Reads
                        } finally { $Reader.Dispose(); $Stream = $null }
                    } catch [IO.IOException] {
                        # A replacement may briefly win access; never accept an
                        # empty/partial read as a successful command.
                    } finally { if ($null -ne $Stream) { $Stream.Dispose() } }
                    Start-Sleep -Milliseconds 1
                }
                [pscustomobject]@{Reader=$ReaderIndex; Reads=$Reads}
            }
        }
        $Deadline = [DateTime]::UtcNow.AddSeconds(30)
        while (@(Get-ChildItem -LiteralPath $TestRoot -Filter '*.ready').Count -ne 3) {
            if ([DateTime]::UtcNow -gt $Deadline) { throw 'Reader self-test startup timeout.' }
            if (@($Jobs | Where-Object State -eq 'Failed').Count) { throw 'A self-test reader failed to start.' }
            Start-Sleep -Milliseconds 20
        }
        for ($Index = 0; $Index -lt 128; ++$Index) { Write-ProfileControl $ControlPath (@('WAIT','START','STOP','EXIT')[$Index % 4]) }
        if (@($Jobs | Wait-Job -Timeout 30).Count -ne 3) { throw 'Reader self-test completion timeout.' }
        $ReaderResults = @($Jobs | Receive-Job -ErrorAction Stop)
        if (@($Jobs | Where-Object State -ne 'Completed').Count -or $ReaderResults.Count -ne 3 -or @($ReaderResults | Where-Object Reads -lt 1).Count) { throw 'Incomplete independent reader checks.' }
        $Checks += 3
        if ([IO.File]::ReadAllText($ControlPath) -cne 'EXIT') { throw 'Final atomic control command was lost.' }
        ++$Checks

        # A permanent reader lock must fail boundedly, leave the last command
        # intact, and remove only our temporary replacement file.
        $Lock = [IO.File]::Open($ControlPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        $Rejected = $false
        try { try { Write-ProfileControl $ControlPath 'START' -MaxAttempts 3 -RetryMilliseconds 1 } catch [IO.IOException] { $Rejected = $true } }
        finally { $Lock.Dispose() }
        if (-not $Rejected -or [IO.File]::ReadAllText($ControlPath) -cne 'EXIT') { throw 'Permanent sharing conflict did not fail closed.' }
        if (@(Get-ChildItem -LiteralPath $TestRoot -Filter '.control-*.tmp' -Force).Count) { throw 'A failed replacement leaked a temporary file.' }
        $Checks += 2
        foreach ($Arguments in @(
            @{Command='BROKEN'}, @{Command='WAIT'; MaxAttempts=0}, @{Command='WAIT'; RetryMilliseconds=0}
        )) {
            $Rejected = $false
            try { Write-ProfileControl -Path $ControlPath @Arguments } catch { $Rejected = $true }
            if (-not $Rejected) { throw 'Invalid control parameters were accepted.' }
            ++$Checks
        }
        foreach ($Arguments in @(
            @{RunName='../invalid'}, @{Map='/Game/Map?invalid'}, @{Port=0}, @{Repeat=0},
            @{TimeoutSeconds=1}, @{Modes='Invalid'}, @{RenderRoles='Server'}, @{Workloads='Invalid'}
        )) {
            $Rejected = $false
            try { & $PSCommandPath -SelfTest @Arguments | Out-Null } catch { $Rejected = $true }
            if (-not $Rejected) { throw 'Invalid runner parameters were accepted.' }
            ++$Checks
        }
        foreach ($Role in @('Server','Client1','Client2')) {
            $Arguments = @(Get-ProfileViewArguments $Role 'Client1' $true)
            $Expected = if ($Role -eq 'Client1') { 1 } else { 0 }
            if ($Arguments.Count -ne $Expected -or @(Get-ProfileViewArguments $Role 'Client1' $false).Count -ne 0) { throw 'Per-component view option escaped the rendered role.' }
            ++$Checks
        }
        Write-Output "Enemy UI runner SelfTest passed: $Checks checks; 3 independent readers, 128 atomic updates; no engine launched."
    } finally {
        foreach ($Job in $Jobs) { if ($Job.State -eq 'Running') { Stop-Job $Job }; Remove-Job $Job -Force }
        # The GUID directory contains only the four known self-test files.
        foreach ($Name in @('control.txt','reader0.ready','reader1.ready','reader2.ready')) {
            $Path = Join-Path $TestRoot $Name
            if ([IO.File]::Exists($Path)) { [IO.File]::Delete($Path) }
        }
        [IO.Directory]::Delete($TestRoot)
    }
}

if ($SelfTest) { Invoke-ProfileSelfTest; return }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$Engine = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Project = Join-Path $ProjectRoot 'fpstrue.uproject'
$Dll = Join-Path $ProjectRoot 'Binaries/Win64/UnrealEditor-fpstrue.dll'
$MapFile = Join-Path $ProjectRoot ('Content/' + $Map.Substring(6) + '.umap')
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$EvidenceRoot = Join-Path $OutputRoot $RunName
foreach ($Path in @($Engine,$Project,$Dll,$MapFile)) { if (-not (Test-Path -LiteralPath $Path)) { throw "Missing input: $Path" } }
if (Test-Path -LiteralPath $EvidenceRoot) { throw "Do not overwrite earlier evidence: $EvidenceRoot" }
$FingerprintPaths = @($Dll,$MapFile,(Join-Path $ProjectRoot 'Config/DefaultEngine.ini'),(Join-Path $ProjectRoot 'Config/DefaultGame.ini'),$PSCommandPath,
    (Join-Path $PSScriptRoot 'SummarizeEnemyUIProfile.py'))
$FingerprintPaths += @('Content/Config','Content/FirstPerson/AI','Content/FirstPerson/UI','Content/FirstPerson/Blueprints/enemy','Content/FirstPerson/Animations/FPSMPT' |
    ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $_) -File -Filter '*.uasset' -ErrorAction Stop | ForEach-Object FullName })
$FingerprintPaths += Join-Path $ProjectRoot 'Content/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.uasset'
function Get-ProfileFingerprint { @($FingerprintPaths | ForEach-Object { [pscustomobject]@{Path=$_; SHA256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash} }) }
$InitialFingerprint = Get-ProfileFingerprint
$Cases = @()
foreach ($Workload in $Workloads) {
foreach ($Role in $RenderRoles) {
    for ($Round = 1; $Round -le $Repeat; ++$Round) {
        # Reverse alternate rounds to avoid always attributing warm GPU/cache state to one mode.
        $Order = @($Modes)
        if (($Round % 2) -eq 0) { [array]::Reverse($Order) }
        foreach ($Mode in $Order) { $Cases += [pscustomobject]@{Mode=$Mode; RenderRole=$Role; Round=$Round; Workload=$Workload} }
    }
}
}
if ($DryRun) {
    [pscustomobject]@{EvidenceRoot=$EvidenceRoot; Cases=$Cases; Map=$Map; Trace=[bool]$Trace; EnemyUIViewPerComponent=[bool]$EnemyUIViewPerComponent;
        Population=160; Clients=2; RenderedClientsPerCase=1; PeerUIPolicy='Off'; Width=1600; Height=900; Warmup=15; Capture=30;
        Boundary='NullRHI peer is networking only. Frustum counts are not pixel-visible counts.'} | ConvertTo-Json -Depth 6
    return
}
New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null
function Write-ProfileJson($Value, [string]$Path) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false)) }
function Read-ProfileLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $Stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $Reader = [IO.StreamReader]::new($Stream)
    try { return $Reader.ReadToEnd() } finally { $Reader.Dispose() }
}
$Hardware = [ordered]@{
    Cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed)
    Gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate)
    System = @(Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory)
    OS = @(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber)
}
Write-ProfileJson ([ordered]@{Created=(Get-Date).ToString('o'); Fingerprint=$InitialFingerprint; Cases=$Cases; Map=$Map; Trace=[bool]$Trace; EnemyUIViewPerComponent=[bool]$EnemyUIViewPerComponent;
    Population=160; Clients=2; PeerUIPolicy='Off'; Resolution='1600x900'; Seed=1337; Warmup=15; Capture=30;
    Hardware=$Hardware;
    Method='One real renderer; real DS and NullRHI network peer. Only the rendered role changes UI mode; DS and NullRHI peer keep UI Off to avoid changing their CPU competition. Recorded camera and real production AI/damage. Default map is a separate open UI stress arena, not the industrial gameplay scene.'}) (Join-Path $EvidenceRoot 'experiment.json')
$Results = @()
$ReferenceViews = @{}
foreach ($Case in $Cases) {
    $Name = '{0}_{1}_{2}_R{3}' -f $Case.Workload,$Case.RenderRole,$Case.Mode,$Case.Round
    $CaseRoot = Join-Path $EvidenceRoot $Name
    New-Item -ItemType Directory -Path $CaseRoot | Out-Null
    $Control = Join-Path $CaseRoot 'control.txt'
    Write-ProfileControl $Control 'WAIT'
    $Processes = @()
    $Launches = @()
    $Failure = $null
    $Isolation = @()
    $Started = Get-Date
    $Result = [ordered]@{Name=$Name; Mode=$Case.Mode; RenderRole=$Case.RenderRole; Round=$Case.Round; Workload=$Case.Workload; EnemyUIViewPerComponent=[bool]$EnemyUIViewPerComponent; PeerUIPolicy='Off'; ViewReadback=$null; SelectedD3D12Adapter=$null; Valid=$false; Error=$null; PrelaunchExitedIds=@(); Processes=@()}
    try {
        if ((Get-ProfileFingerprint | ConvertTo-Json -Compress) -ne ($InitialFingerprint | ConvertTo-Json -Compress)) { throw 'Build/map/default inputs changed.' }
        # Windows can still enumerate the previous case's terminated process while
        # managed Process handles remain open. A confirmed exited process cannot
        # consume CPU/GPU; keep rejecting every actually live foreign process.
        $ObservedUE = @(Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue)
        $Result.PrelaunchExitedIds = @($ObservedUE | Where-Object {$_.HasExited} | ForEach-Object Id)
        $Existing = @($ObservedUE | Where-Object {-not $_.HasExited})
        if ($Existing.Count -ne 0) {
            Write-ProfileJson @($Existing | ForEach-Object {
                $Actual = Get-CimInstance Win32_Process -Filter ('ProcessId='+$_.Id) -ErrorAction SilentlyContinue
                [pscustomobject]@{Id=$_.Id; Name=$_.ProcessName; ParentId=$Actual.ParentProcessId; CommandLine=$Actual.CommandLine; Phase='BeforeLaunch'}
            }) (Join-Path $CaseRoot 'conflicting-processes.json')
            throw 'Other UE processes would contaminate the profile; close only owned test processes before starting.'
        }
        if (@(Get-Process -Name UnrealBuildTool,UnrealHeaderTool,cl,link -ErrorAction SilentlyContinue).Count -ne 0) {
            throw 'A build is active; wait for its owner to finish before profiling.'
        }
        foreach ($Role in @('Server','Client1','Client2')) {
            $Log = Join-Path $CaseRoot ($Role + '.log')
            $User = Join-Path $CaseRoot ('Users/' + $Role)
            New-Item -ItemType Directory -Path $User -Force | Out-Null
            $Travel = if ($Role -eq 'Server') { $Map+'?game=/Script/fpstrue.fpstruePvpGameMode' } else { '127.0.0.1:'+$Port }
            $LocalMode = if ($Role -eq $Case.RenderRole) { $Case.Mode } else { 'Off' }
            $Args = @(('"'+$Project+'"'), $Travel, '-Unattended','-NoSound','-NoSplash','-NoSteam','-NoLiveCoding',
                '-DDC=InstalledNoZenLocalFallback','-DDC-ForceMemoryCache','-PvpProfile','-BenchmarkEnemies=160','-BenchmarkSeed=1337',
                '-PvpProfileFixedView',('-EnemyHealthBars='+$LocalMode),('-PvpProfileRole='+$Role),
                ('-PvpProfileRoot="'+$OutputRoot+'"'),
                ('-PvpProfileControl="'+$Control+'"'),('-PvpProfileOutput="'+$CaseRoot+'"'),
                ('-UserDir="'+$User+'"'),('-abslog="'+$Log+'"'),'-csvGpuStats','-NoVSync','-ExecCmds="t.MaxFPS 0,r.VSync 0"')
            if (-not $Trace) { $Args += '-PvpProfileNoTrace' } else { $Args += '-statnamedevents' }
            $Args += @(Get-ProfileViewArguments $Role $Case.RenderRole ([bool]$EnemyUIViewPerComponent))
            if ($Case.Workload -eq 'HealthAndDeath') { $Args += '-PvpProfileHealthAndDeath' }
            if ($Role -eq 'Server') {
                $Args += @('-server','-NullRHI',('-port='+$Port),
                    '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyPopulation=160',
                    '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemySpawnInterval=0.05',
                    '-ini:Game:[/Script/fpstrue.fpstruePvpGameMode]:EnemyRespawnDelay=600')
            } else {
                $Args += '-game'
                if ($Role -eq $Case.RenderRole) { $Args += @('-dx12','-Windowed','-RenderOffscreen','-ForceRes','-ResX=1600','-ResY=900') }
                else { $Args += '-NullRHI' }
            }
            $Process = Start-Process -FilePath $Engine -ArgumentList $Args -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
            $Processes += [pscustomobject]@{Role=$Role; Process=$Process; Log=$Log; Forced=$false}
            $Launches += [pscustomobject]@{Role=$Role; ProcessId=$Process.Id; Engine=$Engine; Arguments=$Args}
            if ($Role -eq 'Server') { Start-Sleep -Seconds 3 }
        }
        Write-ProfileJson $Launches (Join-Path $CaseRoot 'launch.json')
        Write-Output "Profiling $Name (160 enemies, one rendered client)."
        $Phase = 'Ready'
        $PhaseAt = Get-Date
        while ($true) {
            if (((Get-Date)-$Started).TotalSeconds -gt $TimeoutSeconds) { throw ('Timeout in phase '+$Phase) }
            $OwnedIds = @($Processes | ForEach-Object {$_.Process.Id})
            $Conflicts = @(Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue | Where-Object {$_.Id -notin $OwnedIds -and -not $_.HasExited})
            if ($Conflicts.Count -gt 0) {
                $ConflictDetails = @($Conflicts | ForEach-Object {
                    $Actual = Get-CimInstance Win32_Process -Filter ('ProcessId='+$_.Id) -ErrorAction SilentlyContinue
                    [pscustomobject]@{Id=$_.Id; Name=$_.ProcessName; ParentId=$Actual.ParentProcessId; CommandLine=$Actual.CommandLine; OwnedIds=$OwnedIds}
                })
                Write-ProfileJson $ConflictDetails (Join-Path $CaseRoot 'conflicting-processes.json')
                throw ('An unrelated UE process appeared during collection: '+($Conflicts.Id -join ','))
            }
            $Builds = @(Get-Process -Name UnrealBuildTool,UnrealHeaderTool,cl,link -ErrorAction SilentlyContinue)
            if ($Builds.Count -gt 0) { throw 'A build appeared during collection.' }
            $Isolation += [pscustomobject]@{At=(Get-Date).ToString('o'); Phase=$Phase; OtherUE=$Conflicts.Count; OtherBuild=$Builds.Count}
            $Logs = @($Processes | ForEach-Object { Read-ProfileLog $_.Log })
            foreach ($LogText in $Logs) { if ($LogText -match 'PVP_PROFILE (?:FAIL|QUARANTINED)|Fatal error:|Assertion failed:|Ensure condition failed:') { throw 'Runtime/profile failure (see retained logs).' } }
            if ($Phase -ne 'Exit' -and @($Processes | Where-Object {$_.Process.HasExited}).Count -gt 0) { throw 'A peer exited before STOP.' }
            if ($Phase -eq 'Ready' -and @($Logs | Where-Object {$_ -match 'PVP_PROFILE READY'}).Count -eq 3) {
                $RenderLog = $Logs[(@('Server','Client1','Client2').IndexOf($Case.RenderRole))]
                $View = [regex]::Match($RenderLog,'PVP_PROFILE READY[^\r\n]*Location=(.*?) Rotation=(.*?) Width=(\d+) Height=(\d+)')
                if (-not $View.Success) { throw 'Rendered client did not report its actual fixed camera.' }
                $ViewKey = ($View.Groups[1..4] | ForEach-Object Value) -join '|'
                if ($ReferenceViews.ContainsKey($Case.RenderRole) -and $ReferenceViews[$Case.RenderRole] -ne $ViewKey) {
                    throw ('Camera changed between UI modes/workloads: '+$ViewKey+' versus '+$ReferenceViews[$Case.RenderRole])
                }
                $ReferenceViews[$Case.RenderRole] = $ViewKey
                $Result.ViewReadback = $ViewKey
                $Phase='Warmup'; $PhaseAt=Get-Date
            }
            elseif ($Phase -eq 'Warmup' -and ((Get-Date)-$PhaseAt).TotalSeconds -ge 15) { Write-ProfileControl $Control 'START'; $Phase='CaptureStart' }
            elseif ($Phase -eq 'CaptureStart' -and @($Logs | Where-Object {$_ -match 'PVP_PROFILE CAPTURING'}).Count -eq 3) { $Phase='Capture'; $PhaseAt=Get-Date }
            elseif ($Phase -eq 'Capture' -and ((Get-Date)-$PhaseAt).TotalSeconds -ge 31) { Write-ProfileControl $Control 'STOP'; $Phase='Flush' }
            elseif ($Phase -eq 'Flush' -and @($Logs | Where-Object {$_ -match 'PVP_PROFILE FLUSHED'}).Count -eq 3) { Write-ProfileControl $Control 'EXIT'; $Phase='Exit' }
            elseif ($Phase -eq 'Exit' -and @($Processes | Where-Object {-not $_.Process.HasExited}).Count -eq 0) { break }
            Start-Sleep -Seconds 1
        }
        foreach ($Entry in $Processes) {
            $Entry.Process.Refresh()
            $Text = [IO.File]::ReadAllText($Entry.Log)
            if ($Entry.Process.ExitCode -ne 0 -or $Text -notmatch 'PVP_PROFILE COMPLETE') { throw ('Incomplete peer '+$Entry.Role) }
            $ExpectedNull = if ($Entry.Role -eq $Case.RenderRole) {0} else {1}
            $ExpectedMode = if ($Entry.Role -eq $Case.RenderRole) {$Case.Mode} else {'Off'}
            if ($Text -notmatch ('UI_CONFIG Role='+$Entry.Role+' Mode='+$ExpectedMode+' Trace=\d NullRHI='+$ExpectedNull+' FixedView=1')) { throw ('UI/render mode readback mismatch: '+$Entry.Role) }
            if ($Entry.Role -eq $Case.RenderRole) {
                # CIM inventories every GPU; only this rendered process's RHI log proves which one was used.
                $Chosen = [regex]::Match($Text,'Chosen D3D12 Adapter Id = (\d+)')
                if (-not $Chosen.Success) { throw 'Rendered peer did not report a selected D3D12 adapter.' }
                $Adapter = [regex]::Match($Text,('Found D3D12 adapter '+$Chosen.Groups[1].Value+': ([^\r\n]+)\r?\n[^\r\n]*\r?\n[^\r\n]*Adapter has (\d+)MB of dedicated video memory'))
                if (-not $Adapter.Success) { throw 'Selected adapter name/dedicated VRAM missing from rendered RHI log.' }
                $Result.SelectedD3D12Adapter = [pscustomobject]@{Id=[int]$Chosen.Groups[1].Value; Description=$Adapter.Groups[1].Value; DedicatedVideoMemoryMB=[int]$Adapter.Groups[2].Value; SourceLog=$Entry.Log}
            }
        }
        if ((Get-ProfileFingerprint | ConvertTo-Json -Compress) -ne ($InitialFingerprint | ConvertTo-Json -Compress)) { throw 'Inputs changed while profiling.' }
        $Result.Valid = $true
    } catch {
        $Failure = $_.Exception.Message
        $Result.Error = $Failure
        Write-Warning "$Name failed: $Failure"
    } finally {
        foreach ($Entry in $Processes) {
            $Entry.Process.Refresh()
            if (-not $Entry.Process.HasExited) { $Entry.Forced=$true; Stop-Process -Id $Entry.Process.Id -Force; $Entry.Process.WaitForExit(10000) | Out-Null }
            $Entry.Process.Refresh()
            $Result.Processes += [pscustomobject]@{Role=$Entry.Role; Id=$Entry.Process.Id; ExitCode=$Entry.Process.ExitCode; Forced=$Entry.Forced; Log=$Entry.Log}
            $Entry.Process.Dispose()
        }
        $Result.DurationSeconds = [Math]::Round(((Get-Date)-$Started).TotalSeconds,3)
        Write-ProfileJson $Isolation (Join-Path $CaseRoot 'isolation.json')
        Write-ProfileJson $Result (Join-Path $CaseRoot 'result.json')
        $Results += [pscustomobject]$Result
        Write-ProfileJson $Results (Join-Path $EvidenceRoot 'results.json')
    }
    # Do not continue a broken mode/population/render setup and manufacture a partial average.
    if (-not $Result.Valid) { throw "Profile stopped after invalid $Name; evidence retained at $EvidenceRoot" }
}
& python (Join-Path $PSScriptRoot 'SummarizeEnemyUIProfile.py') $EvidenceRoot
if ($LASTEXITCODE -ne 0) { throw 'Profile aggregation rejected its evidence.' }
Write-Output "Completed profile evidence: $EvidenceRoot"
