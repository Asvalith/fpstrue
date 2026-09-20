# The preset contains only existing experiment parameters, never machine paths or commands.
$RenderCostConfigParameters = @('Counts', 'RunsPerCase', 'WarmupSeconds', 'DurationSeconds',
    'ScreenPercentage', 'PlayerHealth', 'BenchmarkSeed', 'VariantNames', 'ExpectedAvoidanceMode',
    'CaptureTaskTrace', 'BalancedVariantOrder', 'StartTrimSeconds', 'EndTrimSeconds', 'Map')

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
                $Supported = @('Baseline', 'EnemyRayTracingOff', 'EnemyShadowsOff', 'OcclusionQueriesOn',
                    'OcclusionQueriesOff', 'HardwareQueries', 'HZBOcclusion', 'BufferedQueries2')
                if ($Value -isnot [array] -or $Value.Count -eq 0 -or
                    @($Value | Where-Object { $_ -isnot [string] -or $Supported -notcontains $_ }).Count -gt 0) {
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
