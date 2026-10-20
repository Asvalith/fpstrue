<#
Run the existing normal-input rendered pair with opt-in final-pose sole
inspection: player idle/walk/sprint/turn, using the original Facing capture.
This is functional measurement, not a crowd/performance benchmark. Screenshots
still require human review. No Actor relocation, direct damage or fake AI is used.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:/ueprojrct/fpstrue_safe2',
    [string]$EngineRoot = 'E:/program/ue554/UE_5.5',
    [ValidateRange(1,65534)][int]$Port = 17861,
    [ValidateRange(180,600)][int]$TimeoutSeconds = 300,
    [ValidateRange(1,240)][int]$FacingStartServerTime = 150,
    [ValidateSet('Facing')][string]$Scenario = 'Facing',
    [ValidateRange(0,1000)][int]$PacketLagMs = 0,
    [ValidateRange(0,1000)][int]$PacketLagVarianceMs = 0,
    [ValidateRange(0,100)][int]$PacketLossPercent = 0,
    [string]$OutputRoot = '',
    [switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$TaskProject = (Resolve-Path -LiteralPath $ProjectRoot).Path
$TaskRunner = Join-Path $TaskProject 'Tools/Networking/RunFpsVisualAcceptance.ps1'
$TaskModule = Join-Path $TaskProject 'Binaries/Win64/UnrealEditor-fpstrue.dll'
$TaskProfiles = @([pscustomobject]@{ Name=$Scenario; Port=$Port; FacingReview=$true; FullGameplay=$false; EnemyPopulation=0 })
if ($DryRun) {
    [pscustomobject]@{ Mode='ReadOnlyPlan'; Profiles=$TaskProfiles; SampleHz=10; CpuSkin='Configured LOD0 sole subset in final postprocess pose'
        Gates='Each rendered peer: LocalPlayer and RemotePlayer idle/walk/sprint/turn coverage; measured floor penetration, stance gap and fixed-vertex same-support plant drift; original forty consecutive Facing captures.'
        NoGameplayMutation=$true; ScreenshotsRequireHumanReview=$true; SlopeStepEvidence='Separate unit fixture; ordinary map capture proves only surfaces actually sampled.' } | ConvertTo-Json -Depth 5
    return
}
foreach ($TaskPath in @($TaskRunner,$TaskModule)) { if (-not (Test-Path -LiteralPath $TaskPath -PathType Leaf)) { throw "Missing prerequisite: $TaskPath" } }
if (-not $OutputRoot) { $OutputRoot=Join-Path $TaskProject ('Saved/Networking/FootIKReview_'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'_'+[guid]::NewGuid().ToString('N').Substring(0,8)) }
$TaskOutput=[IO.Path]::GetFullPath($OutputRoot)
$TaskAllowed=[IO.Path]::GetFullPath((Join-Path $TaskProject 'Saved/Networking')).TrimEnd('\')+'\'
if (-not $TaskOutput.StartsWith($TaskAllowed,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $TaskOutput)) { throw 'Use a fresh Saved/Networking child for foot IK evidence.' }
$null=New-Item -ItemType Directory -Path $TaskOutput
function Get-TaskFileIdentity([string]$Path) {
    $TaskFile=Get-Item -LiteralPath $Path
    [pscustomobject]@{ Path=$TaskFile.FullName; Bytes=$TaskFile.Length; SHA256=(Get-FileHash -LiteralPath $TaskFile.FullName -Algorithm SHA256).Hash }
}
function Get-TaskFrozenInputs {
    $TaskFiles=@(Get-ChildItem -LiteralPath (Join-Path $TaskProject 'Source') -Recurse -File | Sort-Object FullName)
    $TaskFiles+=Get-Item -LiteralPath (Join-Path $TaskProject 'Content/FirstPerson/Animations/FootIK/ABP_PlayerFootIK.uasset')
    $TaskFiles+=Get-Item -LiteralPath $TaskModule
    @($TaskFiles | ForEach-Object { Get-TaskFileIdentity $_.FullName })
}
function Read-TaskSamples([string]$Path) {
    $TaskPattern='PVP_FOOT_IK_REVIEW SAMPLE Peer=(?<Peer>\w+) Kind=(?<Kind>LocalPlayer|RemotePlayer) Pawn=(?<Pawn>\S+) Phase=(?<Phase>\w+) Side=(?<Side>[LR]) Time=(?<Time>[-\d.]+) Speed=(?<Speed>[-\d.]+) Grounded=(?<Grounded>[01]) Enabled=(?<Enabled>[01]) Planted=(?<Planted>[01]) Cpu=(?<Cpu>[01]) Vertices=(?<Vertices>\d+) Floor=(?<Floor>[01]) Gap=(?<Gap>[-\d.]+) NormalZ=(?<NormalZ>[-\d.]+) Slip=(?<Slip>[-\d.]+) PlantAge=(?<PlantAge>[-\d.]+) Pelvis=(?<Pelvis>[-\d.]+) TraceCount=(?<Traces>\d+) SkinLOD=0 RenderLOD=(?<LOD>\d+) Visible=(?<Visible>[01])(?<Tail>[^\r\n]*)'
    $TaskStream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
    $TaskReader=[IO.StreamReader]::new($TaskStream)
    try {$TaskLog=$TaskReader.ReadToEnd()} finally {$TaskReader.Dispose();$TaskStream.Dispose()}
    @([regex]::Matches($TaskLog,$TaskPattern) | ForEach-Object {
        $TaskMatch=$_
        $TaskMotion=[regex]::Match($TaskMatch.Groups['Tail'].Value,' PlantRevision=(?<Revision>\d+) PlantChanged=(?<Changed>[01]) Displacement=(?<Displacement>[-\d.]+) Delta=(?<Delta>[-\d.]+) DisplacementVertex=(?<Vertex>-?\d+)')
        if(-not $TaskMotion.Success){throw "Missing revision-aware sole motion diagnostics in $Path."}
        [pscustomobject]@{ Peer=$TaskMatch.Groups['Peer'].Value; Kind=$TaskMatch.Groups['Kind'].Value; Pawn=$TaskMatch.Groups['Pawn'].Value
            Phase=$TaskMatch.Groups['Phase'].Value; Side=$TaskMatch.Groups['Side'].Value
            Time=[double]::Parse($TaskMatch.Groups['Time'].Value,[Globalization.CultureInfo]::InvariantCulture)
            Speed=[double]::Parse($TaskMatch.Groups['Speed'].Value,[Globalization.CultureInfo]::InvariantCulture)
            Grounded=$TaskMatch.Groups['Grounded'].Value -eq '1'; Enabled=$TaskMatch.Groups['Enabled'].Value -eq '1'
            Planted=$TaskMatch.Groups['Planted'].Value -eq '1'; Cpu=$TaskMatch.Groups['Cpu'].Value -eq '1'
            Vertices=[int]$TaskMatch.Groups['Vertices'].Value; Floor=$TaskMatch.Groups['Floor'].Value -eq '1'
            Gap=[double]::Parse($TaskMatch.Groups['Gap'].Value,[Globalization.CultureInfo]::InvariantCulture)
            NormalZ=[double]::Parse($TaskMatch.Groups['NormalZ'].Value,[Globalization.CultureInfo]::InvariantCulture)
            Slip=[double]::Parse($TaskMatch.Groups['Slip'].Value,[Globalization.CultureInfo]::InvariantCulture)
            PlantAge=[double]::Parse($TaskMatch.Groups['PlantAge'].Value,[Globalization.CultureInfo]::InvariantCulture)
            Pelvis=[double]::Parse($TaskMatch.Groups['Pelvis'].Value,[Globalization.CultureInfo]::InvariantCulture)
            Traces=[int]$TaskMatch.Groups['Traces'].Value; LOD=[int]$TaskMatch.Groups['LOD'].Value; Visible=$TaskMatch.Groups['Visible'].Value -eq '1'
            PlantRevision=[uint32]$TaskMotion.Groups['Revision'].Value; PlantChanged=$TaskMotion.Groups['Changed'].Value -eq '1'
            Displacement=[double]::Parse($TaskMotion.Groups['Displacement'].Value,[Globalization.CultureInfo]::InvariantCulture)
            SampleDelta=[double]::Parse($TaskMotion.Groups['Delta'].Value,[Globalization.CultureInfo]::InvariantCulture)
            DisplacementVertex=[int]$TaskMotion.Groups['Vertex'].Value }
    })
}
function Get-TaskPercentile($Values,[double]$Percentile) {
    $TaskSorted=@($Values | Sort-Object)
    if($TaskSorted.Count -eq 0){return $null}
    $TaskSorted[[math]::Min($TaskSorted.Count-1,[math]::Floor(($TaskSorted.Count-1)*$Percentile))]
}
function Measure-TaskPeer([string]$Run,[string]$Peer,[ValidateSet('Facing')][string]$Profile) {
    $TaskLog=Join-Path $Run ($Peer+'.log')
    $TaskAll=@(Read-TaskSamples $TaskLog)
    # Keep adjacent same-material-vertex motion across release/replant boundaries.
    # World displacement also includes legitimate steps and moving supports: it is
    # diagnostic evidence, never proof that re-anchored feet are free of sliding.
    # Append before gates so failed coverage/stance checks still retain this data.
    $TaskMotionRows=@($TaskAll | Where-Object { $_.Cpu -and $_.Displacement -ge 0 -and $_.SampleDelta -gt 0 })
    $TaskReport.SampledMotionDiagnostics+=[pscustomobject]@{Profile=$Profile;Peer=$Peer;DiagnosticOnly=$true
        Meaning='Adjacent samples measure the same prior material vertex in world space, including across new plant revisions. Steps, support motion and stopping-pose jumps are not classified as continuous stance slip.'
        Groups=@(foreach($TaskMotionGroup in @($TaskMotionRows | Group-Object Kind,Phase)) {
            $TaskMotionValues=@($TaskMotionGroup.Group | ForEach-Object {$_.Displacement})
            [pscustomobject]@{Group=$TaskMotionGroup.Name;Intervals=$TaskMotionValues.Count
                DisplacementCmP95=(Get-TaskPercentile $TaskMotionValues 0.95)
                DisplacementCmMax=($TaskMotionValues | Measure-Object -Maximum).Maximum
                NewAnchorIntervals=@($TaskMotionGroup.Group | Where-Object PlantChanged).Count}
        })
        CrossAnchorIntervals=@($TaskMotionRows | Where-Object PlantChanged | Select-Object Kind,Pawn,Phase,Side,Time,PlantRevision,Displacement,SampleDelta,DisplacementVertex,Grounded,Enabled,Planted)
    }
    $TaskValid=@($TaskAll | Where-Object { $_.Cpu -and $_.Floor -and $_.Enabled -and $_.Grounded -and $_.Vertices -gt 0 -and $_.Traces -gt 0 })
    if($TaskValid.Count -lt 20){throw "$Profile/$Peer has insufficient final CPU-skinned, independently floor-tested samples."}
    $TaskCoverage=@()
    foreach($TaskKind in @('LocalPlayer','RemotePlayer')) {
        foreach($TaskPhase in @('Idle','Walk','Sprint','Turn')) {
            $TaskRows=@($TaskValid | Where-Object { $_.Kind -eq $TaskKind -and $_.Phase -eq $TaskPhase })
            $TaskTimes=@($TaskRows | Select-Object -ExpandProperty Time -Unique)
            if($TaskTimes.Count -lt 3){throw "$Profile/$Peer lacks actual $TaskKind $TaskPhase final-pose coverage."}
            $TaskCoverage+=[pscustomobject]@{Kind=$TaskKind;Phase=$TaskPhase;Frames=$TaskTimes.Count;FootSamples=$TaskRows.Count}
        }
    }
    $TaskMinimum=($TaskValid | Measure-Object Gap -Minimum).Minimum
    if($TaskMinimum -lt -3.0){throw "$Profile/$Peer final sole penetrated actual support by more than 3 cm (gap=$TaskMinimum)."}
    $TaskGroups=@(foreach($TaskGroup in @($TaskValid | Group-Object Kind,Phase)) {
        $TaskRows=@($TaskGroup.Group)
        $TaskStance=@($TaskRows | Where-Object { $_.Planted -and $_.PlantAge -ge 0.1 })
        $TaskContact=@($TaskRows | Where-Object { [math]::Abs($_.Gap) -le 3.0 })
        $TaskAbsolute=@($TaskStance | ForEach-Object {[math]::Abs($_.Gap)})
        $TaskP95=Get-TaskPercentile $TaskAbsolute 0.95
        $TaskSlip=@($TaskStance | ForEach-Object {$_.Slip})
        $TaskSlipP95=Get-TaskPercentile $TaskSlip 0.95
        if($TaskStance.Count -ge 6 -and $TaskP95 -gt 5.0){throw "$Profile/$Peer $($TaskGroup.Name) planted feet are physically above/below support (P95 gap=$TaskP95 cm)."}
        if($TaskStance.Count -ge 6 -and $TaskSlipP95 -gt 5.0){throw "$Profile/$Peer $($TaskGroup.Name) planted sole material vertices drift on the same support (P95=$TaskSlipP95 cm)."}
        if($TaskRows.Count -ge 12 -and $TaskContact.Count -lt 2){throw "$Profile/$Peer $($TaskGroup.Name) never demonstrates physical sole contact, regardless of bPlanted flags."}
        [pscustomobject]@{ Group=$TaskGroup.Name; Samples=$TaskRows.Count; PhysicalContactSamples=$TaskContact.Count; StablePlantSamples=$TaskStance.Count
            GapMin=($TaskRows | Measure-Object Gap -Minimum).Minimum; GapMax=($TaskRows | Measure-Object Gap -Maximum).Maximum
            PlantedAbsoluteGapP95=$TaskP95; SameSupportFixedVertexDriftP95=$TaskSlipP95
            SampledSlopeFootRows=@($TaskRows | Where-Object {$_.NormalZ -lt 0.99}).Count }
    })
    # The original facing driver owns this continuous capture channel and
    # its unchanged >=40-frame/turn/pitch gate. Do not take duplicate PNGs.
    $TaskFrames=@(Get-ChildItem -LiteralPath (Join-Path $Run ('Visual/'+$Peer)) -File -Filter 'FacingRemote_*.png' -ErrorAction SilentlyContinue | Where-Object Length -gt 0)
    if($TaskFrames.Count -lt 40){throw "$Profile/$Peer lacks the original forty consecutive facing captures."}
    [pscustomobject]@{ Peer=$Peer; Samples=$TaskAll.Count; ValidGroundedSamples=$TaskValid.Count; InvalidCpuSamples=@($TaskAll | Where-Object {-not $_.Cpu}).Count
        MinimumNormalFloorGap=$TaskMinimum; Coverage=$TaskCoverage; Groups=$TaskGroups
        Frames=@($TaskFrames | ForEach-Object {Get-TaskFileIdentity $_.FullName}); VisualQualityReviewed=$false }
}
$TaskReport=[ordered]@{Passed=$false;StartedUtc=[datetime]::UtcNow.ToString('o');Scenario=$Scenario;Failure=$null
    VisualQualityReviewed=$false;AudioReviewed=$false;PerformanceClaim=$false;SlopeStepMapClaim=$false;Profiles=@()
    SampledMotionDiagnostics=@()
    Scope='Player IK only: Local Editor DS and two rendered ordinary-input clients, with AI population disabled. CPU samples use configured LOD0 sole vertices of final postprocess poses; morph/cloth excluded. Runtime foot flags alone do not establish contact. Separate unit fixtures cover synthetic slopes/steps. PrivateUsers credentials must not be shared.'}
try {
    $TaskBefore=@(Get-TaskFrozenInputs)
    $TaskReport.FrozenInputs=$TaskBefore
    foreach($TaskProfile in $TaskProfiles) {
        $TaskArguments=@{ProjectRoot=$TaskProject;EngineRoot=$EngineRoot;Port=$TaskProfile.Port;TimeoutSeconds=$TimeoutSeconds
            FootIKReview=$true;FacingReview=$TaskProfile.FacingReview;FullGameplay=$TaskProfile.FullGameplay
            FacingStartServerTime=$FacingStartServerTime;EnemyPopulation=$TaskProfile.EnemyPopulation
            PacketLagMs=$PacketLagMs;PacketLagVarianceMs=$PacketLagVarianceMs;PacketLossPercent=$PacketLossPercent}
        $TaskTranscript=Join-Path $TaskOutput ($TaskProfile.Name+'_Runner.log')
        $TaskLines=@(& $TaskRunner @TaskArguments 2>&1 | Tee-Object -FilePath $TaskTranscript)
        $TaskMarker=@($TaskLines | ForEach-Object {[string]$_} | Where-Object {$_ -match '^VISUAL_ACCEPTANCE_REPORT '})
        if($TaskMarker.Count -ne 1){throw 'The ordinary rendered runner did not return a unique immutable result path.'}
        $TaskRunReport=$TaskMarker[0].Substring('VISUAL_ACCEPTANCE_REPORT '.Length)
        $TaskNormal=Get-Content -LiteralPath $TaskRunReport -Raw | ConvertFrom-Json
        if(-not $TaskNormal.Passed){throw "$($TaskProfile.Name) ordinary gameplay/facing acceptance failed."}
        $TaskRun=Split-Path -Parent $TaskRunReport
        $TaskPeers=@(foreach($TaskPeer in @('Client1','Client2')) {Measure-TaskPeer $TaskRun $TaskPeer $TaskProfile.Name})
        $TaskReport.Profiles+=[pscustomobject]@{Name=$TaskProfile.Name;OrdinaryRunnerReport=Get-TaskFileIdentity $TaskRunReport;Peers=$TaskPeers}
        if((@(Get-TaskFrozenInputs) | ConvertTo-Json -Compress) -cne ($TaskBefore | ConvertTo-Json -Compress)){throw 'Source, IK assets or module changed during acceptance.'}
    }
    $TaskReport.Passed=$true
} catch {$TaskReport.Failure=$_.Exception.Message;throw}
finally {
    $TaskReport.FinishedUtc=[datetime]::UtcNow.ToString('o')
    $TaskReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $TaskOutput 'Report.json') -Encoding UTF8
    Write-Output ('FOOT_IK_ACCEPTANCE_REPORT '+(Join-Path $TaskOutput 'Report.json'))
}
