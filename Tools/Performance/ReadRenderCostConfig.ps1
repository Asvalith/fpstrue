# Configuration mechanics only. Edit ExperimentProfiles/RenderCostCases.psd1 or a JSON preset,
# not this reader or the process runner, when changing an experiment.
$RenderCostCatalogPath = Join-Path $PSScriptRoot 'ExperimentProfiles/RenderCostCases.psd1'
$RenderCostCatalog = Import-PowerShellDataFile -LiteralPath $RenderCostCatalogPath
$RenderCostConfigParameters = @($RenderCostCatalog.Defaults.Keys | Sort-Object)
$RenderCostVariantNames = @($RenderCostCatalog.Variants | ForEach-Object { $_.Name })

function Get-CVarValue {
    param([string]$Text, [string]$Name)
    $Pattern = '(?im)' + [regex]::Escape($Name) + '\s*=\s*"?([^"\s]+)"?'
    $MatchesFound = [regex]::Matches($Text, $Pattern)
    if ($MatchesFound.Count -eq 0) { return $null }
    # Validate and record the same FINAL echoed value. An earlier match is not sufficient.
    return $MatchesFound[$MatchesFound.Count - 1].Groups[1].Value
}

function Test-RenderCostCVar {
    param([string]$LogText, [string]$Name, $Expected)
    $Actual = Get-CVarValue $LogText $Name
    if ($null -eq $Actual) { return $false }
    # UE may echo this boolean as either a number or a word. Other CVars retain numeric readback.
    if ($Name -eq 'r.AllowOcclusionQueries') {
        return $Actual -in @([string]$Expected, $(if ($Expected -eq 1) { 'true' } else { 'false' }))
    }
    return $Actual -eq [string]$Expected
}

function Get-RenderCostExecCommands {
    param([hashtable]$Variant, [Nullable[double]]$ScreenPercentage)
    $Commands = @(
        foreach ($CVars in @($RenderCostCatalog.Capture.CommonCVars, $Variant.CVars)) {
            foreach ($Name in @($CVars.Keys | Sort-Object)) {
                '{0} {1}' -f $Name, ([double]$CVars[$Name]).ToString('R', [Globalization.CultureInfo]::InvariantCulture)
            }
        }
        if ($null -ne $ScreenPercentage) {
            'r.ScreenPercentage ' + ([double]$ScreenPercentage).ToString('R', [Globalization.CultureInfo]::InvariantCulture)
        }
        # Always read back overrides, including newly added variants, after applying them.
        @($RenderCostCatalog.Capture.ReadbackCVars + @($Variant.CVars.Keys) | Sort-Object -Unique)
    )
    return $Commands -join ','
}

function Test-RenderCostVariant {
    param([hashtable]$Variant, [string]$LogText, [int]$Count, [int]$Shadows, [int]$RayTracing)
    $ConfigurationValid = $true
    foreach ($Name in $Variant.CVars.Keys) {
        if (-not (Test-RenderCostCVar $LogText $Name $Variant.CVars[$Name])) { $ConfigurationValid = $false }
    }
    if ($Variant.RequireDefaultResolution) {
        $ConfigurationValid = $ConfigurationValid -and
            (Test-RenderCostCVar $LogText 'r.ScreenPercentage' 0) -and
            (Test-RenderCostCVar $LogText 'r.DynamicRes.OperationMode' 0)
    }
    $ConsumersValid = $true
    if ($Count -gt 0 -and $null -ne $Variant.ExpectedConsumers) {
        $ConsumersValid = $Shadows -eq $Variant.ExpectedConsumers.Shadows -and
            $RayTracing -eq $Variant.ExpectedConsumers.RayTracing
    }
    return [PSCustomObject]@{
        ConfigurationValid = $ConfigurationValid
        ConsumersValid = $ConsumersValid
        ConsumerOverrideValid = ($Variant.DisabledConsumer -ne 'RayTracing' -or $RayTracing -eq 0) -and
            ($Variant.DisabledConsumer -ne 'Shadows' -or $Shadows -eq 0)
        OcclusionOverrideValid = -not $Variant.CVars.ContainsKey('r.AllowOcclusionQueries') -or
            (Test-RenderCostCVar $LogText 'r.AllowOcclusionQueries' $Variant.CVars['r.AllowOcclusionQueries'])
    }
}

function Read-RenderCostConfig {
    param([string]$Path)
    $Item = Get-Item -LiteralPath $Path -ErrorAction Stop
    $Json = Get-Content -LiteralPath $Item.FullName -Raw -Encoding UTF8
    $Settings = $Json | ConvertFrom-Json -ErrorAction Stop
    if (-not $Json.TrimStart().StartsWith('{') -or $Settings -isnot [PSCustomObject]) {
        throw 'ConfigFile must contain a JSON object.'
    }
    if (($Settings.SchemaVersion -isnot [int] -and $Settings.SchemaVersion -isnot [long]) -or $Settings.SchemaVersion -ne 1) {
        throw 'ConfigFile requires SchemaVersion: 1.'
    }
    foreach ($Property in $Settings.PSObject.Properties) {
        $Name = $Property.Name
        $Value = $Property.Value
        if ($Name -ceq 'SchemaVersion') { continue }
        if ($RenderCostConfigParameters -cnotcontains $Name) { throw "Unknown ConfigFile setting: $Name" }
        switch ($Name) {
            { $_ -in @('CaptureTaskTrace', 'BalancedVariantOrder') } {
                if ($Value -isnot [bool]) { throw "$Name must be a JSON boolean." }
            }
            'VariantNames' {
                if ($Value -isnot [array] -or $Value.Count -eq 0 -or
                    @($Value | Where-Object { $_ -isnot [string] -or $RenderCostVariantNames -notcontains $_ }).Count -gt 0) {
                    throw 'VariantNames must be a nonempty array of supported variant names.'
                }
            }
            'ExpectedAvoidanceMode' {
                if ($Value -isnot [string] -or @('Any', 'RVO', 'DetourCrowd') -notcontains $Value) {
                    throw 'ExpectedAvoidanceMode must be Any, RVO or DetourCrowd.'
                }
            }
            'Map' {
                if ($Value -isnot [string] -or $Value -cnotmatch '\A/Game/(?:[A-Za-z0-9_]+/)*[A-Za-z0-9_]+\z') {
                    throw 'Map must be an Unreal /Game/... package path without arguments or URL options.'
                }
            }
            default {
                if ($Name -eq 'ScreenPercentage' -and $null -eq $Value) { continue }
                if ($Name -eq 'Counts' -and ($Value -isnot [array] -or $Value.Count -eq 0)) {
                    throw 'Counts must be a nonempty array.'
                }
                if ($Name -ne 'Counts' -and $Value -is [array]) { throw "$Name must be a number." }
                foreach ($Number in @($Value)) {
                    if (($Number -isnot [int] -and $Number -isnot [long] -and $Number -isnot [double] -and $Number -isnot [decimal]) -or
                        [double]::IsNaN([double]$Number) -or [double]::IsInfinity([double]$Number)) { throw "$Name must contain finite numbers." }
                    if ($Name -in @('Counts', 'RunsPerCase', 'BenchmarkSeed') -and
                        ([Math]::Truncate([double]$Number) -ne $Number -or $Number -lt [int]::MinValue -or $Number -gt [int]::MaxValue)) {
                        throw "$Name must contain 32-bit integers."
                    }
                    if (($Name -in @('Counts', 'WarmupSeconds', 'StartTrimSeconds', 'EndTrimSeconds') -and $Number -lt 0) -or
                        ($Name -in @('RunsPerCase', 'DurationSeconds', 'PlayerHealth') -and $Number -lt 1) -or
                        ($Name -eq 'ScreenPercentage' -and ($Number -le 0 -or $Number -gt 100))) { throw "$Name is outside the valid range." }
                }
            }
        }
    }
    return [PSCustomObject]@{
        Path = $Item.FullName
        SHA256 = (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash
        Settings = $Settings
    }
}
