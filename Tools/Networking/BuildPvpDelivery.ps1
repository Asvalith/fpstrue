<#
One entry for build, automation, network smoke and delivery evidence.
Editor mode is local validation, NOT a packaged dedicated-server release.
Delivery requires a Server-capable UE 5.5 build and sufficient disk space.
LinuxPackage builds a matched Win64 Game/Linux Server pair; runtime networking
is a separate RemoteLinux gate, never inferred from a successful cook.
No engine downloads, firewall changes, cloud provisioning or process stealing.
#>
[CmdletBinding()]
param(
    [string]$EngineRoot = $env:UE_ROOT,
    [string]$ProjectRoot = '',
    [ValidateSet('Editor', 'Delivery', 'LinuxPackage')][string]$Mode = 'Delivery',
    [string]$CookEngineRoot = '',
    [string]$LinuxToolchainRoot = '',
    [string]$OutputRoot = '',
    [ValidateSet('Full', 'LineTablesOnly', 'None')][string]$ServerDebugInfo = 'Full',
    [switch]$IncludeShipping,
    [switch]$RequireClean,
    [switch]$PlanOnly,
    [ValidateRange(1, 65534)][int]$Port = 7797,
    [ValidateRange(1, 10000)][int]$MinimumTests = 75,
    [ValidateRange(1, 16)][int]$MaxParallelActions = 4,
    [ValidateRange(1, 2000)][int]$MinimumDeliveryFreeGB = 50
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = Join-Path $PSScriptRoot '..\..' }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not $EngineRoot) { throw 'Pass -EngineRoot or set UE_ROOT to a UE 5.5 engine directory.' }
$EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
if ($Mode -eq 'LinuxPackage') {
    if (-not $CookEngineRoot) { $CookEngineRoot = $EngineRoot }
    $CookEngineRoot = (Resolve-Path -LiteralPath $CookEngineRoot).Path
    if (-not $LinuxToolchainRoot) { throw 'LinuxPackage requires -LinuxToolchainRoot pointing to an existing UE 5.5 v23 toolchain.' }
    $LinuxToolchainRoot = (Resolve-Path -LiteralPath $LinuxToolchainRoot).Path
} else { $CookEngineRoot = $EngineRoot }
$Project = Join-Path $ProjectRoot 'fpstrue.uproject'
$Build = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$CookBuild = Join-Path $CookEngineRoot 'Engine\Build\BatchFiles\Build.bat'
$Uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$Editor = Join-Path $CookEngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Map = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked'
$RunId = (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
if (-not $OutputRoot) { $OutputRoot = Join-Path $ProjectRoot 'Saved\Delivery' }
if (-not [IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot = Join-Path $ProjectRoot $OutputRoot }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$Output = Join-Path $OutputRoot $RunId
$OriginalLinuxMultiarchRoot = [Environment]::GetEnvironmentVariable('LINUX_MULTIARCH_ROOT', 'Process')
$OriginalLinuxRoot = [Environment]::GetEnvironmentVariable('LINUX_ROOT', 'Process')
$ToolchainEnvironmentChanged = $false
# Bound this invocation's compiler pressure without changing shared engine
# settings or the system page file. The same policy applies to UAT builds.
$BuildExecutionArguments = @(('-MaxParallelActions=' + $MaxParallelActions), '-NoUBA', '-NoUBALocal')
$UatBuildExecutionArgument = '-ubtargs="' + ($BuildExecutionArguments -join ' ') + '"'
$Steps = [Collections.Generic.List[object]]::new()
$Artifacts = [Collections.Generic.List[object]]::new()
$Report = [ordered]@{
    SchemaVersion = 1; RunId = $RunId; Mode = $Mode; Passed = $false
    StartedUtc = [DateTime]::UtcNow.ToString('o'); FinishedUtc = $null
    Commit = $null; Dirty = $true; ReproducibleCheckout = $false
    Engine = $null; ServerBuildSupported = $false; Failure = $null
    Automation = $null; Steps = $Steps; Artifacts = $Artifacts
    BuildExecutionArguments = $BuildExecutionArguments
    Scope = 'Local build/functional acceptance only; not cross-machine, visual, capacity or long-soak certification.'
}
if ($Mode -eq 'LinuxPackage') {
    $Report.SchemaVersion = 2
    $Report.Scope = 'Build/cook/stage/archive acceptance only. Linux Server is not executed on Windows; RunPvpSmoke -ServerMode RemoteLinux must independently verify networking. No visual, capacity or long-soak certification.'
    $Report['RuntimeVerified'] = $false
    $Report['NetworkVersionOverrideUsed'] = $false
    $Report['ServerDebugInfoRequested'] = $ServerDebugInfo
    $Report['CookEngine'] = $null
    $Report['LinuxToolchain'] = $null
    $Report['DiskSpace'] = @()
    $Report['BuildIdentityVerified'] = $false
    $Report['CookedSerializationPolicy'] = 'Tagged properties and versioned cooked packages; compatible patch numbers alone do not prove matching native property layouts.'
}

# Keep native execution and exit-code checking together; a printed PASS is not enough.
function Invoke-DeliveryStep([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $Step = [ordered]@{ Name = $Name; Passed = $false; Log = (Join-Path $Output ($Name + '.log')) }
    $Steps.Add($Step)
    Write-Host ('Starting: ' + $Name)
    & $Executable @Arguments 2>&1 | Out-File -LiteralPath $Step.Log -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE). Read $($Step.Log)" }
    $Step.Passed = $true
    Write-Host ('Passed: ' + $Name)
}

# UAT may omit .target receipts when staging. Copy only the receipt of the exact
# executable just built, and compare its hash before treating it as provenance.
function Find-PackagedExecutable([string]$Archive, [string]$Target, [string]$Configuration) {
    $Name = if ($Configuration -eq 'Shipping') { "$Target-Win64-Shipping" } else { $Target }
    $SourceExe = Join-Path $ProjectRoot ("Binaries\Win64\$Name.exe")
    $Receipt = Join-Path $ProjectRoot ("Binaries\Win64\$Name.target")
    $Candidates = @(Get-ChildItem -LiteralPath $Archive -Recurse -File -Filter ($Name + '.exe') |
        Where-Object { $_.DirectoryName -match '[\\/]Binaries[\\/]Win64$' })
    if ($Candidates.Count -ne 1) { throw "Expected one direct $Name executable in $Archive, found $($Candidates.Count)." }
    $Packaged = $Candidates[0].FullName
    if ((Get-FileHash -LiteralPath $Packaged).Hash -ne (Get-FileHash -LiteralPath $SourceExe).Hash) {
        throw "Staged executable differs from the current build: $Packaged"
    }
    $ReceiptData = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json
    if ($ReceiptData.Configuration -ne $Configuration -or $ReceiptData.TargetName -ne $Target) {
        throw "Wrong build receipt: $Receipt"
    }
    Copy-Item -LiteralPath $Receipt -Destination (Join-Path (Split-Path $Packaged) ($Name + '.target')) -Force
    $Symbols = Save-DeliverySymbols $Name $Configuration
    $Artifacts.Add([pscustomobject]@{
        Target = $Target; Configuration = $Configuration; Executable = $Packaged
        SHA256 = (Get-FileHash -LiteralPath $Packaged).Hash; Receipt = $ReceiptData; Symbols = $Symbols
    })
    return $Packaged
}

# Keep the symbols of this build, not only a path that the next build overwrites.
function Save-DeliverySymbols([string]$Name, [string]$Configuration) {
    $Source = Join-Path $ProjectRoot ("Binaries\Win64\$Name.pdb")
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing crash symbols: $Source" }
    $Destination = Join-Path $Output ('Symbols\' + $Configuration)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $Saved = Join-Path $Destination ($Name + '.pdb')
    Copy-Item -LiteralPath $Source -Destination $Saved
    return @{ Path = $Saved; SHA256 = (Get-FileHash -LiteralPath $Saved).Hash }
}

# GNU readelf can return success and the correct build-id for an objcopy
# --only-keep-debug file while complaining that its omitted interpreter bytes
# cannot be read. Capture native diagnostics explicitly under Windows PS 5;
# allow only that exact separate-debug diagnostic, not other read errors.
function Read-DeliveryElfBuildId([string]$Reader, [string]$Path, [bool]$SeparateDebug) {
    $PreviousErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $Messages = @(& $Reader '-n' $Path 2>&1)
        $ReaderExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $PreviousErrorPreference }
    if ($ReaderExit -ne 0) { throw "Cannot read ELF build-id (exit $ReaderExit): $Path" }
    foreach ($Message in $Messages) {
        if ($Message -is [System.Management.Automation.ErrorRecord]) {
            $Diagnostic = $Message.Exception.Message.Trim()
            if (-not $SeparateDebug -or $Diagnostic -ne 'readelf: Error: Unable to find program interpreter name') {
                throw "Unexpected ELF reader diagnostic for ${Path}: $Diagnostic"
            }
        }
    }
    $Match = [regex]::Match(($Messages | Out-String), 'Build ID:\s*([0-9a-fA-F]+)')
    if (-not $Match.Success) { throw "ELF file has no readable build-id: $Path" }
    return $Match.Groups[1].Value.ToLowerInvariant()
}

# LinuxPackage deliberately uses the same source engine for BOTH runtime targets.
# Match the actual archived binary to its original receipt; never substitute the
# installed-engine client simply because the editor/cooker is ABI-compatible.
function Find-CrossPlatformExecutable([string]$Archive, [string]$Target, [string]$Platform) {
    $Extension = if ($Platform -eq 'Win64') { '.exe' } else { '' }
    $Source = Join-Path $ProjectRoot ("Binaries\$Platform\$Target$Extension")
    $Receipt = Join-Path $ProjectRoot ("Binaries\$Platform\$Target.target")
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or -not (Test-Path -LiteralPath $Receipt -PathType Leaf)) {
        throw "Missing $Platform runtime binary/receipt: $Source"
    }
    $Data = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json
    $ExpectedType = if ($Platform -eq 'Linux') { 'Server' } else { 'Game' }
    if ($Data.TargetName -ne $Target -or $Data.Platform -ne $Platform -or $Data.Configuration -ne 'Development' -or $Data.TargetType -ne $ExpectedType) {
        throw "Wrong runtime target receipt: $Receipt"
    }
    foreach ($Field in @('MajorVersion', 'MinorVersion', 'PatchVersion', 'Changelist', 'CompatibleChangelist', 'IsLicenseeVersion', 'BranchName')) {
        if ($Data.Version.$Field -ne $Report.Engine.$Field) { throw "Runtime receipt is not from the selected source engine ($Field): $Receipt" }
    }
    $Candidates = @(Get-ChildItem -LiteralPath $Archive -Recurse -File -Filter ($Target + $Extension) |
        Where-Object { $_.DirectoryName -match ('[\\/]Binaries[\\/]' + $Platform + '$') })
    if ($Candidates.Count -ne 1) { throw "Expected one direct $Platform $Target executable, found $($Candidates.Count) in $Archive." }
    $Packaged = $Candidates[0].FullName
    $Hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if ($Hash -ne (Get-FileHash -LiteralPath $Packaged -Algorithm SHA256).Hash) { throw "Archived binary differs from its current build: $Packaged" }
    if ($Platform -eq 'Linux') {
        $Reader = [IO.File]::OpenRead($Packaged)
        try {
            $Header = New-Object byte[] 20
            if ($Reader.Read($Header, 0, 20) -ne 20 -or $Header[0] -ne 127 -or $Header[1] -ne 69 -or $Header[2] -ne 76 -or $Header[3] -ne 70 -or $Header[4] -ne 2 -or $Header[18] -ne 62) {
                throw "Expected an actual Linux x86_64 ELF server, not a launcher or Windows executable: $Packaged"
            }
        } finally { $Reader.Dispose() }
    }
    $PackagedReceipt = Join-Path (Split-Path $Packaged) ($Target + '.target')
    Copy-Item -LiteralPath $Receipt -Destination $PackagedReceipt -Force
    $Symbols = @()
    $SymbolDirectory = Join-Path $Output ("Symbols\$Platform\Development")
    $Extensions = if ($Platform -eq 'Win64') { @('.pdb') } else { @('.debug', '.sym', '.symbols', '.psym') }
    foreach ($SymbolExtension in $Extensions) {
        $Symbol = Join-Path (Split-Path $Source) ($Target + $SymbolExtension)
        if (-not (Test-Path -LiteralPath $Symbol -PathType Leaf)) { continue }
        $Product = @(Get-Item -LiteralPath $Symbol)[0]
        if ($Product.Length -eq 0) { throw "Empty debug symbol output: $Symbol" }
        # A prior Full build may leave .debug beside a later None build. Require
        # ELF build-id agreement before retaining it as this binary's symbols.
        $BuildIdMatched = $null
        if ($SymbolExtension -eq '.debug') {
            $ReadElf = Join-Path $LinuxToolchainRoot 'x86_64-unknown-linux-gnu\bin\x86_64-unknown-linux-gnu-readelf.exe'
            $BinaryId = Read-DeliveryElfBuildId $ReadElf $Source $false
            $DebugId = Read-DeliveryElfBuildId $ReadElf $Symbol $true
            $BuildIdMatched = $BinaryId -eq $DebugId
            if (-not $BuildIdMatched) {
                if ($ServerDebugInfo -ne 'None') { throw "Linux debug file does not match this executable: $Symbol" }
                continue
            }
        }
        New-Item -ItemType Directory -Path $SymbolDirectory -Force | Out-Null
        $Saved = Join-Path $SymbolDirectory $Product.Name
        Copy-Item -LiteralPath $Symbol -Destination $Saved
        $Symbols += [pscustomobject]@{ Path = $Saved; SHA256 = (Get-FileHash -LiteralPath $Saved -Algorithm SHA256).Hash; Bytes = $Product.Length; Extension = $SymbolExtension; ELFBuildIdMatched = $BuildIdMatched }
    }
    if ($Platform -eq 'Win64' -and $Symbols.Count -eq 0) { throw "Missing Win64 crash PDB for $Target." }
    if ($Platform -eq 'Linux' -and $ServerDebugInfo -ne 'None' -and @($Symbols | Where-Object Extension -eq '.debug').Count -eq 0) {
        throw "Requested $ServerDebugInfo Linux debug information but no matching .debug file was produced."
    }
    $Artifacts.Add([pscustomobject]@{
        Target = $Target; Platform = $Platform; Configuration = 'Development'; Executable = $Packaged
        SHA256 = $Hash; ReceiptPath = $PackagedReceipt; ReceiptSHA256 = (Get-FileHash -LiteralPath $PackagedReceipt -Algorithm SHA256).Hash
        Receipt = $Data; Symbols = $Symbols; DebugInfoRequested = $(if ($Platform -eq 'Linux') { $ServerDebugInfo } else { 'Full' })
        RuntimeVerified = $false
    })
    return $Packaged
}

try {
    foreach ($Required in @($Project, $Build, $CookBuild, $Uat, $Editor)) {
        if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) { throw "Missing: $Required" }
    }
    $Version = Get-Content -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') -Raw | ConvertFrom-Json
    $Report.Engine = $Version
    if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 5) { throw 'This pipeline is validated for UE 5.5 only.' }
    $Report.Commit = (& git -C $ProjectRoot rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
    $Changes = @(& git -C $ProjectRoot status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect working tree.' }
    $Report.Dirty = $Changes.Count -gt 0
    $Report.ReproducibleCheckout = -not $Report.Dirty
    $Installed = Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Build\InstalledBuild.txt')
    $BaseIni = Get-Content -LiteralPath (Join-Path $EngineRoot 'Engine\Config\BaseEngine.ini') -Raw
    # Advisory preflight. UBT remains authoritative for custom Installed Builds.
    $Report.ServerBuildSupported = -not $Installed -or $BaseIni -match 'InstalledPlatformConfigurations=\([^\r\n]*PlatformName="Win64"[^\r\n]*PlatformType="Server"'
    if ($Mode -eq 'LinuxPackage') {
        if ($Installed) { throw 'LinuxPackage requires a source engine for both runtime targets; a cooker-only installed build is allowed only as -CookEngineRoot.' }
        $CookVersion = Get-Content -LiteralPath (Join-Path $CookEngineRoot 'Engine\Build\Build.version') -Raw | ConvertFrom-Json
        foreach ($Field in @('MajorVersion', 'MinorVersion', 'PatchVersion', 'CompatibleChangelist', 'IsLicenseeVersion')) {
            if ($CookVersion.$Field -ne $Version.$Field) { throw "Cooker and runtime engine are not compatible ($Field)." }
        }
        $Report.CookEngine = @{ Root = $CookEngineRoot; Version = $CookVersion }
        foreach ($Required in @((Join-Path $EngineRoot 'Engine\Source\Programs\UnrealBuildTool\Configuration\TargetRules.cs'),
            (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealPak.exe'),
            (Join-Path $EngineRoot 'Engine\Binaries\DotNET\AutomationTool\AutomationTool.dll'),
            (Join-Path $ProjectRoot 'Source\fpstrueServer.Target.cs'),
            (Join-Path $LinuxToolchainRoot 'x86_64-unknown-linux-gnu\bin\clang++.exe'),
            (Join-Path $LinuxToolchainRoot 'x86_64-unknown-linux-gnu\bin\x86_64-unknown-linux-gnu-readelf.exe'))) {
            if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) { throw "LinuxPackage prerequisite missing: $Required" }
        }
        $ToolchainVersion = (Get-Content -LiteralPath (Join-Path $LinuxToolchainRoot 'ToolchainVersion.txt') -Raw).Trim()
        if ($ToolchainVersion -ne 'v23_clang-18.1.0-rockylinux8') { throw "Expected the UE 5.5 v23 Linux toolchain, found: $ToolchainVersion" }
        $Report.LinuxToolchain = @{ Root = $LinuxToolchainRoot; Version = $ToolchainVersion; EnvironmentScope = 'Process only; previous values restored in finally.' }
        if ($IncludeShipping) { throw 'LinuxPackage currently produces the matched Development pair only; -IncludeShipping belongs to Win64 Delivery.' }
        $MinimumTests = [Math]::Max($MinimumTests, 75)
        $ServerDebugArguments = switch ($ServerDebugInfo) {
            'Full' { @('-DebugInfo=Full', '-DebugInfoLineTablesOnly=None') }
            'LineTablesOnly' { @('-DebugInfo=Full', '-DebugInfoLineTablesOnly=Full') }
            'None' { @('-DebugInfo=None', '-DebugInfoLineTablesOnly=None') }
        }
        $Report['ServerDebugBuildArguments'] = @($ServerDebugArguments)
    }
    if ($PlanOnly) {
        $Stages = if ($Mode -eq 'LinuxPackage') {
            @('Compatible cooker Editor build', ('Automation JSON gate (at least ' + $MinimumTests + ' tests)'),
                'Source-engine Win64 Game Development build', 'Same-source-engine Linux Server Development build',
                'Source UAT + explicit cooker: isolated Win64/LinuxServer cook, stage, pak, archive',
                'Binary/receipt/hash/build-id symbol inventory (NO runtime or network PASS)')
        } else { @('Editor build', 'Automation JSON gate', 'Game Development + Shipping compile / Development packages',
                'Normal + impaired two-client network smoke', 'Optional Shipping packages', 'Report + binary/symbol inventory') }
        [pscustomobject]@{
            Mode = $Mode; Engine = $EngineRoot; Commit = $Report.Commit; Dirty = $Report.Dirty
            ServerBuildSupported = $Report.ServerBuildSupported; Output = $Output
            CookEngine = $CookEngineRoot; LinuxToolchain = $LinuxToolchainRoot; MinimumTests = $MinimumTests
            MaxParallelActions = $MaxParallelActions; BuildExecutionArguments = $BuildExecutionArguments
            ServerDebugInfoRequested = $ServerDebugInfo; ServerDebugBuildArguments = @($ServerDebugArguments)
            Stages = $Stages
            IncludeShipping = [bool]$IncludeShipping
            Scope = $Report.Scope
        } | ConvertTo-Json -Depth 4
        return
    }
    New-Item -ItemType Directory -Path $Output | Out-Null
    if ($RequireClean -and $Report.Dirty) { throw 'Release/CI requires a clean checkout, including untracked files.' }
    if ($Mode -eq 'Editor' -and $IncludeShipping) { throw '-IncludeShipping packages require -Mode Delivery.' }
    if ($Mode -eq 'Delivery' -and -not $Report.ServerBuildSupported) {
        throw 'This installed UE distribution does not support Win64 Server. Use a Server-capable UE 5.5 build; Editor -server is not a packaged server.'
    }
    if ($Mode -eq 'Delivery') {
        foreach ($Root in @($ProjectRoot, $Output) | Select-Object -Unique) {
            $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root))
            if ($Drive.AvailableFreeSpace / 1GB -lt $MinimumDeliveryFreeGB) {
                throw "Delivery requires at least $MinimumDeliveryFreeGB GB free on $($Drive.Name). Packages and symbols are retained, never silently deleted."
            }
        }
    }
    if ($Mode -eq 'LinuxPackage') {
        # Cooks, archives and retained symbols live under the selected output.
        # Other writable roots retain a 10 GB floor; this is a safety floor, not
        # a promise that a full source-engine rebuild will fit on that volume.
        $WritableRoots = @($OutputRoot, $ProjectRoot, $EngineRoot, $CookEngineRoot) | Select-Object -Unique
        foreach ($Root in $WritableRoots) {
            $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root))
            $MinimumGB = if ([IO.Path]::GetPathRoot($Root) -eq [IO.Path]::GetPathRoot($OutputRoot)) { $MinimumDeliveryFreeGB } else { 10 }
            $FreeGB = [Math]::Round($Drive.AvailableFreeSpace / 1GB, 2)
            $Report.DiskSpace += [pscustomobject]@{ Path = $Root; Drive = $Drive.Name; FreeGB = $FreeGB; MinimumGB = $MinimumGB }
            if ($FreeGB -lt $MinimumGB) { throw "LinuxPackage requires $MinimumGB GB free on $($Drive.Name); only $FreeGB GB remains. No data was deleted." }
        }
    }
    # Refuse Git LFS pointers before invoking Unreal. Missing binary contents must
    # fail here, not appear later as misleading asset/cook failures.
    foreach ($Asset in Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'Content') -File -Recurse |
        Where-Object { $_.Extension -in @('.uasset', '.umap') -and $_.Length -lt 1024 }) {
        if ((Get-Content -LiteralPath $Asset.FullName -TotalCount 1) -eq 'version https://git-lfs.github.com/spec/v1') {
            throw "Git LFS content not fetched: $($Asset.FullName)"
        }
    }
    if ($Mode -eq 'LinuxPackage') {
        [Environment]::SetEnvironmentVariable('LINUX_MULTIARCH_ROOT', $LinuxToolchainRoot, 'Process')
        [Environment]::SetEnvironmentVariable('LINUX_ROOT', $null, 'Process')
        $ToolchainEnvironmentChanged = $true
        # NoZenLocalFallback selects a file backend without rewriting global
        # settings. Its path still follows existing registry/Editor overrides;
        # do not report an environment request as the actual cache location.
        $Report['CookerCachePolicy'] = 'NoZenLocalFallback; actual writable cache path is recorded in the full cooker log and may follow existing registry/Editor settings.'
    }
    $BuildArgs = @('Win64', 'Development', ('-Project=' + $Project), '-WaitMutex', '-NoHotReloadFromIDE') + $BuildExecutionArguments
    Invoke-DeliveryStep 'BuildEditor' $CookBuild (@('fpstrueEditor') + $BuildArgs)
    $AutomationDir = Join-Path $Output 'Automation'
    $AutomationLog = Join-Path $Output 'AutomationEngine.log'
    Invoke-DeliveryStep 'AutomationRunner' $Editor @(
        $Project, '-Unattended', '-NullRHI', '-NoSound', '-NoSplash', '-NoSteam', '-NoLiveCoding',
        '-ddc=NoZenLocalFallback', ('-LocalDataCachePath=' + (Join-Path $ProjectRoot 'Saved\LocalDDC')),
        '-ExecCmds=Automation RunTests fpstrue.', '-TestExit=Automation Test Queue Empty',
        ('-ReportExportPath=' + $AutomationDir), ('-abslog=' + $AutomationLog))
    $Automation = Get-Content -LiteralPath (Join-Path $AutomationDir 'index.json') -Raw | ConvertFrom-Json
    $Count = [int]$Automation.succeeded + [int]$Automation.succeededWithWarnings
    if ($Count -lt $MinimumTests -or $Automation.failed -ne 0 -or $Automation.notRun -ne 0 -or $Automation.inProcess -ne 0) {
        throw "Automation gate failed: passed=$Count, failed=$($Automation.failed), notRun=$($Automation.notRun)."
    }
    $Report.Automation = @{ Passed = $Count; Warnings = $Automation.succeededWithWarnings; Report = (Join-Path $AutomationDir 'index.json') }
    if ($Mode -eq 'LinuxPackage') {
        # The installed editor may cook content, but its Game executable must
        # never be substituted for the source-engine client paired with Server.
        Invoke-DeliveryStep 'BuildSourceGameDevelopment' $Build (@('fpstrue') + $BuildArgs + @('-DebugInfo=Full', '-DebugInfoLineTablesOnly=None'))
        Invoke-DeliveryStep 'BuildLinuxServerDevelopment' $Build (@('fpstrueServer', 'Linux', 'Development',
            ('-Project=' + $Project), '-WaitMutex', '-NoHotReloadFromIDE') + $BuildExecutionArguments + $ServerDebugArguments)
        $ClientArchive = Join-Path $Output 'Packages\Win64'
        $ServerArchive = Join-Path $Output 'Packages\LinuxServer'
        $CrossCommon = @('-nocompile', 'BuildCookRun', ('-project=' + $Project), '-nop4', '-unattended', '-utf8output',
            '-skipbuild', '-skipbuildeditor', '-nocompileeditor', '-cook', '-stage', '-pak', '-package', '-archive', '-nodebuginfo',
            '-ddc=NoZenLocalFallback', '-VersionCookedContent', ('-unrealexe=' + $Editor), ('-map=' + $Map), $UatBuildExecutionArgument)
        Invoke-DeliveryStep 'PackageSourceGameWin64' $Uat ($CrossCommon + @('-platform=Win64', '-clientconfig=Development',
            ('-CookOutputDir=' + (Join-Path $Output 'Cooked\Windows')),
            ('-stagingdirectory=' + (Join-Path $Output 'Staging\Win64')), ('-archivedirectory=' + $ClientArchive)))
        Invoke-DeliveryStep 'PackageSourceServerLinux' $Uat ($CrossCommon + @('-platform=Linux', '-serverplatform=Linux',
            '-server', '-noclient', '-serverconfig=Development',
            ('-CookOutputDir=' + (Join-Path $Output 'Cooked\LinuxServer')),
            ('-stagingdirectory=' + (Join-Path $Output 'Staging\LinuxServer')), ('-archivedirectory=' + $ServerArchive)))
        $ClientExe = Find-CrossPlatformExecutable $ClientArchive 'fpstrue' 'Win64'
        $ServerExe = Find-CrossPlatformExecutable $ServerArchive 'fpstrueServer' 'Linux'
        $Report.BuildIdentityVerified = $true
        $Report['ClientExecutable'] = $ClientExe
        $Report['ServerExecutable'] = $ServerExe
        $Report['Symbols'] = @($Artifacts | ForEach-Object { $_.Symbols })
        $Report['RuntimeValidationRequired'] = 'RunPvpSmoke.ps1 -ExecutionMode Packaged -ServerMode RemoteLinux with this exact Win64 executable and archived Linux ELF SHA256.'
        $Report.Passed = $true
        return
    }
    $SmokeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'RunPvpSmoke.ps1'),
        '-ProjectRoot', $ProjectRoot, '-EngineRoot', $EngineRoot)
    if ($Mode -eq 'Editor') {
        Invoke-DeliveryStep 'BuildGameDevelopment' $Build (@('fpstrue') + $BuildArgs)
        Invoke-DeliveryStep 'BuildGameShipping' $Build (@('fpstrue', 'Win64', 'Shipping', ('-Project=' + $Project), '-WaitMutex', '-NoHotReloadFromIDE') + $BuildExecutionArguments)
        $SmokeArgs += @('-ExecutionMode', 'Editor')
    }
    else {
        $Common = @('BuildCookRun', ('-project=' + $Project), '-nop4', '-unattended', '-utf8output', '-platform=Win64',
            '-build', '-cook', '-stage', '-pak', '-package', '-archive', ('-map=' + $Map), $UatBuildExecutionArgument)
        $DevArchive = Join-Path $Output 'Development'
        Invoke-DeliveryStep 'PackageDevelopment' $Uat ($Common + @('-clientconfig=Development', '-serverconfig=Development',
            '-server', '-serverplatform=Win64', ('-archivedirectory=' + $DevArchive)))
        $ServerExe = Find-PackagedExecutable $DevArchive 'fpstrueServer' 'Development'
        $ClientExe = Find-PackagedExecutable $DevArchive 'fpstrue' 'Development'
        $SmokeArgs += @('-ExecutionMode', 'Packaged', '-ServerExecutable', $ServerExe, '-ClientExecutable', $ClientExe)
    }
    Invoke-DeliveryStep 'NetworkNormal' 'powershell.exe' ($SmokeArgs + @('-Port', [string]$Port))
    Invoke-DeliveryStep 'NetworkImpaired' 'powershell.exe' ($SmokeArgs + @('-Port', [string]($Port + 1),
        '-PacketLagMs', '100', '-PacketLagVarianceMs', '20', '-PacketLossPercent', '2'))
    if ($Mode -eq 'Delivery' -and $IncludeShipping) {
        $ShippingArchive = Join-Path $Output 'Shipping'
        Invoke-DeliveryStep 'PackageShipping' $Uat ($Common + @('-clientconfig=Shipping', '-serverconfig=Shipping',
            '-server', '-serverplatform=Win64', ('-archivedirectory=' + $ShippingArchive)))
        $null = Find-PackagedExecutable $ShippingArchive 'fpstrueServer' 'Shipping'
        $null = Find-PackagedExecutable $ShippingArchive 'fpstrue' 'Shipping'
        # Shipping intentionally has no automated-input hooks. These packages are
        # build/cook verified here, NOT falsely marked as runtime smoke verified.
    }
    # Symbols remain local/internal for crash matching. Never upload licensed
    # Content or debug symbols publicly just because a package was built.
    $Symbols = if ($Mode -eq 'Editor') {
        @((Save-DeliverySymbols 'fpstrue' 'Development'), (Save-DeliverySymbols 'fpstrue-Win64-Shipping' 'Shipping'))
    } else { @($Artifacts | ForEach-Object { $_.Symbols }) }
    $Report['Symbols'] = $Symbols
    $Report.Passed = $true
}
catch {
    $Report.Failure = $_.Exception.Message
    throw
}
finally {
    if ($ToolchainEnvironmentChanged) {
        [Environment]::SetEnvironmentVariable('LINUX_MULTIARCH_ROOT', $OriginalLinuxMultiarchRoot, 'Process')
        [Environment]::SetEnvironmentVariable('LINUX_ROOT', $OriginalLinuxRoot, 'Process')
    }
    if (-not $PlanOnly -and (Test-Path -LiteralPath $Output -PathType Container)) {
        $Report.FinishedUtc = [DateTime]::UtcNow.ToString('o')
        $Report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $Output 'DeliveryReport.json') -Encoding utf8
        Write-Host ('Evidence: ' + (Join-Path $Output 'DeliveryReport.json'))
    }
}
