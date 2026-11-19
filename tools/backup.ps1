# Copies everything needed to rebuild Silex quickly (about 1.1 GB instead of ~23 GB) to a folder,
# typically a NAS share. Run it again later: only changed files are copied (robocopy mirror).
#
#   .\tools\backup.ps1 -Destination \\NAS\backup\Silex-dev
#
# Resulting layout:
#   <Destination>\Silex             sources + vcpkg_installed (compiled dependencies, no debug symbols)
#   <Destination>\vcpkg             the vcpkg tool itself (scripts, ports, exe), no build trees
#   <Destination>\vcpkg-archives    vcpkg binary cache (plan B to restore dependencies in minutes)
#
# Restore on any PC (Visual Studio 2022 + C++ workload required):
#   robocopy \\NAS\backup\Silex-dev C:\Projects /E
#   C:\Projects\Silex\tools\build.ps1 -Offline -Test
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [switch]$NoArchives  # skip the 585 MB binary cache
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\Projects\vcpkg" }
$archives = Join-Path $env:LOCALAPPDATA "vcpkg\archives"

function Copy-Tree([string]$From, [string]$To, [string[]]$ExcludeDirs = @(), [string[]]$ExcludeFiles = @()) {
    Write-Host "-> $To" -ForegroundColor Cyan
    $rcArgs = @($From, $To, "/MIR", "/R:2", "/W:2", "/NFL", "/NDL", "/NP", "/NJH")
    if ($ExcludeDirs.Count) { $rcArgs += "/XD"; $rcArgs += $ExcludeDirs }
    if ($ExcludeFiles.Count) { $rcArgs += "/XF"; $rcArgs += $ExcludeFiles }
    & robocopy @rcArgs | Select-String -Pattern "Dirs :|Files :|Bytes :" | ForEach-Object { "   " + $_.Line.Trim() }
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $From" }
}

New-Item -ItemType Directory -Force $Destination | Out-Null

# 1. sources (the generated build tree is excluded; vcpkg_installed is copied separately below)
Copy-Tree $root (Join-Path $Destination "Silex") `
    -ExcludeDirs @((Join-Path $root "build"), (Join-Path $root "vcpkg_installed"), (Join-Path $root ".vs"), (Join-Path $root "out")) `
    -ExcludeFiles @("*.user", "build.log", "test.log", "build-deps.log")

# 2. compiled dependencies, without vcpkg's intermediate trees and debug symbols
$vi = Join-Path $root "vcpkg_installed"
Copy-Tree $vi (Join-Path $Destination "Silex\vcpkg_installed") `
    -ExcludeDirs @((Join-Path $vi "vcpkg\blds"), (Join-Path $vi "vcpkg\pkgs")) -ExcludeFiles @("*.pdb")

# 3. the vcpkg tool (toolchain file + ports pinned by vcpkg.json's builtin-baseline)
Copy-Tree $vcpkg (Join-Path $Destination "vcpkg") `
    -ExcludeDirs @((Join-Path $vcpkg "buildtrees"), (Join-Path $vcpkg "downloads"), (Join-Path $vcpkg "packages"), (Join-Path $vcpkg "installed"))

# 4. binary cache (plan B)
if (-not $NoArchives -and (Test-Path $archives)) {
    Copy-Tree $archives (Join-Path $Destination "vcpkg-archives")
}

$size = (Get-ChildItem $Destination -Recurse -File -Force | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Backup complete: {0:N0} MB in {1}" -f $size, $Destination) -ForegroundColor Green

exit 0  # robocopy codes 1-7 mean success
