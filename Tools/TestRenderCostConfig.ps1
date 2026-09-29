# Small Windows PowerShell 5.1 regression suite; no Pester or UE installation needed.
$ErrorActionPreference = 'Stop'
$Runner = Join-Path $PSScriptRoot 'RunRenderCostMatrix.ps1'
$Profile = Join-Path $PSScriptRoot 'ExperimentProfiles\baseline160.json'
. (Join-Path $PSScriptRoot 'ReadRenderCostConfig.ps1')
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
    Check 'active entry, reader and summarizer parse in the current PowerShell host' {
        foreach ($File in @('RunRenderCostMatrix.ps1', 'ReadRenderCostConfig.ps1', 'SummarizeRenderCostMatrix.ps1')) {
            $Tokens = $null
            $Errors = $null
            $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $File), [ref]$Tokens, [ref]$Errors)
            Assert-That ($Errors.Count -eq 0) "Parser/encoding error in $File`: $($Errors.Message -join '; ')"
        }
    }
    Check 'catalog and CLI expose exactly the same preset fields' {
        $Command = Get-Command $Runner
        foreach ($Name in $RenderCostConfigParameters) {
            Assert-That ($Command.Parameters.ContainsKey($Name)) "Catalog setting has no CLI parameter: $Name"
        }
        Assert-That (($RenderCostVariantNames | Sort-Object -Unique).Count -eq $RenderCostVariantNames.Count) 'Duplicate variant names.'
    }
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
        Assert-That ($Settings.Map -eq '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked') 'Review baseline must use the accepted baked map.'
        Assert-That ($null -eq $Settings.ScreenPercentage -and -not $Settings.CaptureTaskTrace) 'Baseline acquired a diagnostic override.'
        Assert-That ($Report.Configuration.InputConfig.SHA256 -eq (Get-FileHash -LiteralPath $Profile -Algorithm SHA256).Hash) 'Input hash mismatch.'
    }
    Check 'TSR history comparison is an accepted isolated variant pair' {
        $Report = Validate @{ Counts = @(160); VariantNames = @('TSRHistory200', 'TSRHistory150'); BalancedVariantOrder = $true }
        Assert-That (($Report.Configuration.Settings.VariantNames -join ',') -eq 'TSRHistory200,TSRHistory150') 'TSR variant selection changed.'
        Assert-That $Report.Configuration.Settings.BalancedVariantOrder 'Balanced order was not preserved.'
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
    Check 'spline ray tracing diagnosis accepts explicit On Off without changing defaults' {
        $Path = Write-Config '{"SchemaVersion":1,"Counts":[0],"VariantNames":["SplineRayTracingOn","SplineRayTracingOff"]}'
        $Report = Validate @{ ConfigFile = $Path }
        Assert-That (($Report.Configuration.Settings.VariantNames -join ',') -eq 'SplineRayTracingOn,SplineRayTracingOff') 'Spline variants rejected.'
        Assert-That ($null -eq $Report.Configuration.Settings.ScreenPercentage) 'Spline test changed screen percentage.'
    }
    Check 'scene acceptance is a single 0/160 run on the accepted map, not the ISM candidate' {
        $Report = Validate @{ ConfigFile = (Join-Path $PSScriptRoot 'ExperimentProfiles\scene-acceptance.json') }
        $Settings = $Report.Configuration.Settings
        Assert-That (($Settings.Counts -join ',') -eq '0,160' -and $Settings.RunsPerCase -eq 1) 'Unexpected acceptance matrix.'
        Assert-That ($Settings.WarmupSeconds -eq 20 -and $Settings.DurationSeconds -eq 180) 'Unexpected capture window.'
        Assert-That ($Settings.Map -eq '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked') 'Unaccepted map became the default.'
        Assert-That (-not $Settings.CaptureTaskTrace -and $null -eq $Settings.ScreenPercentage) 'Acceptance contains a diagnostic override.'
    }
    Check 'unknown CLI variants cannot be silently dropped from a mixed selection' {
        $Caught = $null
        try { $null = Validate @{ VariantNames = @('Baseline', 'Unknown') } } catch { $Caught = $_.Exception.Message }
        Assert-That ($Caught -eq 'VariantNames contains unsupported test variants.') 'Mixed unknown variants passed.'
    }
    Check 'incompatible internal resolution override fails before launching TSR or spline cases' {
        foreach ($VariantName in @('TSRHistory200', 'TSRHistory150', 'SplineRayTracingOn', 'SplineRayTracingOff')) {
            $Caught = $null
            try { $null = Validate @{ VariantNames = @($VariantName); ScreenPercentage = 50 } } catch { $Caught = $_.Exception.Message }
            Assert-That ($Caught -like 'Selected variants require the default internal resolution*') 'Conflicting resolution reached launch.'
        }
    }
    Check 'all catalog variants round-trip through JSON and the dry-run plan' {
        $Path = Write-Config (@{ SchemaVersion = 1; VariantNames = $RenderCostVariantNames } | ConvertTo-Json)
        $Report = Validate @{ ConfigFile = $Path }
        Assert-That ($Report.Configuration.Cases.Count -eq $RenderCostVariantNames.Count) 'A catalog variant was not resolved.'
        foreach ($Case in $Report.Configuration.Cases) {
            $Variant = $RenderCostCatalog.Variants | Where-Object { $_.Name -eq $Case.Definition.Name }
            Assert-That ($Case.ExecCmds -eq (Get-RenderCostExecCommands $Variant)) 'Dry-run launch commands differ.'
        }
    }
    foreach ($Variant in $RenderCostCatalog.Variants) {
        Check "$($Variant.Name): launch/readback use one definition; missing or conflicting values fail" {
            $Commands = (Get-RenderCostExecCommands $Variant) -split ','
            $LogText = @(
                foreach ($Name in $Variant.CVars.Keys) { '{0} = "{1}"' -f $Name, $Variant.CVars[$Name] }
                'r.ScreenPercentage = "0"'
                'r.DynamicRes.OperationMode = "0"'
            ) -join "`n"
            $Result = Test-RenderCostVariant $Variant $LogText 160 5 12
            Assert-That ($Result.ConfigurationValid -and $Result.ConsumersValid) 'Matching configuration rejected.'
            foreach ($Name in $Variant.CVars.Keys) {
                Assert-That ($Commands -contains "$Name $($Variant.CVars[$Name])" -and $Commands -contains $Name) "Missing set/read command for $Name"
                $Missing = ($LogText -split "`n" | Where-Object { -not $_.StartsWith("$Name =") }) -join "`n"
                $Result = Test-RenderCostVariant $Variant $Missing 160 5 12
                Assert-That (-not $Result.ConfigurationValid) "Missing readback accepted: $Name"
                $Result = Test-RenderCostVariant $Variant ($LogText + "`n$Name = 999") 160 5 12
                Assert-That (-not $Result.ConfigurationValid) "Earlier value hid a later mismatch: $Name"
            }
            if ($Variant.RequireDefaultResolution) {
                $Result = Test-RenderCostVariant $Variant ($LogText + "`nr.ScreenPercentage = 50") 160 5 12
                Assert-That (-not $Result.ConfigurationValid) 'Resolution isolation guard was lost.'
            }
        }
    }
    Check 'consumer boundaries retain strict 5/12 checks and skip them only for zero enemies' {
        foreach ($Variant in $RenderCostCatalog.Variants) {
            $Result = Test-RenderCostVariant $Variant '' 160 4 11
            Assert-That ($Result.ConsumersValid -eq ($null -eq $Variant.ExpectedConsumers)) 'Consumer budget guard changed.'
            $Result = Test-RenderCostVariant $Variant '' 0 0 0
            Assert-That $Result.ConsumersValid 'Zero-enemy run requires enemy consumers.'
            $Result = Test-RenderCostVariant $Variant '' 160 5 12
            Assert-That ($Result.ConsumerOverrideValid -eq ([string]::IsNullOrEmpty($Variant.DisabledConsumer))) 'Disabled consumer was not checked.'
            $Result = Test-RenderCostVariant $Variant '' 160 0 0
            Assert-That $Result.ConsumerOverrideValid 'Disabled consumers should pass at zero.'
        }
    }
    Check 'boolean readbacks and invariant-culture command values are preserved' {
        Assert-That (Test-RenderCostCVar 'r.AllowOcclusionQueries = true' 'r.AllowOcclusionQueries' 1) 'True alias rejected.'
        Assert-That (Test-RenderCostCVar 'r.AllowOcclusionQueries = false' 'r.AllowOcclusionQueries' 0) 'False alias rejected.'
        $OldCulture = [Threading.Thread]::CurrentThread.CurrentCulture
        try {
            [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('de-DE')
            $Commands = Get-RenderCostExecCommands $RenderCostCatalog.Variants[0] 66.7
            Assert-That (($Commands -split ',') -contains 'r.ScreenPercentage 66.7') 'Locale changed command syntax.'
        } finally { [Threading.Thread]::CurrentThread.CurrentCulture = $OldCulture }
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
    Check 'environment serialization records plain INI text, never provider metadata' {
        $SettingsDirectory = Join-Path $MapProject 'Saved\Config\WindowsEditor'
        $null = New-Item -ItemType Directory -Path $SettingsDirectory -Force
        Set-Content -LiteralPath (Join-Path $SettingsDirectory 'GameUserSettings.ini') -Value '[Test] Value=1' -Encoding UTF8
        function Start-Process { throw 'TEST FORBIDS process launch.' }
        function Get-Process { throw 'TEST FORBIDS process queries.' }
        function Get-Command { return $null }
        function git { 'test-revision' }
        function powercfg { 'test-power-scheme' }
        function ConvertTo-Json {
            param([Parameter(ValueFromPipeline)]$InputObject, [int]$Depth = 2, [switch]$Compress)
            process {
                if ($InputObject -is [Collections.IDictionary] -and $InputObject.Contains('GameUserSettings')) {
                    $Content = $InputObject.GameUserSettings.Content
                    # Reject the metadata before serializing it: the old WinPS 5.1 path could run for minutes.
                    Assert-That ($Content -is [string] -and $Content.Contains('[Test] Value=1')) 'INI text was lost.'
                    Assert-That ($Content.PSObject.Properties.Name -notcontains 'PSDrive') 'Get-Content provider metadata leaked into the record.'
                    $Json = Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $InputObject -Depth $Depth
                    Assert-That ($Json.Length -lt 50000 -and $Json -notmatch 'PSProvider') 'Environment JSON expanded filesystem metadata.'
                    throw 'TEST environment serialized without launching UE.'
                }
                Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $InputObject -Depth $Depth -Compress:$Compress
            }
        }
        $Caught = $null
        try { $null = & $Runner -ProjectRoot $MapProject -EditorPath $Runner -Map '/Game/Regression/Selected' -VariantNames Baseline -RunName EnvironmentPlainText }
        catch { $Caught = $_.Exception.Message }
        Assert-That ($Caught -eq 'TEST environment serialized without launching UE.') "Environment test did not reach its boundary: $Caught"
    }
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
