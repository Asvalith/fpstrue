param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot
)

$ErrorActionPreference = "Stop"

function Get-NumericValues {
    param([object[]]$Rows, [string]$Column)

    $Values = New-Object System.Collections.Generic.List[double]
    foreach ($Row in $Rows) {
        $Value = 0.0
        if ([double]::TryParse(
            $Row.$Column,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$Value
        )) {
            $Values.Add($Value)
        }
    }
    return $Values.ToArray()
}

function Get-Average {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    return ($Values | Measure-Object -Average).Average
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { return 0.0 }
    $Sorted = $Values | Sort-Object
    $Index = [Math]::Ceiling($Percentile * $Sorted.Count) - 1
    return $Sorted[[Math]::Max(0, [Math]::Min($Index, $Sorted.Count - 1))]
}

function Import-CsvWithUniqueHeaders {
    param([string]$Path)

    $Lines = @(Get-Content -LiteralPath $Path)
    if ($Lines.Count -lt 2) {
        return @()
    }

    $HeaderCounts = @{}
    $Headers = @($Lines[0] -split ',' | ForEach-Object {
        $Name = $_.Trim('"')
        if ($HeaderCounts.ContainsKey($Name)) {
            ++$HeaderCounts[$Name]
            return "${Name}__$($HeaderCounts[$Name])"
        }
        $HeaderCounts[$Name] = 1
        return $Name
    })

    return @($Lines | Select-Object -Skip 1 | ConvertFrom-Csv -Header $Headers)
}

$Manifest = Import-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv")
$RunSummary = @()

foreach ($Entry in $Manifest) {
    $Rows = @(Import-CsvWithUniqueHeaders $Entry.Csv | Where-Object {
        $Value = 0.0
        [double]::TryParse(
            $_.FrameTime,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$Value
        ) -and $Value -gt 0.0
    })

    $Frame = Get-NumericValues $Rows "FrameTime"
    $Game = Get-NumericValues $Rows "GameThreadTime"
    $Render = Get-NumericValues $Rows "RenderThreadTime"
    $Gpu = Get-NumericValues $Rows "GPUTime"
    $Movement = Get-NumericValues $Rows "Exclusive/GameThread/CharacterMovement"
    $Animation = Get-NumericValues $Rows "Exclusive/GameThread/Animation"
    $EnemyActors = Get-NumericValues $Rows "ActorCount/fpstrueEnemyCharacter"
    $FrameAverage = Get-Average $Frame

    $RunSummary += [PSCustomObject]@{
        Group = $Entry.Group
        Run = [int]$Entry.Run
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
        GameThreadMs = [Math]::Round((Get-Average $Game), 3)
        RenderThreadMs = [Math]::Round((Get-Average $Render), 3)
        GpuMs = [Math]::Round((Get-Average $Gpu), 3)
        CharacterMovementMs = [Math]::Round((Get-Average $Movement), 3)
        AnimationMs = [Math]::Round((Get-Average $Animation), 3)
        AverageEnemyActors = [Math]::Round((Get-Average $EnemyActors), 1)
    }
}

$GroupSummary = @()
foreach ($Group in @("A_FullRate", "B_Tiered")) {
    $GroupRows = @($RunSummary | Where-Object { $_.Group -eq $Group })
    $GroupSummary += [PSCustomObject]@{
        Group = $Group
        Runs = $GroupRows.Count
        AverageFps = [Math]::Round((Get-Average @($GroupRows.AverageFps)), 2)
        FrameAverageMs = [Math]::Round((Get-Average @($GroupRows.FrameAverageMs)), 3)
        FrameP95Ms = [Math]::Round((Get-Average @($GroupRows.FrameP95Ms)), 3)
        FrameP99Ms = [Math]::Round((Get-Average @($GroupRows.FrameP99Ms)), 3)
        GameThreadMs = [Math]::Round((Get-Average @($GroupRows.GameThreadMs)), 3)
        RenderThreadMs = [Math]::Round((Get-Average @($GroupRows.RenderThreadMs)), 3)
        GpuMs = [Math]::Round((Get-Average @($GroupRows.GpuMs)), 3)
        CharacterMovementMs = [Math]::Round((Get-Average @($GroupRows.CharacterMovementMs)), 3)
        AnimationMs = [Math]::Round((Get-Average @($GroupRows.AnimationMs)), 3)
        AverageEnemyActors = [Math]::Round((Get-Average @($GroupRows.AverageEnemyActors)), 1)
    }
}

$RunSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "run_summary.csv") -NoTypeInformation -Encoding UTF8
$GroupSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "group_summary.csv") -NoTypeInformation -Encoding UTF8

$A = $GroupSummary | Where-Object { $_.Group -eq "A_FullRate" }
$B = $GroupSummary | Where-Object { $_.Group -eq "B_Tiered" }
$Comparison = [PSCustomObject]@{
    FpsChange = [Math]::Round($B.AverageFps - $A.AverageFps, 2)
    FrameChangeMs = [Math]::Round($B.FrameAverageMs - $A.FrameAverageMs, 3)
    P95ChangeMs = [Math]::Round($B.FrameP95Ms - $A.FrameP95Ms, 3)
    P99ChangeMs = [Math]::Round($B.FrameP99Ms - $A.FrameP99Ms, 3)
    GameThreadChangeMs = [Math]::Round($B.GameThreadMs - $A.GameThreadMs, 3)
    CharacterMovementChangeMs = [Math]::Round($B.CharacterMovementMs - $A.CharacterMovementMs, 3)
    AnimationChangeMs = [Math]::Round($B.AnimationMs - $A.AnimationMs, 3)
}
$Comparison | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "comparison.csv") -NoTypeInformation -Encoding UTF8

$GroupSummary | Format-Table -AutoSize
$Comparison | Format-List
