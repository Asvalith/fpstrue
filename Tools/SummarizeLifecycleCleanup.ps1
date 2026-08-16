param(
    [string]$EvidenceRoot = "E:\ueprojrct\fpstrue_safe2\Saved\Profiling\LifecycleCleanup_80_20260816"
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

function Get-CsvSummary {
    param([string]$State, [string]$Path)

    $Rows = @(Import-Csv -LiteralPath $Path | Where-Object {
        $FrameValue = 0.0
        [double]::TryParse(
            $_.FrameTime,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$FrameValue
        ) -and $FrameValue -gt 0.0
    })
    $Frame = Get-NumericValues $Rows "FrameTime"
    $FrameAverage = Get-Average $Frame

    return [PSCustomObject]@{
        State = $State
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        GameThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GameThreadTime")), 3)
        RenderThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "RenderThreadTime")), 3)
        GpuMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPUTime")), 3)
        CharacterMovementMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/CharacterMovement")), 3)
        AnimationMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/Animation")), 3)
        TickActorsMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/TickActors")), 3)
        PhysicalMemoryMB = [Math]::Round((Get-Average (Get-NumericValues $Rows "PhysicalUsedMB")), 0)
        GpuMemoryMB = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPUMem/LocalUsedMB")), 0)
        WantedMipsMB = [Math]::Round((Get-Average (Get-NumericValues $Rows "TextureStreaming/WantedMips")), 1)
        EnemyActors = [Math]::Round((Get-Average (Get-NumericValues $Rows "ActorCount/fpstrueEnemyCharacter")), 1)
    }
}

function Get-MemReportSummary {
    param([string]$Path)

    $Text = Get-Content -LiteralPath $Path -Raw
    $Physical = [regex]::Match($Text, "Process Physical Memory:\s+([0-9.]+) MB used")
    $Texture = [regex]::Match($Text, "([0-9.]+)MB\s+-\s+Texture Memory Used")
    $Objects = [regex]::Match($Text, "(?m)^([0-9]+) Objects \(Total:")

    return [PSCustomObject]@{
        ProcessPhysicalMB = [double]$Physical.Groups[1].Value
        TextureMemoryMB = [double]$Texture.Groups[1].Value
        UObjectCount = [int]$Objects.Groups[1].Value
    }
}

$Alive = Get-CsvSummary "Alive" (Join-Path $EvidenceRoot "Alive_80.csv")
$After = Get-CsvSummary "PostCleanup" (Join-Path $EvidenceRoot "PostCleanup_80.csv")
$BeforeMemory = Get-MemReportSummary (Join-Path $EvidenceRoot "BeforeCleanup_80.memreport")
$AfterMemory = Get-MemReportSummary (Join-Path $EvidenceRoot "AfterCleanup_80.memreport")
$LogPath = Join-Path $EvidenceRoot "LifecycleCleanup_80.log"
$LogLines = Get-Content -LiteralPath $LogPath

$Alive | Add-Member -NotePropertyName UObjectCount -NotePropertyValue $BeforeMemory.UObjectCount
$Alive | Add-Member -NotePropertyName MemReportPhysicalMB -NotePropertyValue $BeforeMemory.ProcessPhysicalMB
$Alive | Add-Member -NotePropertyName MemReportTextureMB -NotePropertyValue $BeforeMemory.TextureMemoryMB
$After | Add-Member -NotePropertyName UObjectCount -NotePropertyValue $AfterMemory.UObjectCount
$After | Add-Member -NotePropertyName MemReportPhysicalMB -NotePropertyValue $AfterMemory.ProcessPhysicalMB
$After | Add-Member -NotePropertyName MemReportTextureMB -NotePropertyValue $AfterMemory.TextureMemoryMB

$Summary = @($Alive, $After)
$Summary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "summary.csv") -NoTypeInformation -Encoding UTF8

$ObjectDelta = $AfterMemory.UObjectCount - $BeforeMemory.UObjectCount
$Lines = @(
    "# Lifecycle Cleanup 80",
    "",
    "| State | Enemy Actors | FPS | Frame | GT | RT | GPU | Movement | Animation | UObjects | Process Physical |",
    "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    "| Alive | $($Alive.EnemyActors) | $($Alive.AverageFps) | $($Alive.FrameAverageMs) ms | $($Alive.GameThreadMs) ms | $($Alive.RenderThreadMs) ms | $($Alive.GpuMs) ms | $($Alive.CharacterMovementMs) ms | $($Alive.AnimationMs) ms | $($Alive.UObjectCount) | $($Alive.MemReportPhysicalMB) MB |",
    "| Post cleanup | $($After.EnemyActors) | $($After.AverageFps) | $($After.FrameAverageMs) ms | $($After.GameThreadMs) ms | $($After.RenderThreadMs) ms | $($After.GpuMs) ms | $($After.CharacterMovementMs) ms | $($After.AnimationMs) ms | $($After.UObjectCount) | $($After.MemReportPhysicalMB) MB |",
    "",
    "- UObject delta: $ObjectDelta",
    "- VSM queue overflows: $(($LogLines | Select-String -SimpleMatch 'Non-Nanite Marking Job Queue overflow').Count)",
    "- Texture pool warnings: $(($LogLines | Select-String -Pattern 'Texture streaming pool.*over budget').Count)",
    "- CharacterMovement max-iteration warnings: $(($LogLines | Select-String -SimpleMatch 'Max iterations 8 hit').Count)",
    "- Process physical memory is allocator-sensitive and is not used as proof of immediate memory release."
)
$Lines | Set-Content -LiteralPath (Join-Path $EvidenceRoot "summary.md") -Encoding UTF8

$Summary | Format-Table -AutoSize
