<#
Runs the original single-player map/UI, not the multiplayer smoke fixture.
The development driver uses the original LevelScript AnyKey start input and
clicks the actual UMG restart delegate. Victory
uses a transient one-enemy, six-second configuration; Defeat uses real AI damage
and normal CMC approach. No direct HP/damage, equipment or position writes.
Pictures require a separate human review. This is not a performance benchmark.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:\ueprojrct\fpstrue_safe2',
    [string]$EngineRoot = 'E:\program\ue554\UE_5.5',
    [ValidateSet('Both','Victory','Defeat')][string]$Case = 'Both',
    [ValidateRange(30,300)][int]$TimeoutSeconds = 240,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $ProjectRoot 'fpstrue.uproject'
$module = Join-Path $ProjectRoot 'Binaries\Win64\UnrealEditor-fpstrue.dll'
$root = Join-Path $ProjectRoot ('Saved\Networking\StandaloneLoop_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$cases = if ($Case -eq 'Both') { @('Victory','Defeat') } else { @($Case) }
$map = '/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked'
if ($DryRun) { [pscustomobject]@{Output=$root;Cases=$cases;Map=$map;Rendered=$true;OriginalMenu=$true;TransientVictorySeconds=6;TransientEnemyCount=1} | ConvertTo-Json; return }
foreach ($path in @($editor,$project,$module)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file missing: $path" } }
$hash = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
$results = [Collections.Generic.List[object]]::new()
$failure = $null
function Read-VisualLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $s = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite,Delete')
    $r = [IO.StreamReader]::new($s)
    try { $r.ReadToEnd() } finally { $r.Dispose(); $s.Dispose() }
}
try {
    foreach ($scenario in $cases) {
        $dir = Join-Path $root $scenario
        $visual = Join-Path $dir 'Visual'
        New-Item -ItemType Directory -Path $visual -Force | Out-Null
        $log = Join-Path $dir 'Game.log'
        $arguments = @(('"' + $project + '"'),$map,'-game','-Multiprocess','-dx12','-Windowed','-RenderOffscreen','-ResX=1280','-ResY=720','-ForceRes',
            '-NoLiveCoding','-NoP4','-NoSplash','-NoSteam','-NoSound','-NoVSync','-noraytracing',('-FpsStandaloneVisualCase=' + $scenario),
            ('-FpsStandaloneCaptureDir="' + $visual + '"'),('-UserDir="' + (Join-Path $dir 'User') + '"'),('-abslog="' + $log + '"'),
            '-ddc=fpstrueEditorTestDDC','-LocalDataCachePath="C:\UEBuild\FPS\RARAnimationCompletion_20261016\DDC"',
            '-ExecCmds="t.MaxFPS 30,sg.ViewDistanceQuality 1,sg.AntiAliasingQuality 1,sg.ShadowQuality 1,sg.GlobalIlluminationQuality 1,sg.ReflectionQuality 1,sg.PostProcessQuality 1,sg.TextureQuality 1,sg.EffectsQuality 1,sg.FoliageQuality 1,r.DynamicGlobalIlluminationMethod 0,r.ReflectionMethod 0,r.MotionBlurQuality 0,r.Streaming.PoolSize 768,r.Shadow.Virtual.MaxPhysicalPages 1024,fp.AuditStandaloneLoop"')
        $process = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
        $started = $process.StartTime.ToUniversalTime()
        $passed = $false; $reason = $null
        try {
            $clock = [Diagnostics.Stopwatch]::StartNew()
            while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
                $text = Read-VisualLog $log
                if ($text -match 'FPS_STANDALONE_VISUAL FAIL|Fatal error:|Assertion failed:|Unhandled Exception:|Blueprint Runtime Error|LogScript: Warning: Accessed None|Out of video memory') { throw 'Runtime or real UI/outcome driver failure; inspect the original log.' }
                if ($text -match 'Virtual Shadow Map Page Pool overflow') { throw 'Real virtual-shadow page allocation overflow; the visual profile is invalid and warnings must not be hidden.' }
                $process.Refresh()
                if ($process.HasExited) { throw ('Game exited before UI acceptance: ' + $process.ExitCode) }
                if ($text -match ('FPS_STANDALONE_VISUAL PASS Case=' + $scenario + '\b')) { $passed = $true; break }
                Start-Sleep -Milliseconds 300
            }
            if (-not $passed) { throw 'Original UI/outcome/restart sequence timed out.' }
            foreach ($file in @('MainMenu','Playing',($scenario + 'UI'),'RestartMenu','RestartPlaying')) {
                $png = Join-Path $visual ($file + '.png')
                if (-not (Test-Path -LiteralPath $png -PathType Leaf) -or (Get-Item -LiteralPath $png).Length -le 1024) { throw ('Missing actual rendered screenshot: ' + $file) }
                $bytes = [IO.File]::ReadAllBytes($png)
                if ([BitConverter]::ToString($bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A' -or
                    ($bytes[16]*16777216 + $bytes[17]*65536 + $bytes[18]*256 + $bytes[19]) -ne 1280 -or
                    ($bytes[20]*16777216 + $bytes[21]*65536 + $bytes[22]*256 + $bytes[23]) -ne 720) { throw ('Invalid 1280x720 PNG: ' + $file) }
            }
        } catch { $reason = $_.Exception.Message; $passed = $false }
        finally {
            $current = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
            if ($current -and $current.StartTime.ToUniversalTime() -eq $started -and [string]::Equals($current.Path,$editor,[StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $current.Id -Force }
            $results.Add([pscustomobject]@{Case=$scenario;Passed=$passed;Failure=$reason;Log=$log;Images=@(Get-ChildItem -LiteralPath $visual -File -Filter '*.png' | ForEach-Object {
                [pscustomobject]@{Path=$_.FullName;Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
            })})
        }
        if (-not $passed) { throw $reason }
    }
    if ((Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash -ne $hash) { throw 'Editor module changed during the real UI sequence.' }
} catch { $failure = $_.Exception.Message }
finally {
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    [pscustomobject]@{Passed=(-not $failure -and $results.Count -eq $cases.Count);Failure=$failure;ModuleSHA256=$hash;Cases=@($results);
        Graphics='Single DX12 offscreen 1280x720 medium functional profile, 30FPS cap, GI/reflections/motion blur off, VSM physical pages 1024 (medium bias/samples unchanged); not default-quality or performance proof.';
        TemporaryConfig=@{VictorySeconds=6;DefeatSeconds=120;Enemies=1;AssetFilesWritten=$false};VisualQualityReviewed=$false;AudioReviewed=$false;
        Scope='Original single-player AnyKey menu input, natural outcome and original UMG button restart. Not packaged, physical keyboard/mouse, network or capacity acceptance.'} |
        ConvertTo-Json -Depth 7 | Out-File -LiteralPath (Join-Path $root 'Report.json') -Encoding utf8
    Write-Output ('STANDALONE_VISUAL_REPORT ' + (Join-Path $root 'Report.json'))
}
if ($failure) { throw $failure }
