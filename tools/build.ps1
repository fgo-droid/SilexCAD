# Configure + build Silex from any PowerShell (loads the MSVC x64 environment first).
#   .\tools\build.ps1                # configure (if needed) and build Release
#   .\tools\build.ps1 -Test          # ... and run ctest
#   .\tools\build.ps1 -Offline       # never run vcpkg: use the dependencies already in vcpkg_installed\
#                                    # (restore from a backup => only Silex is compiled, a few minutes)
param([switch]$Test, [switch]$Offline, [string]$Config = "Release")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $env:VCPKG_ROOT) {
    # backup layout: <dir>\Silex and <dir>\vcpkg side by side
    $sibling = Join-Path (Split-Path -Parent $root) "vcpkg"
    $env:VCPKG_ROOT = if (Test-Path (Join-Path $sibling "scripts\buildsystems\vcpkg.cmake")) { $sibling } else { "C:\Projects\vcpkg" }
}
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
    throw "vcpkg not found in $env:VCPKG_ROOT (set VCPKG_ROOT)"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ workload was not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$steps = @()
if (-not (Test-Path (Join-Path $root "build\build-$Config.ninja"))) {
    $configure = "cmake --fresh --preset windows"
    if ($Offline) { $configure += " -DVCPKG_MANIFEST_INSTALL=OFF" }
    $steps += $configure
}
$steps += "cmake --build --preset $($Config.ToLower()) -- -k 0"
if ($Test) { $steps += "ctest --preset release --output-on-failure" }

# vcvars64 points VCPKG_ROOT at Visual Studio's bundled vcpkg: restore ours afterwards,
# otherwise every dependency would be rebuilt from scratch in another tree.
$installer = Split-Path -Parent $vswhere  # vcvars64 needs vswhere.exe on PATH
$cmd = "set `"PATH=$installer;%PATH%`" && `"$vcvars`" >nul && set VCPKG_ROOT=$env:VCPKG_ROOT&& cd /d `"$root`" && " + ($steps -join " && ")
cmd /c $cmd
exit $LASTEXITCODE
