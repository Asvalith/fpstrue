param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,
    [double]$StartTrimSeconds = 0,
    [double]$EndTrimSeconds = 0
)

$ErrorActionPreference = "Stop"

foreach ($TrimSeconds in @($StartTrimSeconds, $EndTrimSeconds)) {
    if ([double]::IsNaN($TrimSeconds) -or [double]::IsInfinity($TrimSeconds) -or $TrimSeconds -lt 0) {
        throw "StartTrimSeconds and EndTrimSeconds must be finite and non-negative."
    }
}

function Get-NumericValues {
    param([object[]]$Rows, [string]$Column, [switch]$Required, [string]$Context = "CSV")

    $Values = New-Object System.Collections.Generic.List[double]
    if ($Rows.Count -eq 0 -or $null -eq $Rows[0].PSObject.Properties[$Column]) {
        if ($Required) { throw "$Context`: required column '$Column' is missing or has no samples." }
        return $null
    }
    $Sample = 0
    foreach ($Row in $Rows) {
        ++$Sample
        $Value = 0.0
        $Parsed = [double]::TryParse(
                [string]$Row.$Column,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$Value)
        if (-not $Parsed -or [double]::IsNaN($Value) -or [double]::IsInfinity($Value)) {
            if ($Required) { throw "$Context`: required column '$Column' has a missing, non-numeric or non-finite value at sample $Sample." }
            # Optional metrics require complete finite samples; partial data is not averaged as zero.
            return $null
        }
        $Values.Add($Value)
    }
    return $Values.ToArray()
}

function Get-Average {
    param([object[]]$Values)
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    foreach ($Value in $Values) {
        if ($null -eq $Value) { return $null }
    }
    return ($Values | Measure-Object -Average).Average
}

function Get-GroupAverage {
    param([object[]]$Rows, [string]$Column)
    $Values = New-Object System.Collections.Generic.List[object]
    foreach ($Row in $Rows) { $Values.Add($Row.$Column) }
    return Get-Average -Values $Values.ToArray()
}

function Round-Nullable {
    param([object]$Value, [int]$Digits = 3)
    if ($null -eq $Value) { return $null }
    return [Math]::Round([double]$Value, $Digits)
}

function Get-NullableDifference {
    param([object]$Baseline, [object]$Value)
    if ($null -eq $Baseline -or $null -eq $Value) { return $null }
    return [Math]::Round([double]$Baseline - [double]$Value, 3)
}

function Format-Nullable {
    param([object]$Value, [string]$Unit = "")
    if ($null -eq $Value) { return "N/A" }
    return "$Value$Unit"
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { return 0.0 }
    $Sorted = @($Values | Sort-Object)
    $Index = [Math]::Ceiling($Percentile * $Sorted.Count) - 1
    return $Sorted[[Math]::Max(0, [Math]::Min($Index, $Sorted.Count - 1))]
}

function Get-Spread {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $Measure = $Values | Measure-Object -Minimum -Maximum
    return $Measure.Maximum - $Measure.Minimum
}

function Get-ConsumerSampleSummary {
    param([object[]]$Rows, [object]$Entry)

    $Result = [ordered]@{
        AliveEnemies = $null
        ShadowCasters = $null
        RayTracingVisible = $null
        ConsumerSampleFrames = 0
        ConsumerSamplingBasis = "MissingCoordinatorSampleMask"
    }
    # CSV_CUSTOM_STAT(Set) here is emitted only in Coordinator::Update (normally
    # every 0.25 s). UE fills unrecorded frame cells with zero; it does not hold
    # the last value. A full-frame average would dilute counts according to FPS.
    # For these fixed positive-population captures, AliveEnemies>0 identifies the
    # shared sampling frame. Do NOT independently discard shadow/RT zero values.
    # This is a mean at sampled coordinator updates, NOT a per-frame/time-weighted
    # population estimate. It cannot establish layouts or transitions through zero.
    if ([string]$Entry.EnemyCount -eq "0" -and [string]$Entry.AliveAtEnd -eq "0") {
        $Result.AliveEnemies = 0.0
        $Result.ShadowCasters = 0.0
        $Result.RayTracingVisible = 0.0
        $Result.ConsumerSamplingBasis = "EmptySceneConfirmedByManifest"
        return [PSCustomObject]$Result
    }

    $AliveValues = @(Get-NumericValues $Rows "fpstrueSignificance/AliveEnemies")
    if ($AliveValues.Count -ne $Rows.Count -or $AliveValues.Count -eq 0 -or $null -eq $AliveValues[0]) {
        return [PSCustomObject]$Result
    }
    $SampleRows = @(for ($Index = 0; $Index -lt $Rows.Count; ++$Index) {
        if ($AliveValues[$Index] -gt 0) { $Rows[$Index] }
    })
    if ($SampleRows.Count -eq 0) { return [PSCustomObject]$Result }

    $Result.ConsumerSampleFrames = $SampleRows.Count
    $Result.ConsumerSamplingBasis = "CoordinatorAlivePositiveMask"
    foreach ($Metric in @("AliveEnemies", "ShadowCasters", "RayTracingVisible")) {
        $Result[$Metric] = Round-Nullable (Get-Average @(Get-NumericValues $SampleRows "fpstrueSignificance/$Metric")) 1
    }
    return [PSCustomObject]$Result
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
            return "{0}__{1}" -f $Name, $HeaderCounts[$Name]
        }
        $HeaderCounts[$Name] = 1
        return $Name
    })
    $DataLines = @($Lines | Select-Object -Skip 1 | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and $_ -notmatch '^"?\[HasHeaderRowAtEnd\]'
    })
    return @($DataLines | ConvertFrom-Csv -Header $Headers)
}

$ManifestPath = Join-Path $EvidenceRoot "manifest.csv"
if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "Manifest was not found: $ManifestPath"
}

$Manifest = @(Import-Csv -LiteralPath $ManifestPath)
$RequiredMetrics = [ordered]@{
    GameThreadMs = "GameThreadTime"
    RenderThreadMs = "RenderThreadTime"
    RHIThreadMs = "RHIThreadTime"
    GpuMs = "GPUTime"
    VisibilityWaitMs = "Exclusive/RenderThread/EventWait/Visibility"
    DrawCalls = "RHI/DrawCalls"
}
$OptionalMetrics = [ordered]@{
    SkinnedGeometryBuildBLASMs = "GPU/SkinnedGeometryBuildBLAS"
    RayTracingDynamicGeometryMs = "GPU/RayTracingDynamicGeometry"
    ShadowDepthsMs = "GPU/ShadowDepths"
    ShadowProjectionMs = "GPU/ShadowProjection"
    ShadowDrawCalls = "DrawCall/ShadowDepths"
    AliveEnemies = "fpstrueSignificance/AliveEnemies"
    ShadowCasters = "fpstrueSignificance/ShadowCasters"
    RayTracingVisible = "fpstrueSignificance/RayTracingVisible"
}
$CountMetrics = @("DrawCalls", "ShadowDrawCalls", "AliveEnemies", "ShadowCasters", "RayTracingVisible")
$ConsumerMetrics = @("AliveEnemies", "ShadowCasters", "RayTracingVisible")
$RunSummary = @()
foreach ($Entry in $Manifest) {
    if ($Entry.Valid -ne "True") {
        continue
    }

    $ImportedRows = @(Import-CsvWithUniqueHeaders -Path $Entry.Csv)
    if ($ImportedRows.Count -eq 0 -or $null -eq $ImportedRows[0].PSObject.Properties["FrameTime"]) {
        throw "$($Entry.Csv): required column 'FrameTime' is missing or has no samples."
    }
    # UE CSV appends a repeated header and metadata row; neither is a frame sample.
    $DataRows = @($ImportedRows | Where-Object {
        $FrameText = [string]$_.FrameTime
        $FrameText -ne "FrameTime" -and $FrameText -ne "[HasHeaderRowAtEnd]"
    })
    $AllFrameValues = @(Get-NumericValues $DataRows "FrameTime" -Required -Context $Entry.Csv)
    $RawRows = New-Object System.Collections.Generic.List[object]
    $RawFrameValues = New-Object System.Collections.Generic.List[double]
    for ($Index = 0; $Index -lt $DataRows.Count; ++$Index) {
        if ($AllFrameValues[$Index] -gt 0.0) {
            $RawRows.Add($DataRows[$Index])
            $RawFrameValues.Add($AllFrameValues[$Index])
        }
    }
    if ($RawRows.Count -eq 0) {
        throw "No valid frame samples were found in $($Entry.Csv)."
    }
    # Validate required metrics before trimming so bad raw measurements cannot be hidden by the crop.
    foreach ($Column in $RequiredMetrics.Values) {
        $null = @(Get-NumericValues $RawRows.ToArray() $Column -Required -Context $Entry.Csv)
    }

    $RawDurationSeconds = ($RawFrameValues | Measure-Object -Sum).Sum / 1000.0
    $WindowEndSeconds = $RawDurationSeconds - $EndTrimSeconds
    if ($StartTrimSeconds -ge $WindowEndSeconds) {
        throw "$($Entry.RunId): trimming removes the entire capture ($RawDurationSeconds seconds)."
    }
    $SelectedRows = New-Object System.Collections.Generic.List[object]
    $SelectedFrameValues = New-Object System.Collections.Generic.List[double]
    $ElapsedSeconds = 0.0
    $UsedStartSeconds = $null
    $UsedEndSeconds = $null
    for ($Index = 0; $Index -lt $RawRows.Count; ++$Index) {
        $FrameStartSeconds = $ElapsedSeconds
        $ElapsedSeconds += $RawFrameValues[$Index] / 1000.0
        # Keep only whole frames contained in the cumulative-FrameTime window (not Trace timestamps).
        if ($FrameStartSeconds + 1e-9 -ge $StartTrimSeconds -and $ElapsedSeconds -le $WindowEndSeconds + 1e-9) {
            if ($null -eq $UsedStartSeconds) { $UsedStartSeconds = $FrameStartSeconds }
            $UsedEndSeconds = $ElapsedSeconds
            $SelectedRows.Add($RawRows[$Index])
            $SelectedFrameValues.Add($RawFrameValues[$Index])
        }
    }
    $Rows = $SelectedRows.ToArray()
    $Frame = $SelectedFrameValues.ToArray()
    if ($Frame.Count -eq 0) { throw "$($Entry.RunId): no complete frames remain after trimming." }
    $FrameAverage = Get-Average $Frame

    $RunRow = [ordered]@{
        RunId = $Entry.RunId
        Variant = $Entry.Variant
        Run = [int]$Entry.Run
        EnemyCount = [int]$Entry.EnemyCount
        Samples = $Frame.Count
        RawSamples = $RawRows.Count
        UsedSamples = $Frame.Count
        RawDurationSeconds = [Math]::Round($RawDurationSeconds, 6)
        UsedDurationSeconds = [Math]::Round(($Frame | Measure-Object -Sum).Sum / 1000.0, 6)
        StartTrimSeconds = $StartTrimSeconds
        EndTrimSeconds = $EndTrimSeconds
        UsedStartSeconds = [Math]::Round($UsedStartSeconds, 6)
        UsedEndSeconds = [Math]::Round($UsedEndSeconds, 6)
        AverageFps = [Math]::Round(1000.0 / $FrameAverage, 2)
        FrameAverageMs = [Math]::Round($FrameAverage, 3)
        FrameP95Ms = [Math]::Round((Get-Percentile $Frame 0.95), 3)
        FrameP99Ms = [Math]::Round((Get-Percentile $Frame 0.99), 3)
    }
    foreach ($Metric in $RequiredMetrics.Keys) {
        $Digits = if ($CountMetrics -contains $Metric) { 1 } else { 3 }
        $RunRow[$Metric] = Round-Nullable (Get-Average @(Get-NumericValues $Rows $RequiredMetrics[$Metric] -Required -Context $Entry.Csv)) $Digits
    }
    $MissingOptional = New-Object System.Collections.Generic.List[string]
    $ConsumerSummary = Get-ConsumerSampleSummary $Rows $Entry
    foreach ($Metric in $OptionalMetrics.Keys) {
        $Column = $OptionalMetrics[$Metric]
        if ($ConsumerMetrics -contains $Metric) {
            # ActorCount includes different object populations and is not a valid
            # sampling mask for sparse coordinator counters. Missing stays N/A.
            $RunRow[$Metric] = $ConsumerSummary.$Metric
        } else {
            $Digits = if ($CountMetrics -contains $Metric) { 1 } else { 3 }
            $RunRow[$Metric] = Round-Nullable (Get-Average @(Get-NumericValues $Rows $Column)) $Digits
        }
        if ($null -eq $RunRow[$Metric]) { $MissingOptional.Add($Metric) }
    }
    $RunRow["ConsumerSampleFrames"] = $ConsumerSummary.ConsumerSampleFrames
    $RunRow["ConsumerSamplingBasis"] = $ConsumerSummary.ConsumerSamplingBasis
    $RunRow["MissingOptionalMetrics"] = $MissingOptional -join ";"
    $RunSummary += [PSCustomObject]$RunRow
}

if ($RunSummary.Count -eq 0) {
    throw "The manifest does not contain any valid runs."
}

$VariantOrder = @("Baseline", "HardwareQueries", "HZBOcclusion", "BufferedQueries2", "EnemyRayTracingOff", "EnemyShadowsOff", "OcclusionQueriesOn", "OcclusionQueriesOff")
$GroupSummary = @()
foreach ($Count in @($RunSummary.EnemyCount | Sort-Object -Unique)) {
    foreach ($Variant in $VariantOrder) {
        $Rows = @($RunSummary | Where-Object { $_.EnemyCount -eq $Count -and $_.Variant -eq $Variant })
        if ($Rows.Count -eq 0) {
            continue
        }

        $GroupRow = [ordered]@{
            EnemyCount = $Count
            Variant = $Variant
            Runs = $Rows.Count
            RawSamples = ($Rows.RawSamples | Measure-Object -Sum).Sum
            UsedSamples = ($Rows.UsedSamples | Measure-Object -Sum).Sum
            RawDurationSeconds = [Math]::Round(($Rows.RawDurationSeconds | Measure-Object -Sum).Sum, 6)
            UsedDurationSeconds = [Math]::Round(($Rows.UsedDurationSeconds | Measure-Object -Sum).Sum, 6)
            StartTrimSeconds = $StartTrimSeconds
            EndTrimSeconds = $EndTrimSeconds
            AverageFps = [Math]::Round((Get-Average @($Rows.AverageFps)), 2)
            FrameAverageMs = [Math]::Round((Get-Average @($Rows.FrameAverageMs)), 3)
            FrameRunSpreadMs = [Math]::Round((Get-Spread @($Rows.FrameAverageMs)), 3)
            FrameP95Ms = [Math]::Round((Get-Average @($Rows.FrameP95Ms)), 3)
            FrameP99Ms = [Math]::Round((Get-Average @($Rows.FrameP99Ms)), 3)
            GameThreadMs = [Math]::Round((Get-Average @($Rows.GameThreadMs)), 3)
            RenderThreadMs = [Math]::Round((Get-Average @($Rows.RenderThreadMs)), 3)
            RHIThreadMs = [Math]::Round((Get-Average @($Rows.RHIThreadMs)), 3)
            GpuMs = [Math]::Round((Get-Average @($Rows.GpuMs)), 3)
            VisibilityWaitMs = [Math]::Round((Get-Average @($Rows.VisibilityWaitMs)), 3)
            DrawCalls = [Math]::Round((Get-Average @($Rows.DrawCalls)), 1)
            ConsumerSampleFrames = ($Rows.ConsumerSampleFrames | Measure-Object -Sum).Sum
            ConsumerSamplingBasis = (@($Rows.ConsumerSamplingBasis | Sort-Object -Unique) -join ";")
        }
        $MissingOptional = New-Object System.Collections.Generic.List[string]
        foreach ($Metric in $OptionalMetrics.Keys) {
            $Digits = if ($CountMetrics -contains $Metric) { 1 } else { 3 }
            $GroupRow[$Metric] = Round-Nullable (Get-GroupAverage $Rows $Metric) $Digits
            if ($null -eq $GroupRow[$Metric]) { $MissingOptional.Add($Metric) }
        }
        $GroupRow["MissingOptionalMetrics"] = $MissingOptional -join ";"
        $GroupSummary += [PSCustomObject]$GroupRow
    }
}

$Comparison = @()
foreach ($Count in @($GroupSummary.EnemyCount | Sort-Object -Unique)) {
    $Baseline = @($GroupSummary | Where-Object { $_.EnemyCount -eq $Count -and $_.Variant -eq "Baseline" }) |
        Select-Object -First 1
    if (-not $Baseline) {
        $Baseline = @($GroupSummary | Where-Object { $_.EnemyCount -eq $Count -and $_.Variant -eq "HardwareQueries" }) |
            Select-Object -First 1
    }
    if (-not $Baseline) {
        $Baseline = @($GroupSummary | Where-Object { $_.EnemyCount -eq $Count -and $_.Variant -eq "OcclusionQueriesOn" }) |
            Select-Object -First 1
    }
    if (-not $Baseline) {
        continue
    }

    foreach ($Disabled in @($GroupSummary | Where-Object {
        $_.EnemyCount -eq $Count -and $_.Variant -ne $Baseline.Variant
    })) {
        $FrameSaved = $Baseline.FrameAverageMs - $Disabled.FrameAverageMs
        $Comparison += [PSCustomObject]@{
            EnemyCount = $Count
            BaselineVariant = $Baseline.Variant
            DisabledFeature = $Disabled.Variant
            FpsChangeWhenDisabled = [Math]::Round($Disabled.AverageFps - $Baseline.AverageFps, 2)
            FrameMsSavedWhenDisabled = [Math]::Round($FrameSaved, 3)
            FramePercentSaved = if ($Baseline.FrameAverageMs -gt 0) {
                [Math]::Round(100.0 * $FrameSaved / $Baseline.FrameAverageMs, 2)
            } else { 0.0 }
            P95MsSavedWhenDisabled = [Math]::Round($Baseline.FrameP95Ms - $Disabled.FrameP95Ms, 3)
            RenderThreadMsSaved = [Math]::Round($Baseline.RenderThreadMs - $Disabled.RenderThreadMs, 3)
            RHIThreadMsSaved = [Math]::Round($Baseline.RHIThreadMs - $Disabled.RHIThreadMs, 3)
            GpuMsSaved = [Math]::Round($Baseline.GpuMs - $Disabled.GpuMs, 3)
            VisibilityWaitMsSaved = [Math]::Round($Baseline.VisibilityWaitMs - $Disabled.VisibilityWaitMs, 3)
            BlasMsSaved = Get-NullableDifference $Baseline.SkinnedGeometryBuildBLASMs $Disabled.SkinnedGeometryBuildBLASMs
            ShadowDepthsMsSaved = Get-NullableDifference $Baseline.ShadowDepthsMs $Disabled.ShadowDepthsMs
        }
    }
}

$RunSummaryPath = Join-Path $EvidenceRoot "run_summary.csv"
$GroupSummaryPath = Join-Path $EvidenceRoot "group_summary.csv"
$ComparisonPath = Join-Path $EvidenceRoot "comparison_vs_baseline.csv"
$MarkdownPath = Join-Path $EvidenceRoot "summary.md"
$RunSummary | Export-Csv -LiteralPath $RunSummaryPath -NoTypeInformation -Encoding UTF8
$GroupSummary | Export-Csv -LiteralPath $GroupSummaryPath -NoTypeInformation -Encoding UTF8
$Comparison | Export-Csv -LiteralPath $ComparisonPath -NoTypeInformation -Encoding UTF8

$Lines = New-Object System.Collections.Generic.List[string]
$Lines.Add("# 渲染成本自动测试")
$Lines.Add("")
$Lines.Add("每个组合独立启动进程，固定地图、分辨率、随机种子、预热和采样时长；同组重复运行后取平均。")
$Lines.Add("")
$Lines.Add("首尾裁剪：开头 $StartTrimSeconds 秒，末尾 $EndTrimSeconds 秒。以正值 FrameTime 从首个 CSV 样本累计得到近似时间轴，只保留完整落在窗口内的帧；这不等同于 Trace 时间戳。组级原始/使用帧数与时长为各轮之和，性能指标仍为各轮等权平均。")
$Lines.Add("")
$Lines.Add("| 运行 | 原始帧 | 使用帧 | 原始时长 | 使用时长 | 实际保留累计窗口 |")
$Lines.Add("| :--- | ---: | ---: | ---: | ---: | :--- |")
foreach ($Row in $RunSummary) {
    $Lines.Add("| $($Row.RunId) | $($Row.RawSamples) | $($Row.UsedSamples) | $($Row.RawDurationSeconds) s | $($Row.UsedDurationSeconds) s | $($Row.UsedStartSeconds)–$($Row.UsedEndSeconds) s |")
}
$Lines.Add("")
$Lines.Add("## 各实验组汇总")
$Lines.Add("")
$Lines.Add("| 敌人数 | 实验组 | 重复 | FPS | 帧平均 | P95 | GT | RT | RHI | GPU | 协调器采样时点RT人数 | 协调器采样时点投影人数 |")
$Lines.Add("| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($Row in @($GroupSummary | Sort-Object EnemyCount, Variant)) {
    $Lines.Add("| $($Row.EnemyCount) | $($Row.Variant) | $($Row.Runs) | $($Row.AverageFps) | $($Row.FrameAverageMs) ms | $($Row.FrameP95Ms) ms | $($Row.GameThreadMs) ms | $($Row.RenderThreadMs) ms | $($Row.RHIThreadMs) ms | $($Row.GpuMs) ms | $(Format-Nullable $Row.RayTracingVisible) | $(Format-Nullable $Row.ShadowCasters) |")
}
$Lines.Add("")
$Lines.Add("## 单变量关闭后的变化")
$Lines.Add("")
$Lines.Add("正数表示本批相对对照组更快，属于开关净变化，不等同于该功能的独占耗时。对照组名称记录在 comparison_vs_baseline.csv 的 BaselineVariant。")
$Lines.Add("")
$Lines.Add("| 敌人数 | 关闭项 | FPS变化 | 帧节省 | P95节省 | RT节省 | RHI节省 | GPU节省 | BLAS节省 | 阴影深度节省 |")
$Lines.Add("| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($Row in @($Comparison | Sort-Object EnemyCount, DisabledFeature)) {
    $Lines.Add("| $($Row.EnemyCount) | $($Row.DisabledFeature) | $($Row.FpsChangeWhenDisabled) | $($Row.FrameMsSavedWhenDisabled) ms | $($Row.P95MsSavedWhenDisabled) ms | $($Row.RenderThreadMsSaved) ms | $($Row.RHIThreadMsSaved) ms | $($Row.GpuMsSaved) ms | $(Format-Nullable $Row.BlasMsSaved ' ms') | $(Format-Nullable $Row.ShadowDepthsMsSaved ' ms') |")
}
$Lines.Add("")
$Lines.Add("## 可选指标完整性")
$Lines.Add("")
$Lines.Add("N/A 表示列缺失或样本不完整/非有限；CSV 以空字段表示 null，MissingOptionalMetrics 记录指标名称。任何一轮缺失会使对应组指标及其对照差值为 N/A，不以零补齐。")
$Lines.Add("")
$Lines.Add("消费者为协调器采样时点人数均值：这三个 CSV Set 计数只在协调器更新时写入，未写入帧由 UE 补零。正人数固定场景统一用 AliveEnemies>0 的帧作为采样 mask，再对同一帧的存活/投影/RT人数求平均，保留采样帧中投影或RT的真实零值。该结果不是逐帧或按时间加权的人数，也不能证明零人数过渡和逐帧布局。缺少协调器列不回退为口径不同的 ActorCount。")
$Lines.Add("")
$Lines.Add("ConsumerSampleFrames 为实际采用的协调器采样帧数，组级为各轮之和，组级人数均值仍按各轮等权。EmptySceneConfirmedByManifest 表示请求0敌人且有效manifest的结束存活数为0，人数0来自该确认，非CSV采样，采样帧数记0。")
$Lines.Add("")
$Lines.Add("| 运行 | 协调器采样帧数 | 人数统计依据 |")
$Lines.Add("| :--- | ---: | :--- |")
foreach ($Row in $RunSummary) {
    $Lines.Add("| $($Row.RunId) | $($Row.ConsumerSampleFrames) | $($Row.ConsumerSamplingBasis) |")
}
$Lines.Add("")
foreach ($Row in @($RunSummary | Where-Object { $_.MissingOptionalMetrics })) {
    $Lines.Add("- $($Row.RunId)：$($Row.MissingOptionalMetrics)")
}
foreach ($Row in @($GroupSummary | Where-Object { $_.MissingOptionalMetrics })) {
    $Lines.Add("- 组 N$($Row.EnemyCount)/$($Row.Variant)：$($Row.MissingOptionalMetrics)")
}
$Lines.Add("")
$Lines.Add("## 数据解释边界")
$Lines.Add("")
$Lines.Add("- VisibilityWait 是 RenderThread 可见性等待的稳定代理，不等同于精确的 WaitForGatherDynamicMeshElements 事件。")
$Lines.Add("- SkinnedGeometryBuildBLAS 是 CSV 中的 GPU 统计，不等同于 Insights 里 RHIThread 的 CPU BuildAccelerationStructure_BottomLevel。")
$Lines.Add("- 只有 manifest.csv 中 Valid=True 的独立运行才会进入汇总。")
$Lines | Set-Content -LiteralPath $MarkdownPath -Encoding UTF8

$GroupSummary | Format-Table EnemyCount, Variant, Runs, AverageFps, FrameAverageMs, FrameP95Ms, GameThreadMs, RenderThreadMs, RHIThreadMs, GpuMs -AutoSize
$Comparison | Format-Table -AutoSize
Write-Output "Run summary: $RunSummaryPath"
Write-Output "Group summary: $GroupSummaryPath"
Write-Output "Comparison: $ComparisonPath"
Write-Output "Markdown: $MarkdownPath"
