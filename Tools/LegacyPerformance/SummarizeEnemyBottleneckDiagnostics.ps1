param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot
)

$ErrorActionPreference = "Stop"
$InvariantCulture = [System.Globalization.CultureInfo]::InvariantCulture
$NumberStyles = [System.Globalization.NumberStyles]::Float

function Get-NumericValues {
    param([object[]]$Rows, [string]$Column)
    $Values = New-Object System.Collections.Generic.List[double]
    foreach ($Row in $Rows) {
        $Value = 0.0
        if ([double]::TryParse($Row.$Column, $NumberStyles, $InvariantCulture, [ref]$Value)) {
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
    if ($Lines.Count -lt 2) { return @() }
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

function Get-SnapshotValue {
    param([string]$LogPath, [string]$Name)
    $SnapshotLine = Get-Content -LiteralPath $LogPath |
        Select-String -Pattern "Benchmark enemy snapshot:" |
        Select-Object -Last 1
    if (-not $SnapshotLine) { return 0 }
    $Match = [regex]::Match($SnapshotLine.Line, "$Name=(\d+)")
    if (-not $Match.Success) { return 0 }
    return [int]$Match.Groups[1].Value
}

$Manifest = Import-Csv -LiteralPath (Join-Path $EvidenceRoot "manifest.csv")
$RunSummary = @()

foreach ($Entry in $Manifest) {
    $Rows = @(Import-CsvWithUniqueHeaders $Entry.Csv | Where-Object {
        $Value = 0.0
        [double]::TryParse($_.FrameTime, $NumberStyles, $InvariantCulture, [ref]$Value) -and $Value -gt 0.0
    })

    $Frame = Get-NumericValues $Rows "FrameTime"
    $FrameAverage = Get-Average $Frame
    $RunSummary += [PSCustomObject]@{
        Group = $Entry.Group
        Run = [int]$Entry.Run
        Samples = $Frame.Count
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
        GameThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GameThreadTime")), 3)
        RenderThreadMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "RenderThreadTime")), 3)
        GpuMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "GPUTime")), 3)
        EndPhysicsWaitMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/EventWait/EndPhysics")), 3)
        WorkerPhysicsMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/AllWorkers/Physics")), 3)
        TickActorsMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/TickActors")), 3)
        SyncBodiesMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/SyncBodies")), 3)
        CharacterMovementMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/CharacterMovement")), 3)
        AnimationMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "Exclusive/GameThread/Animation")), 3)
        AttackSweepMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueCombat/GameThread/AttackSweepTime")), 3)
        AttackWindowsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueCombat/AttackWindowUpdateCount")), 3)
        AttackSweepsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueCombat/AttackSweepCount")), 3)
        SweepHitsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueCombat/SweepReturnedHitCount")), 3)
        AIDecisionMs = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/GameThread/DecisionTime")), 3)
        AIDecisionsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/DecisionCount")), 3)
        MoveRequestsPerFrame = [Math]::Round((Get-Average (Get-NumericValues $Rows "fpstrueAI/MoveRequestCount")), 3)
        CharacterMovementTicks = [Math]::Round((Get-Average (Get-NumericValues $Rows "Ticks/CharacterMovementComponent")), 2)
        SkeletalMeshTicks = [Math]::Round((Get-Average (Get-NumericValues $Rows "Ticks/SkeletalMeshComponent")), 2)
        PathFollowingTicks = [Math]::Round((Get-Average (Get-NumericValues $Rows "Ticks/PathFollowingComponent")), 2)
        MovementFull = Get-SnapshotValue $Entry.Log "movementFull"
        MovementMid = Get-SnapshotValue $Entry.Log "movementMid"
        MovementFar = Get-SnapshotValue $Entry.Log "movementFar"
        MovementTickEnabled = Get-SnapshotValue $Entry.Log "movementTickEnabled"
		SkeletalMeshTickEnabled = Get-SnapshotValue $Entry.Log "skeletalMeshTickEnabled"
        Attacking = Get-SnapshotValue $Entry.Log "attacking"
        CastingShadow = Get-SnapshotValue $Entry.Log "castingShadow"
    }
}

$GroupSummary = @()
foreach ($Group in @($RunSummary.Group | Sort-Object -Unique)) {
    $Rows = @($RunSummary | Where-Object { $_.Group -eq $Group })
    $GroupSummary += [PSCustomObject]@{
        Group = $Group
        Runs = $Rows.Count
        AverageFps = [Math]::Round((Get-Average @($Rows.AverageFps)), 2)
        FrameAverageMs = [Math]::Round((Get-Average @($Rows.FrameAverageMs)), 3)
        FrameP95Ms = [Math]::Round((Get-Average @($Rows.FrameP95Ms)), 3)
        GameThreadMs = [Math]::Round((Get-Average @($Rows.GameThreadMs)), 3)
		RenderThreadMs = [Math]::Round((Get-Average @($Rows.RenderThreadMs)), 3)
		GpuMs = [Math]::Round((Get-Average @($Rows.GpuMs)), 3)
        EndPhysicsWaitMs = [Math]::Round((Get-Average @($Rows.EndPhysicsWaitMs)), 3)
		WorkerPhysicsMs = [Math]::Round((Get-Average @($Rows.WorkerPhysicsMs)), 3)
        TickActorsMs = [Math]::Round((Get-Average @($Rows.TickActorsMs)), 3)
        SyncBodiesMs = [Math]::Round((Get-Average @($Rows.SyncBodiesMs)), 3)
        CharacterMovementMs = [Math]::Round((Get-Average @($Rows.CharacterMovementMs)), 3)
        AnimationMs = [Math]::Round((Get-Average @($Rows.AnimationMs)), 3)
        AttackSweepMs = [Math]::Round((Get-Average @($Rows.AttackSweepMs)), 3)
        AttackSweepsPerFrame = [Math]::Round((Get-Average @($Rows.AttackSweepsPerFrame)), 3)
        SweepHitsPerFrame = [Math]::Round((Get-Average @($Rows.SweepHitsPerFrame)), 3)
        PathFollowingTicks = [Math]::Round((Get-Average @($Rows.PathFollowingTicks)), 2)
        CharacterMovementTicks = [Math]::Round((Get-Average @($Rows.CharacterMovementTicks)), 2)
		SkeletalMeshTicks = [Math]::Round((Get-Average @($Rows.SkeletalMeshTicks)), 2)
    }
}

$Baseline = $GroupSummary | Where-Object { $_.Group -eq "Baseline" }
$Comparison = @()
foreach ($Group in @($GroupSummary | Where-Object { $_.Group -ne "Baseline" })) {
    $Comparison += [PSCustomObject]@{
        Group = $Group.Group
        GameThreadSavedMs = [Math]::Round($Baseline.GameThreadMs - $Group.GameThreadMs, 3)
		RenderThreadSavedMs = [Math]::Round($Baseline.RenderThreadMs - $Group.RenderThreadMs, 3)
		GpuSavedMs = [Math]::Round($Baseline.GpuMs - $Group.GpuMs, 3)
        FrameTimeSavedMs = [Math]::Round($Baseline.FrameAverageMs - $Group.FrameAverageMs, 3)
        EndPhysicsWaitSavedMs = [Math]::Round($Baseline.EndPhysicsWaitMs - $Group.EndPhysicsWaitMs, 3)
		WorkerPhysicsSavedMs = [Math]::Round($Baseline.WorkerPhysicsMs - $Group.WorkerPhysicsMs, 3)
        TickActorsSavedMs = [Math]::Round($Baseline.TickActorsMs - $Group.TickActorsMs, 3)
        CharacterMovementSavedMs = [Math]::Round($Baseline.CharacterMovementMs - $Group.CharacterMovementMs, 3)
		AnimationSavedMs = [Math]::Round($Baseline.AnimationMs - $Group.AnimationMs, 3)
        AttackSweepSavedMs = [Math]::Round($Baseline.AttackSweepMs - $Group.AttackSweepMs, 3)
    }
}

$RunSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "run_summary.csv") -NoTypeInformation -Encoding UTF8
$GroupSummary | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "group_summary.csv") -NoTypeInformation -Encoding UTF8
$Comparison | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "comparison_vs_baseline.csv") -NoTypeInformation -Encoding UTF8
$GroupSummary | Format-Table -AutoSize
$Comparison | Format-Table -AutoSize
