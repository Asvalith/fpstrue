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
    [switch]$SelfTest,
    [string]$ValidateAutomationReport = '',
    [ValidateRange(1, 65534)][int]$Port = 7797,
    [ValidateRange(1, 10000)][int]$MinimumTests = 98,
    [ValidateRange(1, 16)][int]$MaxParallelActions = 4,
    [ValidateRange(1, 2000)][int]$MinimumDeliveryFreeGB = 50
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = Join-Path $PSScriptRoot '..\..' }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

# Approved Editor/Content suite after removing the enemy health-bar feature. A count alone
# cannot detect a missing asset-only test replaced by another successful test.
# Review this list together with source registrations when adding/removing cases.
$RequiredAutomationTests = @(
    'fpstrue.AI.BehaviorTree.AssetGraphAndBindings.DefaultProjectAssets'
    'fpstrue.AI.BehaviorTree.AssetSafety'
    'fpstrue.AI.BehaviorTree.BlackboardKeyCache'
    'fpstrue.AI.BehaviorTree.DefaultStructure'
    'fpstrue.AI.BehaviorTree.LowHealthRetreatLifecycle'
    'fpstrue.AI.BehaviorTree.MoveRequestOwnershipAndBackoff'
    'fpstrue.AI.BehaviorTree.RuntimeLifecycle'
    'fpstrue.AI.BehaviorTree.StartupFailureAndStopIdempotence'
    'fpstrue.AI.EQS.AsyncParametersAndMissingNavigation'
    'fpstrue.AI.EQS.TemplateAndItemIdentity'
    'fpstrue.AI.EQS.TwoOwnersInflightGcAndRecreate'
    'fpstrue.AI.Movement.NativeStationaryFloorCache'
    'fpstrue.AI.Perception.MultiplayerIdleSearchAndStop'
    'fpstrue.AI.Perception.AuthoritativeGunshotInvestigation'
    'fpstrue.AI.Perception.SightOcclusionAndFacing'
    'fpstrue.AI.Surround.CacheAndSlotOwnership'
    'fpstrue.AI.Surround.ParticipationAndGoalAvailability'
    'fpstrue.AI.Surround.AttackWaitingQueue'
    'fpstrue.AI.Surround.TacticalQueueAndRecovery'
    'fpstrue.AI.Surround.TacticalBoundaryAndAssignments'
    'fpstrue.Gameplay.Boundaries.AI.AnimationDefaultsConfig'
    'fpstrue.Gameplay.Boundaries.AI.FacingTolerance'
    'fpstrue.Gameplay.Boundaries.AI.RotationConfig'
    'fpstrue.Gameplay.Boundaries.GAS.CommitCancellationAndSameSpecRestart'
    'fpstrue.Gameplay.Boundaries.Weapon.AssetReloadNormalAndEmpty'
    'fpstrue.Gameplay.Boundaries.Weapon.AutoReloadConflicts'
    'fpstrue.Gameplay.Boundaries.Weapon.ConfigurationSnapshotAndReequip'
    'fpstrue.Gameplay.Boundaries.Weapon.ConfigurationValidation'
    'fpstrue.Gameplay.Boundaries.Weapon.ExternalOwnerAndEquipReentry'
    'fpstrue.Gameplay.Boundaries.Weapon.HitscanDamage'
    'fpstrue.Gameplay.Boundaries.Weapon.StaleTransactionCallbacksIgnored'
    'fpstrue.Gameplay.Boundaries.Weapon.OwnerInputTransitionRejectsReentry'
    'fpstrue.Gameplay.Boundaries.Weapon.PlaybackFailureAndEndOwnership'
    'fpstrue.Gameplay.Boundaries.Weapon.RecoilUsesElapsedTime'
    'fpstrue.Gameplay.Boundaries.Weapon.ReloadFinishPreservesDisabledOnReentry'
    'fpstrue.Gameplay.Boundaries.Weapon.ReloadIdentityAndEndReasons'
    'fpstrue.Gameplay.Boundaries.Weapon.ReloadMontageInstanceIdentity'
    'fpstrue.Gameplay.Boundaries.Weapon.ReloadTransitions'
    'fpstrue.Gameplay.Boundaries.Weapon.RemainingCooldownAndShotFact'
    'fpstrue.Gameplay.Boundaries.Weapon.ShotPresentationIdentityOnReentry'
    'fpstrue.Gameplay.Combat.AttackProtectionAndCleanup'
    'fpstrue.Gameplay.Combat.ConfigurationSource'
    'fpstrue.Gameplay.Combat.ExplicitPlaybackAndStopReentry'
    'fpstrue.Gameplay.Combat.ProjectBlueprintPlaybackIntegration'
    'fpstrue.Gameplay.Health.NotificationTermination'
    'fpstrue.Gameplay.Health.QueuedEnemyHitContext'
    'fpstrue.Gameplay.Health.ReentrantMutationOrder'
    'fpstrue.Gameplay.Weapon.VisualRecoil.PulseAndScaling'
    'fpstrue.Gameplay.Weapon.VisualRecoil.RetriggerAndStepSize'
    'fpstrue.Gameplay.Weapon.VisualRecoil.ViewIsolation'
    'fpstrue.Multiplayer.Animation.BodyDefaultsAndViewSeparation'
    'fpstrue.Multiplayer.Animation.RARAbilityPresentationAndCancel'
    'fpstrue.Multiplayer.Animation.RARAssetsAndMontageBindings'
    'fpstrue.Multiplayer.Animation.RARReceivedReloadProgress'
    'fpstrue.Multiplayer.Animation.RemoteVelocityWithoutAcceleration'
    'fpstrue.Multiplayer.Animation.TemplateAssetsGripAndOriginalHud'
    'fpstrue.Multiplayer.Animation.TemplateDeathWeaponPhysicsAndLateReplication'
    'fpstrue.Multiplayer.GAS.ExternalHealthEffectsAndReentry'
    'fpstrue.Multiplayer.Health.AuthorityAndGASAttributeCommit'
    'fpstrue.Multiplayer.Health.ReplicatedAttributeNotifications'
    'fpstrue.Multiplayer.Hitscan.HistoryGeometryTimeAndLifecycle'
    'fpstrue.Multiplayer.Lifecycle.BoundedSpawnRecovery'
    'fpstrue.Multiplayer.Lifecycle.EnemyBehaviorStartupRequired'
    'fpstrue.Multiplayer.Lifecycle.SingleModeEnemyRegistrationAndSpawn'
    'fpstrue.Multiplayer.Movement.SavedModesReplayAndAuthority'
    'fpstrue.Multiplayer.Performance.AuthorityPoseNeverUsesClientBudget'
    'fpstrue.Multiplayer.Performance.ClientLocalRenderAndVelocityState'
    'fpstrue.Multiplayer.Performance.EnemyReplicationTieringAndReference'
    'fpstrue.Multiplayer.Performance.ServerAllParticipantsAndLifecycle'
    'fpstrue.Multiplayer.PvPvE.ClientSnapshotIsPresentationOnly'
    'fpstrue.Multiplayer.PvPvE.ServerCombatAndSingleCommit'
    'fpstrue.Multiplayer.PvPvE.SimpleHitProxyLifecycle'
    'fpstrue.Multiplayer.PvPvE.TargetDeathDestroyAndResourceRelease'
    'fpstrue.Multiplayer.PvPvE.WorldSharedNavigationBudget'
    'fpstrue.Multiplayer.Reconnect.CancelInvalidatesOldTravelAndRetry'
    'fpstrue.Multiplayer.Reconnect.HudRebindAndCurrentSnapshot'
    'fpstrue.Multiplayer.Reconnect.ServerRetentionAndSingleUseClaim'
    'fpstrue.Multiplayer.Reconnect.DeferredTravelAndBoundedActiveSlotRetry'
    'fpstrue.Multiplayer.Clock.BetterSampleRebasesStartupEstimate'
    'fpstrue.Multiplayer.Clock.DynamicRoundTripAndFreshness'
    'fpstrue.Multiplayer.Prediction.AmmoSnapshotAckReordering'
    'fpstrue.Multiplayer.Prediction.ReloadPlaybackAdoptionAndAuthorityClock'
    'fpstrue.Multiplayer.Prediction.BoundedShotTransportReorderAndTail'
    'fpstrue.Multiplayer.Prediction.BatchFireAuthorizationAndNativeIsolation'
    'fpstrue.Multiplayer.Weapon.AuthorityAndReplicatedState'
    'fpstrue.Multiplayer.Weapon.ReplicationAudienceAndArrivalOrder'
    'fpstrue.Multiplayer.Weapon.ShotIdentityCadenceAndLifecycle'
    'fpstrue.Performance.AnimationSharing.StopAndSwapLifecycle'
    'fpstrue.Performance.Significance.RenderPriorityOrder'
    'fpstrue.Performance.Significance.GameplayTierReasonSnapshot'
    'fpstrue.Performance.Significance.RenderBudgetRetentionOrder'
    'fpstrue.Performance.Significance.RenderScoringSnapshotEquivalence'
    'fpstrue.Performance.Significance.RenderTopKSelection'
    'fpstrue.Performance.Significance.ScopedCombatProtection'
    'fpstrue.Rendering.EnemyBudget.MeshOwnershipAndDeath'
    'fpstrue.Rendering.EnemyBudget.IndependentBudgetRetention'
    'fpstrue.Rendering.EnemyBudget.NoPostProcessAcrossLifecycle'
    'fpstrue.Rendering.EnemyBudget.TierHysteresis'
)

function Get-DeliveryAutomationListHash {
    $Hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes(($RequiredAutomationTests -join "`n"))
        return ([BitConverter]::ToString($Hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $Hasher.Dispose() }
}

# This is a conservative source-text audit, not a substitute for UE discovery.
# Static complex-test children must remain enumerable; dynamic enumeration needs
# an explicit auditor/list review rather than silently weakening the requirement.
function Test-DeliveryAutomationSourceManifest([string]$Root) {
    $Registered = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $Files = [Collections.Generic.List[object]]::new()
    $Pattern = '(?m)^\s*IMPLEMENT_(SIMPLE|COMPLEX)_AUTOMATION_TEST\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"]+)"'
    foreach ($File in Get-ChildItem -LiteralPath (Join-Path $Root 'Source\fpstrue') -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.h', '.inl') }) {
        $Text = [IO.File]::ReadAllText($File.FullName)
        $Registrations = @([regex]::Matches($Text, $Pattern))
        $MacroCount = [regex]::Matches($Text, '(?m)^\s*(?:IMPLEMENT_[A-Z_]*AUTOMATION_TEST|(?:BEGIN_)?DEFINE_SPEC)\s*\(').Count
        if ($MacroCount -ne $Registrations.Count) { throw "Automation manifest rejected: unsupported registration in $($File.FullName)." }
        if ($Registrations.Count -eq 0) { continue }
        $Files.Add([pscustomobject]@{ Path = $File.FullName; SHA256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash })
        foreach ($Registration in $Registrations) {
            $Prefix = $Registration.Groups[3].Value
            $Names = @($Prefix)
            if ($Registration.Groups[1].Value -eq 'COMPLEX') {
                $Class = [regex]::Escape($Registration.Groups[2].Value)
                $Body = [regex]::Match($Text, ('(?ms)^void\s+' + $Class + '::GetTests\([^)]*\)\s*const\s*\{(.*?)^\}'))
                $Children = @([regex]::Matches($Body.Groups[1].Value, 'OutBeautifiedNames\.Add\s*\(\s*TEXT\s*\(\s*"([^"]+)"\s*\)\s*\)'))
                if (-not $Body.Success -or $Children.Count -eq 0) { throw "Automation manifest rejected: cannot enumerate complex test $Prefix." }
                $Names = @($Children | ForEach-Object { $Prefix + '.' + $_.Groups[1].Value })
            }
            foreach ($Name in $Names) {
                if (-not $Registered.Add($Name)) { throw "Automation manifest rejected: duplicate source test $Name." }
            }
        }
    }
    $Approved = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Name in $RequiredAutomationTests) {
        if (-not $Approved.Add($Name)) { throw "Automation manifest rejected: duplicate approved test $Name." }
        if (-not $Registered.Contains($Name)) { throw "Automation manifest rejected: required test no longer registered: $Name." }
    }
    foreach ($Name in $Registered) {
        if (-not $Approved.Contains($Name)) { throw "Automation manifest rejected: unreviewed source test $Name. Update the approved list deliberately." }
    }
    return [pscustomobject]@{ RequiredCount = $Approved.Count; RegisteredCount = $Registered.Count
        RequiredListSHA256 = Get-DeliveryAutomationListHash; SourceFiles = @($Files.ToArray())
        Scope = 'Source registration audit only; actual Editor/Content test discovery is verified from report rows.' }
}

function Read-DeliveryAutomationCount($Object, [string]$Name, [string]$Context) {
    $Property = if ($null -ne $Object) { $Object.PSObject.Properties[$Name] } else { $null }
    $Value = if ($null -ne $Property) { $Property.Value } else { $null }
    if ($null -eq $Value -or $Value -is [bool] -or
        -not ($Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or $Value -is [uint16] -or
              $Value -is [int32] -or $Value -is [uint32] -or $Value -is [int64] -or $Value -is [uint64]) -or
        $Value -lt 0 -or $Value -gt [int]::MaxValue) {
        throw "Automation report rejected: $Context.$Name must be a non-negative integer."
    }
    return [long]$Value
}

# Keep JSON decoding shared as well as semantic validation. Requiring an object
# root avoids PowerShell pipeline unrolling accepting a one-item JSON array.
function ConvertFrom-DeliveryAutomationJson([string]$JsonText) {
    if ([string]::IsNullOrWhiteSpace($JsonText) -or -not $JsonText.Trim().StartsWith('{') -or
        -not $JsonText.Trim().EndsWith('}')) { throw 'Automation report rejected: JSON must contain a non-empty object root.' }
    try { return ($JsonText | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "Automation report rejected: unreadable JSON: $($_.Exception.Message)" }
}

# Pure validator shared by current-run acceptance, historical inspection and
# in-memory negative fixtures. Warnings are counted and exposed, never renamed
# clean passes. Deliberate negative gameplay tests currently produce warnings.
function Assert-DeliveryAutomationDocument($Document, [int]$MinimumCount = $RequiredAutomationTests.Count,
    [datetime]$RunStartedUtc = [datetime]::MinValue, [datetime]$RunFinishedUtc = [datetime]::MinValue,
    [datetime]$FileCreatedUtc = [datetime]::MinValue, [datetime]$FileWrittenUtc = [datetime]::MinValue) {
    if ($null -eq $Document -or $Document -isnot [pscustomobject]) { throw 'Automation report rejected: root must be a JSON object.' }
    $Counts = @{}
    foreach ($Field in @('succeeded', 'succeededWithWarnings', 'failed', 'notRun', 'inProcess')) {
        $Counts[$Field] = Read-DeliveryAutomationCount $Document $Field 'root'
    }
    $DateProperty = $Document.PSObject.Properties['reportCreatedOn']
    $CreatedUtc = [datetime]::MinValue
    $DateStyle = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    if ($null -eq $DateProperty -or $DateProperty.Value -isnot [string] -or
        -not [datetime]::TryParseExact($DateProperty.Value, 'yyyy.MM.dd-HH.mm.ss', [Globalization.CultureInfo]::InvariantCulture,
            $DateStyle, [ref]$CreatedUtc)) { throw 'Automation report rejected: invalid/missing UTC reportCreatedOn.' }
    $TestProperty = $Document.PSObject.Properties['tests']
    if ($null -eq $TestProperty -or $TestProperty.Value -isnot [array] -or $TestProperty.Value.Count -eq 0) {
        throw 'Automation report rejected: tests must be a non-empty array.'
    }
    $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $RowTimestamps = [Collections.Generic.List[datetime]]::new()
    $Clean = 0; $Warned = 0; $WarningEvents = 0L
    $WarningTests = [Collections.Generic.List[object]]::new()
    foreach ($Test in $TestProperty.Value) {
        if ($null -eq $Test -or $Test -isnot [pscustomobject]) { throw 'Automation report rejected: each test must be an object.' }
        $NameProperty = $Test.PSObject.Properties['fullTestPath']
        $Name = if ($null -ne $NameProperty) { $NameProperty.Value } else { $null }
        if ($Name -isnot [string] -or [string]::IsNullOrWhiteSpace($Name) -or $Name -cne $Name.Trim() -or
            $RequiredAutomationTests -cnotcontains $Name) { throw "Automation report rejected: missing or unapproved fullTestPath '$Name'." }
        if (-not $Seen.Add($Name)) { throw "Automation report rejected: duplicate fullTestPath $Name." }
        $RowDateProperty = $Test.PSObject.Properties['dateTime']
        $RowUtc = [datetime]::MinValue
        if ($null -eq $RowDateProperty -or $RowDateProperty.Value -isnot [string] -or
            -not [datetime]::TryParseExact($RowDateProperty.Value, 'yyyy.MM.dd-HH.mm.ss', [Globalization.CultureInfo]::InvariantCulture,
                $DateStyle, [ref]$RowUtc)) { throw "Automation report rejected: invalid/missing UTC dateTime for $Name." }
        $RowTimestamps.Add($RowUtc)
        $StateProperty = $Test.PSObject.Properties['state']
        if ($null -eq $StateProperty -or $StateProperty.Value -isnot [string] -or $StateProperty.Value -cne 'Success') {
            throw "Automation report rejected: $Name did not finish with Success."
        }
        $Errors = Read-DeliveryAutomationCount $Test 'errors' $Name
        $Warnings = Read-DeliveryAutomationCount $Test 'warnings' $Name
        if ($Errors -ne 0) { throw "Automation report rejected: $Name has $Errors errors despite its state." }
        $EntryProperty = $Test.PSObject.Properties['entries']
        if ($null -eq $EntryProperty -or $EntryProperty.Value -isnot [array]) {
            throw "Automation report rejected: $Name.entries must be an array (empty is valid)."
        }
        $ListedWarnings = 0
        foreach ($Entry in $EntryProperty.Value) {
            if ($null -eq $Entry -or $Entry -isnot [pscustomobject]) { throw "Automation report rejected: malformed entry for $Name." }
            $EventProperty = $Entry.PSObject.Properties['event']
            $Event = if ($null -ne $EventProperty) { $EventProperty.Value } else { $null }
            $TypeProperty = if ($null -ne $Event) { $Event.PSObject.Properties['type'] } else { $null }
            if ($Event -isnot [pscustomobject] -or $null -eq $TypeProperty -or $TypeProperty.Value -isnot [string]) {
                throw "Automation report rejected: malformed event for $Name."
            }
            if ($TypeProperty.Value -ceq 'Error') { throw "Automation report rejected: embedded Error event contradicts Success/errors=0 for $Name." }
            if ($TypeProperty.Value -ceq 'Warning') { $ListedWarnings++ }
            elseif ($TypeProperty.Value -cne 'Info') { throw "Automation report rejected: unknown event type for $Name." }
        }
        # UE can omit captured/expected warning entries while retaining their
        # counter. Empty entries with warnings are legal; explicit entries must
        # not reveal more warnings than the report admits.
        if ($ListedWarnings -gt $Warnings) { throw "Automation report rejected: explicit Warning events exceed the warning counter for $Name." }
        if ($Warnings -gt 0) { $Warned++; $WarningEvents += $Warnings; $WarningTests.Add([pscustomobject]@{ Name = $Name; Warnings = $Warnings }) }
        else { $Clean++ }
    }
    foreach ($Name in $RequiredAutomationTests) {
        if (-not $Seen.Contains($Name)) { throw "Automation report rejected: required case absent: $Name." }
    }
    if ($Counts.succeeded -ne $Clean -or $Counts.succeededWithWarnings -ne $Warned -or
        $Counts.failed -ne 0 -or $Counts.notRun -ne 0 -or $Counts.inProcess -ne 0 -or
        $Clean + $Warned -ne $TestProperty.Value.Count -or $Seen.Count -lt [Math]::Max($MinimumCount, $RequiredAutomationTests.Count)) {
        throw 'Automation report rejected: summary counters disagree with test rows or the required minimum.'
    }
    $Fresh = $RunStartedUtc -ne [datetime]::MinValue -or $RunFinishedUtc -ne [datetime]::MinValue
    if ($Fresh) {
        if ($RunStartedUtc -eq [datetime]::MinValue -or $RunFinishedUtc -lt $RunStartedUtc) {
            throw 'Automation report rejected: current-run freshness requires both valid UTC invocation bounds.'
        }
        # UE timestamps have one-second precision. A two-second boundary
        # tolerance handles rounding, not a license to reuse an earlier run.
        foreach ($Time in (@($CreatedUtc, $FileCreatedUtc, $FileWrittenUtc) + @($RowTimestamps.ToArray()))) {
            if ($Time -lt $RunStartedUtc.AddSeconds(-2) -or $Time -gt $RunFinishedUtc.AddSeconds(2)) {
                throw 'Automation report rejected: report/test timestamp or file creation/write time is outside this invocation.'
            }
        }
    }
    return [pscustomobject]@{ Passed = $Seen.Count; CleanPassed = $Clean; Warnings = $Warned; WarningEvents = $WarningEvents
        WarningTests = @($WarningTests.ToArray()); RequiredCount = $RequiredAutomationTests.Count
        RequiredListSHA256 = Get-DeliveryAutomationListHash; RequiredTests = $RequiredAutomationTests
        ReportCreatedUtc = $CreatedUtc.ToString('o'); FreshnessChecked = $Fresh
        Scope = $(if ($Fresh) { 'Current invocation report structure, required cases and timestamps verified; process exit is checked separately. Not visual, public-network or performance acceptance.' }
                  else { 'Historical/offline report structure only. No engine was launched and no current build, gameplay or runtime acceptance is claimed.' }) }
}

function Get-DeliveryAutomationReport([string]$Path, [int]$MinimumCount = $RequiredAutomationTests.Count,
    [datetime]$RunStartedUtc = [datetime]::MinValue, [datetime]$RunFinishedUtc = [datetime]::MinValue) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Automation report rejected: missing report $Path." }
    $BeforeHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    $Document = ConvertFrom-DeliveryAutomationJson (Get-Content -LiteralPath $Path -Raw)
    $AfterHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($BeforeHash -ne $AfterHash) { throw 'Automation report rejected: report changed while being read.' }
    $File = Get-Item -LiteralPath $Path
    $Verified = Assert-DeliveryAutomationDocument $Document $MinimumCount $RunStartedUtc $RunFinishedUtc $File.CreationTimeUtc $File.LastWriteTimeUtc
    $Verified | Add-Member -NotePropertyName Report -NotePropertyValue $File.FullName
    $Verified | Add-Member -NotePropertyName ReportSHA256 -NotePropertyValue $AfterHash
    return $Verified
}

function Assert-DeliveryCookEngineMatch([string]$RuntimeRoot, [string]$CookerRoot) {
    # Version/CL equality cannot establish shader or native serialization ABI compatibility.
    # BuildEditor below refreshes the cooker from this same source engine before cooking.
    $RuntimePath = [IO.Path]::GetFullPath($RuntimeRoot).TrimEnd([char[]]'\/')
    $CookerPath = [IO.Path]::GetFullPath($CookerRoot).TrimEnd([char[]]'\/')
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($RuntimePath, $CookerPath)) {
        throw 'Cooker/runtime engine mismatch: LinuxPackage requires the same resolved source-engine root for building and cooking. Matching version numbers are insufficient; build its Editor rather than mixing in an installed cooker.'
    }
}

function Invoke-DeliveryAutomationSelfTest {
    $Audit = Test-DeliveryAutomationSourceManifest $ProjectRoot
    $At = [datetime]::SpecifyKind([datetime]'2026-10-17T12:00:00', [DateTimeKind]::Utc)
    $Fixture = [pscustomobject]@{ reportCreatedOn = $At.AddSeconds(2).ToString('yyyy.MM.dd-HH.mm.ss')
        succeeded = $RequiredAutomationTests.Count; succeededWithWarnings = 0; failed = 0; notRun = 0; inProcess = 0
        tests = @($RequiredAutomationTests | ForEach-Object { [pscustomobject]@{ fullTestPath = $_; state = 'Success'; warnings = 0; errors = 0; entries = @()
            dateTime = $At.AddSeconds(1).ToString('yyyy.MM.dd-HH.mm.ss') } }) }
    $FixtureJson = $Fixture | ConvertTo-Json -Depth 6
    $Fixture = ConvertFrom-DeliveryAutomationJson $FixtureJson
    $Checks = [Collections.Generic.List[string]]::new()
    Assert-DeliveryCookEngineMatch 'C:\UE55_Source\' 'c:/UE55_Source'
    $Checks.Add('same cooker/runtime root tolerates path casing and trailing separator')
    $RejectedCooker = $false
    try { Assert-DeliveryCookEngineMatch 'C:\UE55_Source' 'C:\UE55_Installed' }
    catch { if ($_.Exception.Message -notlike 'Cooker/runtime engine mismatch:*') { throw }; $RejectedCooker = $true }
    if (-not $RejectedCooker) { throw 'SelfTest accepted a different cooker engine root.' }
    $Checks.Add('different cooker root fails before any engine launch or output creation')
    $Result = Assert-DeliveryAutomationDocument $Fixture $RequiredAutomationTests.Count $At $At.AddSeconds(10) $At.AddSeconds(1) $At.AddSeconds(3)
    if ($Result.Passed -ne $RequiredAutomationTests.Count -or -not $Result.FreshnessChecked) { throw 'SelfTest valid fixture failed.' }
    $Checks.Add('valid required tests and current invocation timestamps')
    $WarnFixture = ConvertFrom-DeliveryAutomationJson $FixtureJson
    $WarnFixture.tests[0].warnings = 3; $WarnFixture.succeeded--; $WarnFixture.succeededWithWarnings++
    $Result = Assert-DeliveryAutomationDocument $WarnFixture
    if ($Result.CleanPassed -ne ($RequiredAutomationTests.Count - 1) -or $Result.Warnings -ne 1 -or $Result.WarningEvents -ne 3 -or $Result.FreshnessChecked) {
        throw 'SelfTest warning accounting/offline scope failed.'
    }
    $Checks.Add('legitimate warned success remains distinct from clean success')
    $Cases = [ordered]@{
        'missing required row even with minimum set to one' = { param($D) $D.tests = @($D.tests | Select-Object -Skip 1); $D.succeeded-- }
        'replacement cannot hide a missing required case' = { param($D) $D.tests[0].fullTestPath = 'fpstrue.UnapprovedReplacement' }
        'duplicate report rows' = { param($D) $D.tests = @($D.tests) + @($D.tests[0]); $D.succeeded++ }
        'failed row despite green summary' = { param($D) $D.tests[0].state = 'Fail' }
        'unrun row' = { param($D) $D.tests[0].state = 'NotRun' }
        'in-progress row' = { param($D) $D.tests[0].state = 'InProcess' }
        'skipped row' = { param($D) $D.tests[0].state = 'Skipped' }
        'unknown state' = { param($D) $D.tests[0].state = 'Unexpected' }
        'empty tests' = { param($D) $D.tests = @() }
        'null tests' = { param($D) $D.tests = $null }
        'non-array tests' = { param($D) $D.tests = $D.tests[0] }
        'string tests' = { param($D) $D.tests = 'Success' }
        'null test row' = { param($D) $D.tests[0] = $null }
        'errors on a Success row' = { param($D) $D.tests[0].errors = 1 }
        'warning counter mismatch' = { param($D) $D.tests[0].warnings = 1 }
        'summary count mismatch' = { param($D) $D.succeeded++ }
        'nonzero aggregate failed count' = { param($D) $D.failed = 1 }
        'nonzero aggregate notRun count' = { param($D) $D.notRun = 1 }
        'nonzero aggregate inProcess count' = { param($D) $D.inProcess = 1 }
        'missing aggregate field' = { param($D) $D.PSObject.Properties.Remove('failed') }
        'negative counter' = { param($D) $D.failed = -1 }
        'string counter' = { param($D) $D.failed = '0' }
        'fractional counter' = { param($D) $D.failed = 0.5 }
        'floating-point integer counter' = { param($D) $D.failed = [double]0 }
        'oversized integer counter' = { param($D) $D.failed = [long]2147483648 }
        'boolean counter' = { param($D) $D.failed = $false }
        'missing per-test error field' = { param($D) $D.tests[0].PSObject.Properties.Remove('errors') }
        'missing per-test warning field' = { param($D) $D.tests[0].PSObject.Properties.Remove('warnings') }
        'missing entries field' = { param($D) $D.tests[0].PSObject.Properties.Remove('entries') }
        'null entries' = { param($D) $D.tests[0].entries = $null }
        'string entries' = { param($D) $D.tests[0].entries = 'Info' }
        'null entry' = { param($D) $D.tests[0].entries = @($null) }
        'missing event in entry' = { param($D) $D.tests[0].entries = @([pscustomobject]@{}) }
        'malformed event object' = { param($D) $D.tests[0].entries = @([pscustomobject]@{ event = 'Error' }) }
        'embedded Error cannot contradict Success' = { param($D) $D.tests[0].entries = @([pscustomobject]@{ event = [pscustomobject]@{ type = 'Error' } }) }
        'unknown event type' = { param($D) $D.tests[0].entries = @([pscustomobject]@{ event = [pscustomobject]@{ type = 'Unknown' } }) }
        'explicit Warning exceeds counter' = { param($D) $D.tests[0].entries = @([pscustomobject]@{ event = [pscustomobject]@{ type = 'Warning' } }) }
        'blank path' = { param($D) $D.tests[0].fullTestPath = ' ' }
        'path casing is not a replacement for the required identity' = { param($D) $D.tests[0].fullTestPath = $D.tests[0].fullTestPath.ToUpperInvariant() }
        'malformed timestamp' = { param($D) $D.reportCreatedOn = 'not-a-date' }
        'missing timestamp' = { param($D) $D.PSObject.Properties.Remove('reportCreatedOn') }
        'missing test timestamp' = { param($D) $D.tests[0].PSObject.Properties.Remove('dateTime') }
        'malformed test timestamp' = { param($D) $D.tests[0].dateTime = 'not-a-date' }
        'old row cannot hide under a fresh report header' = { param($D) $D.tests[0].dateTime = '2000.01.01-00.00.00' }
        'future test timestamp' = { param($D) $D.tests[0].dateTime = '2099.01.01-00.00.00' }
        'stale report timestamp in a fresh file' = { param($D) $D.reportCreatedOn = '2000.01.01-00.00.00' }
        'future report timestamp' = { param($D) $D.reportCreatedOn = '2099.01.01-00.00.00' }
    }
    foreach ($Case in $Cases.GetEnumerator()) {
        $Document = ConvertFrom-DeliveryAutomationJson $FixtureJson
        & $Case.Value $Document
        $Rejected = $false
        try { $null = Assert-DeliveryAutomationDocument $Document 1 $At $At.AddSeconds(10) $At.AddSeconds(1) $At.AddSeconds(3) }
        catch {
            if ($_.Exception.Message -notlike 'Automation report rejected:*') { throw }
            $Rejected = $true
        }
        if (-not $Rejected) { throw "SelfTest accepted invalid fixture: $($Case.Key)" }
        $Checks.Add($Case.Key)
    }
    foreach ($Case in @('stale file creation', 'stale file write', 'missing invocation end')) {
        $Created = $At.AddSeconds(1); $Written = $At.AddSeconds(3); $End = $At.AddSeconds(10)
        if ($Case -eq 'stale file creation') { $Created = $At.AddDays(-1) }
        if ($Case -eq 'stale file write') { $Written = $At.AddDays(-1) }
        if ($Case -eq 'missing invocation end') { $End = [datetime]::MinValue }
        $Rejected = $false
        try { $null = Assert-DeliveryAutomationDocument $Fixture $RequiredAutomationTests.Count $At $End $Created $Written }
        catch { if ($_.Exception.Message -notlike 'Automation report rejected:*') { throw }; $Rejected = $true }
        if (-not $Rejected) { throw "SelfTest accepted invalid fixture: $Case" }
        $Checks.Add($Case)
    }
    foreach ($InvalidJson in @('', ' ', '{', '{"tests": [}', 'null', '[]', ('[' + $FixtureJson + ']'))) {
        $Rejected = $false
        try { $null = ConvertFrom-DeliveryAutomationJson $InvalidJson }
        catch { if ($_.Exception.Message -notlike 'Automation report rejected:*') { throw }; $Rejected = $true }
        if (-not $Rejected) { throw 'SelfTest accepted empty, broken or non-object JSON.' }
        $Checks.Add('JSON decoding rejects empty/broken/non-object input')
    }
    $AbsentReport = Join-Path ([IO.Path]::GetTempPath()) ('fpstrue-report-absent-' + [guid]::NewGuid().ToString('N') + '\index.json')
    $Rejected = $false
    try { $null = Get-DeliveryAutomationReport $AbsentReport }
    catch { if ($_.Exception.Message -notlike 'Automation report rejected:*') { throw }; $Rejected = $true }
    if (-not $Rejected) { throw 'SelfTest accepted a missing report file.' }
    $Checks.Add('missing GUID report path fails without creating files')
    return [pscustomobject]@{ Mode = 'SelfTest'; Passed = $Checks.Count; Checks = @($Checks.ToArray()); SourceAudit = $Audit
        Scope = 'Offline validator and source-manifest checks only. No UE, build, game, networking or process launch.' }
}

# Offline modes deliberately precede every engine prerequisite, output-directory
# creation and native command. Their success is not current gameplay acceptance.
if ($SelfTest -and $ValidateAutomationReport) { throw 'Choose either -SelfTest or -ValidateAutomationReport.' }
if ($SelfTest) { Invoke-DeliveryAutomationSelfTest | ConvertTo-Json -Depth 8; return }
if ($ValidateAutomationReport) {
    $SourceAudit = Test-DeliveryAutomationSourceManifest $ProjectRoot
    $Verified = Get-DeliveryAutomationReport $ValidateAutomationReport $MinimumTests
    $Verified | Add-Member -NotePropertyName SourceAudit -NotePropertyValue $SourceAudit
    $Verified | ConvertTo-Json -Depth 8
    return
}

if (-not $EngineRoot) { throw 'Pass -EngineRoot or set UE_ROOT to a UE 5.5 engine directory.' }
$EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
if ($Mode -eq 'LinuxPackage') {
    if (-not $CookEngineRoot) { $CookEngineRoot = $EngineRoot }
    $CookEngineRoot = (Resolve-Path -LiteralPath $CookEngineRoot).Path
    Assert-DeliveryCookEngineMatch $EngineRoot $CookEngineRoot
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
# installed-engine client merely because its version/CL matches the source engine.
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
        if ($Installed) { throw 'LinuxPackage requires one source engine, including its built Editor, for both runtime targets and cooking.' }
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
        $MinimumTests = [Math]::Max($MinimumTests, $RequiredAutomationTests.Count)
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
    # Validate the fail-closed parsers before expensive builds. These checks do
    # not launch UE; the runtime reports below remain separate acceptance gates.
    $ContractSelfTest = Invoke-DeliveryAutomationSelfTest
    $Report['AutomationContractSelfTest'] = $ContractSelfTest
    Invoke-DeliveryStep 'NetworkContractSelfTest' 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $PSScriptRoot 'RunPvpSmoke.ps1'), '-ProjectRoot', $ProjectRoot, '-SelfTest')
    $BuildArgs = @('Win64', 'Development', ('-Project=' + $Project), '-WaitMutex', '-NoHotReloadFromIDE') + $BuildExecutionArguments
    Invoke-DeliveryStep 'BuildEditor' $CookBuild (@('fpstrueEditor') + $BuildArgs)
    $AutomationDir = Join-Path $Output 'Automation'
    $AutomationLog = Join-Path $Output 'AutomationEngine.log'
    $AutomationStartedUtc = [datetime]::UtcNow
    Invoke-DeliveryStep 'AutomationRunner' $Editor @(
        $Project, '-Unattended', '-NullRHI', '-NoSound', '-NoSplash', '-NoSteam', '-NoLiveCoding',
        '-ddc=NoZenLocalFallback', ('-LocalDataCachePath=' + (Join-Path $ProjectRoot 'Saved\LocalDDC')),
        '-ExecCmds=Automation RunTests fpstrue.', '-TestExit=Automation Test Queue Empty',
        ('-ReportExportPath=' + $AutomationDir), ('-abslog=' + $AutomationLog))
    $AutomationFinishedUtc = [datetime]::UtcNow
    $Report.Automation = Get-DeliveryAutomationReport (Join-Path $AutomationDir 'index.json') $MinimumTests $AutomationStartedUtc $AutomationFinishedUtc
    $Report.Automation | Add-Member -NotePropertyName SourceAudit -NotePropertyValue (Test-DeliveryAutomationSourceManifest $ProjectRoot)
    if ($Mode -eq 'LinuxPackage') {
        # The same source engine supplies the cooker and both runtime targets;
        # never substitute an installed-engine cooker or Game executable.
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
