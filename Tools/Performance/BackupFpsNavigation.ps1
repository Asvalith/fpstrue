<# Exact, recoverable copies before the approved 55/192 navigation repair. No UE calls or deletions. #>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'E:/ueprojrct/fpstrue_safe2',
    [string]$BackupRoot = ('C:/UEBuild/FinalAcceptance20261018/FPS/NavAgentRepair_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$BackupRoot = [IO.Path]::GetFullPath($BackupRoot)
if (Test-Path -LiteralPath $BackupRoot) { throw 'A backup must be fresh; never overwrite earlier evidence.' }
$Files = @('Config/DefaultEngine.ini','Config/DefaultGame.ini',
    'Content/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked.umap',
    'Content/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked_BuiltData.uasset',
    'Content/PerformanceCandidates/EnemyUI160/EnemyUI160_Arena.umap',
    'Content/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview.umap')
$ExternalRoots = foreach ($Kind in @('__ExternalActors__','__ExternalObjects__')) {
    foreach ($Map in @('SplineBake_Tracks20260928/Demonstration_Baked','EnemyUI160/EnemyUI160_Arena','EnemyUI160/EnemyUI160_ArenaOverview')) {
        'Content/'+$Kind+'/PerformanceCandidates/'+$Map
    }
}
$ExternalInventory = @($ExternalRoots | ForEach-Object {
    $Path = Join-Path $ProjectRoot $_
    [pscustomobject]@{RelativePath=$_; Exists=(Test-Path -LiteralPath $Path -PathType Container)}
})
foreach ($External in $ExternalInventory) {
    if ($External.Exists) {
        $Files += @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $External.RelativePath) -File -Recurse |
            ForEach-Object {[IO.Path]::GetRelativePath($ProjectRoot,$_.FullName)})
    }
}
New-Item -ItemType Directory -Path $BackupRoot | Out-Null
$Copies = foreach ($Relative in $Files) {
    $Source = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $Relative))
    if (-not $Source.StartsWith($ProjectRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Backup source escaped the named project.' }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        [pscustomobject]@{RelativePath=$Relative; Exists=$false; Source=$Source; Backup=$null; SHA256=$null; Bytes=0}
        continue
    }
    $Destination = Join-Path $BackupRoot $Relative
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Destination)) -Force | Out-Null
    $Hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $Source -Destination $Destination
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $Hash -or
        (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne $Hash) { throw 'Backup/source changed while copying.' }
    [pscustomobject]@{RelativePath=$Relative; Exists=$true; Source=$Source; Backup=$Destination; SHA256=$Hash; Bytes=(Get-Item -LiteralPath $Source).Length}
}
$OriginalFactory = Join-Path $ProjectRoot 'Content/FactoryDistrict/Maps/Demonstration.umap'
$FactoryHash = if (Test-Path -LiteralPath $OriginalFactory) {(Get-FileHash -LiteralPath $OriginalFactory -Algorithm SHA256).Hash} else {$null}
$Manifest = [ordered]@{Created=(Get-Date).ToString('o'); ProjectRoot=$ProjectRoot; Files=@($Copies); ExternalPackages=$ExternalInventory;
    ReadOnlyFactoryMap=$OriginalFactory; ReadOnlyFactorySHA256=$FactoryHash;
    ActiveMap='/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked';
    Scope='DefaultEngine config, current baked map and exact external-package roots, previous UI arena candidates. No original Factory asset changed.'}
[IO.File]::WriteAllText((Join-Path $BackupRoot 'manifest.json'),($Manifest | ConvertTo-Json -Depth 7),[Text.UTF8Encoding]::new($false))
Write-Output ('Navigation backup: '+$BackupRoot)
