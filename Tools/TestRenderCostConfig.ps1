# Small Windows PowerShell 5.1 regression suite; no Pester or UE installation needed.
$ErrorActionPreference = 'Stop'
$Runner = Join-Path $PSScriptRoot 'RunRenderCostMatrix.ps1'
$Profile = Join-Path $PSScriptRoot 'ExperimentProfiles\baseline160.json'
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('RenderCostConfigTests_' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $TestRoot
$Passed = 0
function Assert-That([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Write-Config([string]$Json) {
    $Path = Join-Path $TestRoot 'input.json'
    Set-Content -LiteralPath $Path -Value $Json -Encoding UTF8
    return $Path
}
function Validate([hashtable]$Parameters = @{}) {
    function Start-Process { throw 'TEST FORBIDS process launch.' }
    function Get-Process { throw 'TEST FORBIDS process queries.' }
    function New-Item { throw 'TEST FORBIDS evidence creation.' }
    function Set-Content { throw 'TEST FORBIDS evidence writes.' }
    return (& $Runner -ValidateOnly @Parameters | ConvertFrom-Json)
}
function Check([string]$Name, [scriptblock]$Body) { & $Body; $script:Passed++; Write-Output "PASS $Name" }
try {
    Check 'legacy defaults are unchanged' {
        $Report = Validate
        $Expected = [ordered]@{
            Counts = @(20, 50, 100, 160); RunsPerCase = 3; WarmupSeconds = 15; DurationSeconds = 30
            ScreenPercentage = $null; PlayerHealth = 1000000; BenchmarkSeed = 1337
            VariantNames = @('Baseline', 'EnemyRayTracingOff', 'EnemyShadowsOff'); ExpectedAvoidanceMode = 'Any'
            CaptureTaskTrace = $false; BalancedVariantOrder = $false; StartTrimSeconds = 0; EndTrimSeconds = 0
            Map = '/Game/FactoryDistrict/Maps/Demonstration'
        }
        foreach ($Name in $Expected.Keys) {
            $Actual = $Report.Configuration.Settings.$Name
            $MatchesDefault = if ($null -eq $Expected[$Name]) { $null -eq $Actual } else { ($Actual -join ',') -ceq ($Expected[$Name] -join ',') }
            Assert-That $MatchesDefault "Legacy default changed: $Name"
        }
        Assert-That ($null -eq $Report.Configuration.InputConfig) 'Unexpected input config.'
    }
    Check 'baseline160 preserves normal rendering and records input SHA256' {
        $Report = Validate @{ ConfigFile = $Profile }
        $Settings = $Report.Configuration.Settings
        Assert-That (($Settings.Counts -join ',') -eq '160' -and ($Settings.VariantNames -join ',') -eq 'Baseline') 'Wrong baseline selection.'
        Assert-That ($null -eq $Settings.ScreenPercentage -and -not $Settings.CaptureTaskTrace) 'Baseline acquired a diagnostic override.'
        Assert-That ($Report.Configuration.InputConfig.SHA256 -eq (Get-FileHash -LiteralPath $Profile -Algorithm SHA256).Hash) 'Input hash mismatch.'
    }
    Check 'CLI (including false/null/original defaults) beats JSON; JSON beats defaults' {
        $Path = Write-Config '{"SchemaVersion":1,"Counts":[160,20,160],"RunsPerCase":2,"CaptureTaskTrace":true,"BalancedVariantOrder":true,"ScreenPercentage":75}'
        $Report = Validate @{ ConfigFile = $Path }
        Assert-That ($Report.Configuration.Settings.RunsPerCase -eq 2 -and $Report.Configuration.Settings.CaptureTaskTrace) 'JSON did not win.'
        Assert-That (($Report.Configuration.Settings.Counts -join ',') -eq '20,160') 'Count normalization changed.'
        Assert-That ($Report.Configuration.Sources.DurationSeconds -eq 'Default') 'Omitted JSON value lost default.'
        $Report = Validate @{ ConfigFile = $Path; RunsPerCase = 3; CaptureTaskTrace = $false; BalancedVariantOrder = $false; ScreenPercentage = $null }
        Assert-That ($Report.Configuration.Settings.RunsPerCase -eq 3 -and $Report.Configuration.Sources.RunsPerCase -eq 'CommandLine') 'Explicit original default lost.'
        Assert-That (-not $Report.Configuration.Settings.CaptureTaskTrace -and -not $Report.Configuration.Settings.BalancedVariantOrder -and $null -eq $Report.Configuration.Settings.ScreenPercentage) 'Explicit false/null lost.'
    }
    Check 'DryRun needs no UE/project and creates no evidence' {
        $MissingRoot = Join-Path $TestRoot 'MissingProject'
        $Report = & $Runner -DryRun -ProjectRoot $MissingRoot -EditorPath 'NoUE.exe' -Map '/Game/Regression/Missing.Missing?listen' | ConvertFrom-Json
        Assert-That ($Report.Mode -eq 'ValidateOnly' -and -not (Test-Path -LiteralPath $MissingRoot)) 'DryRun wrote output or did not validate.'
    }
    Check 'ValidateOnly rejects an empty CLI variant selection even over a preset' {
        $Caught = $null
        try { $null = Validate @{ ConfigFile = $Profile; VariantNames = @() } }
        catch { $Caught = $_.Exception.Message }
        Assert-That ($Caught -eq 'VariantNames did not select any supported test variant.') 'Empty variants passed validation.'
    }
    Check 'merged floating-point settings reject nonfinite CLI overrides' {
        foreach ($Name in @('WarmupSeconds', 'DurationSeconds', 'PlayerHealth', 'StartTrimSeconds', 'EndTrimSeconds', 'ScreenPercentage')) {
            foreach ($Number in @([double]::NaN, [double]::PositiveInfinity, [double]::NegativeInfinity)) {
                $Parameters = @{ ConfigFile = $Profile }
                $Parameters[$Name] = $Number
                $Caught = $null
                try { $null = Validate $Parameters }
                catch { $Caught = $_.Exception.Message }
                Assert-That ($Caught -eq "$Name must be finite.") "Nonfinite $Name passed validation or failed for another reason."
            }
        }
    }
    $MapProject = Join-Path $TestRoot 'MapProject'
    $MapDirectory = Join-Path $MapProject 'Content\Regression'
    $null = New-Item -ItemType Directory -Path $MapDirectory -Force
    Set-Content -LiteralPath (Join-Path $MapProject 'fpstrue.uproject') -Value '{}'
    $SelectedMap = Join-Path $MapDirectory 'Selected.umap'
    Set-Content -LiteralPath $SelectedMap -Value 'Test map fingerprint input; never loaded by UE.'
    Check 'fingerprints the selected package with optional CLI object/travel suffix' {
        function Start-Process { throw 'TEST FORBIDS process launch.' }
        function Get-Process { throw 'TEST FORBIDS process queries.' }
        # Stop at the first fingerprint read, before any machine or process queries.
        function Get-FileHash { param([string]$LiteralPath, [string]$Algorithm) throw "TEST fingerprint: $LiteralPath" }
        $Index = 0
        foreach ($Map in @('/Game/Regression/Selected', '/Game/Regression/Selected.Selected?listen')) {
            $Caught = $null
            try { $null = & $Runner -ProjectRoot $MapProject -EditorPath $Runner -Map $Map -RunName "Fingerprint$Index" }
            catch { $Caught = $_.Exception.Message }
            Assert-That ($Caught -eq "TEST fingerprint: $SelectedMap") "The selected map was not the fingerprint input: $Caught"
            $Index++
        }
    }
    Check 'missing selected map is rejected before evidence creation or launch' {
        function Start-Process { throw 'TEST FORBIDS process launch.' }
        function Get-Process { throw 'TEST FORBIDS process queries.' }
        function New-Item { throw 'TEST FORBIDS evidence creation.' }
        $Caught = $null
        try { $null = & $Runner -ProjectRoot $MapProject -EditorPath $Runner -Map '/Game/Regression/Missing' -RunName 'MissingMap' }
        catch { $Caught = $_.Exception.Message }
        Assert-That ($Caught -like 'Selected map was not found:*Missing.umap') 'Missing map reached evidence creation or launch.'
    }
    $BadInputs = @(
        '{"SchemaVersion":1', '{"SchemaVersion":2}', '{"SchemaVersion":"1"}', '{}', '[]',
        '{"SchemaVersion":1,"ExecCmds":"quit"}', '{"SchemaVersion":1,"EditorPath":"other.exe"}',
        '{"SchemaVersion":1,"Counts":160}', '{"SchemaVersion":1,"Counts":[]}', '{"SchemaVersion":1,"Counts":[-1]}',
        '{"SchemaVersion":1,"Counts":[1.5]}', '{"SchemaVersion":1,"RunsPerCase":0}',
        '{"SchemaVersion":1,"DurationSeconds":"30"}', '{"SchemaVersion":1,"WarmupSeconds":-1}',
        '{"SchemaVersion":1,"ScreenPercentage":101}', '{"SchemaVersion":1,"CaptureTaskTrace":"false"}',
        '{"SchemaVersion":1,"VariantNames":["Unknown"]}', '{"SchemaVersion":1,"ExpectedAvoidanceMode":"None"}',
        '{"SchemaVersion":1,"Map":"/Game/Map -ExecCmds=quit"}', '{"SchemaVersion":1,"Map":"/Game/Map.Map?listen"}',
        '{"SchemaVersion":1,"StartTrimSeconds":30}'
    )
    foreach ($Json in $BadInputs) {
        Check "reject $Json before machine checks/launch" {
            $Path = Write-Config $Json
            $Caught = $null
            # Explicit RunsPerCase=3 also proves invalid JSON values are checked even when overridden.
            try { $null = & $Runner -ConfigFile $Path -RunsPerCase 3 -EditorPath 'NoUE.exe' }
            catch { $Caught = $_.Exception.Message }
            Assert-That ($null -ne $Caught -and $Caught -notmatch 'Unreal Editor was not found') 'Invalid config reached machine checks.'
        }
    }
} finally {
    $ExactRoot = [IO.Path]::GetFullPath($TestRoot)
    $TempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($ExactRoot.StartsWith($TempParent, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($ExactRoot) -match '\ARenderCostConfigTests_[0-9a-f]{32}\z') {
        Remove-Item -LiteralPath $ExactRoot -Recurse -Force
    }
}
Write-Output "$Passed configuration checks passed. No Unreal processes launched."
