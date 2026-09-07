[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$ObsPluginRoot = "$env:ProgramData/obs-studio/plugins",
    [string]$ETS2Path,
    [string]$ATSPath
)
$ErrorActionPreference = 'Stop'
$bundle = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $bundle 'obs-racing-motion/bin/64bit/obs-racing-motion.dll'))) {
    $bundle = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../dist'))
}
$manifestPath = Join-Path $bundle 'SHA256SUMS.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Build/extract the complete release bundle first.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$bundlePrefix = [IO.Path]::GetFullPath($bundle).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
function File-SHA256([string]$FilePath) {
    # Avoid optional PowerShell module discovery; use the built-in .NET API.
    $stream = [IO.File]::OpenRead($FilePath)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Assert-ManifestFile([string]$FilePath) {
    $full = [IO.Path]::GetFullPath($FilePath)
    if (-not $full.StartsWith($bundlePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid release file path.' }
    $key = $full.Substring($bundlePrefix.Length).Replace('\','/')
    $found = $manifest.PSObject.Properties | Where-Object { $_.Name.Replace('\','/') -eq $key }
    if (-not $found) { throw "Release file is not listed in checksum manifest: $key" }
    if ((File-SHA256 $full) -ne $found.Value) { throw "Checksum mismatch: $key" }
}
foreach ($entry in $manifest.PSObject.Properties) {
    $file = [IO.Path]::GetFullPath((Join-Path $bundle $entry.Name))
    if (-not $file.StartsWith($bundlePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path.' }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing release file: $($entry.Name)" }
    if ((File-SHA256 $file) -ne $entry.Value) { throw "Checksum mismatch: $($entry.Name)" }
}
if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing the plugin.' }
$copies = [Collections.Generic.List[object]]::new()
$pluginSource = Join-Path $bundle 'obs-racing-motion'
$pluginTarget = [IO.Path]::GetFullPath((Join-Path $ObsPluginRoot 'obs-racing-motion'))
foreach ($file in Get-ChildItem -LiteralPath $pluginSource -File -Recurse) {
    Assert-ManifestFile $file.FullName
    $relative = $file.FullName.Substring($pluginSource.Length).TrimStart('\','/')
    $copies.Add(@{Source=$file.FullName;Target=(Join-Path $pluginTarget $relative)})
}
foreach ($game in @(@{Path=$ETS2Path;Exe='eurotrucks2'},@{Path=$ATSPath;Exe='amtrucks'})) {
    if (-not $game.Path) { continue }
    if (Get-Process -Name $game.Exe -ErrorAction SilentlyContinue) { throw "Close $($game.Exe) before installing." }
    $gameRoot = [IO.Path]::GetFullPath($game.Path)
    if (-not (Test-Path -LiteralPath (Join-Path $gameRoot "bin/win_x64/$($game.Exe).exe"))) {
        throw "Game executable not found under $gameRoot/bin/win_x64. Pass the game's root folder."
    }
    $copies.Add(@{Source=(Join-Path $bundle 'truck/racing-motion-scs.dll');Target=(Join-Path $gameRoot 'bin/win_x64/plugins/racing-motion-scs.dll')})
}
foreach ($copy in $copies) { Assert-ManifestFile $copy.Source }
$backupRoot = Join-Path $env:LOCALAPPDATA ('OBSRacingMotion/backups/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$index = 0
foreach ($copy in $copies) {
    if ($PSCmdlet.ShouldProcess($copy.Target,'Install verified plugin file')) {
        if (Test-Path -LiteralPath $copy.Target) {
            New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
            $backup = Join-Path $backupRoot ("$index-" + [IO.Path]::GetFileName($copy.Target))
            Copy-Item -LiteralPath $copy.Target -Destination $backup
        }
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($copy.Target)) -Force | Out-Null
        Copy-Item -LiteralPath $copy.Source -Destination $copy.Target -Force
        if ((File-SHA256 $copy.Source) -ne (File-SHA256 $copy.Target)) {throw "Install verification failed: $($copy.Target)"}
        Write-Output "Installed: $($copy.Target)"
    }
    $index++
}
Write-Output 'Open OBS, add Racing Motion under source Effect Filters, select your game, and try Preview motion.'
