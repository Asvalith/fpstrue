param(
    [string]$EvidenceRoot = "E:\ueprojrct\fpstrue_safe2\Saved\Profiling\VSM_CoarsePages_AB_20260816"
)

$ErrorActionPreference = "Stop"

function Get-Values {
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

$Variants = @("CoarsePagesOn", "CoarsePagesOff")
$Summary = @()

foreach ($Variant in $Variants) {
    $Rows = @(Import-Csv -LiteralPath (Join-Path $EvidenceRoot "$Variant.csv") | Where-Object {
        $Value = 0.0
        [double]::TryParse(
            $_.FrameTime,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$Value
        ) -and $Value -gt 0.0
    })

    $Frame = Get-Values $Rows "FrameTime"
    $Game = Get-Values $Rows "GameThreadTime"
    $Render = Get-Values $Rows "RenderThreadTime"
    $Gpu = Get-Values $Rows "GPUTime"
    $Shadow = Get-Values $Rows "GPU/ShadowDepths"
    $DrawCalls = Get-Values $Rows "RHI/DrawCalls"
    $FrameAverage = Get-Average $Frame

    $Summary += [PSCustomObject]@{
        Variant = $Variant
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
        GameThreadMs = [Math]::Round((Get-Average $Game), 3)
        RenderThreadMs = [Math]::Round((Get-Average $Render), 3)
        GpuMs = [Math]::Round((Get-Average $Gpu), 3)
        ShadowDepthsMs = [Math]::Round((Get-Average $Shadow), 3)
        DrawCalls = [Math]::Round((Get-Average $DrawCalls), 0)
    }
}

$Summary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "summary.csv") -NoTypeInformation -Encoding UTF8
$Summary | Format-Table -AutoSize
