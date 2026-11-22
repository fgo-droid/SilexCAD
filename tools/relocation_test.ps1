# RelocationTest (sprint "Fiabilité, Portabilité & Conformité", criterion 1).
#
# Copies ONLY the self-contained bin\<config> folder to an isolated directory, then from there:
#   1. checks that silex.exe holds no path of the source or build trees;
#   2. runs `silex.exe --doctor` (exit code 0 required);
#   3. runs the resource-dependent tests (silex_tests, silex_ipc_tests) with SILEX_PORTABLE_ONLY=1,
#      which forbids any fallback to the source tree;
#   4. runs `python mcp_server.py --check` from the copy when Python is available (the bridge must
#      find silex.exe next to itself).
#
#   powershell -File tools\relocation_test.ps1 -BinDir build\bin\Release [-Target C:\Temp\Silex_Portable_Test]
# Compatible with Windows PowerShell 5.1 (ctest) and PowerShell 7.
param(
    [Parameter(Mandatory = $true)][string]$BinDir,
    [string]$Target = $(if ($env:SILEX_RELOCATION_DIR) { $env:SILEX_RELOCATION_DIR } else { "C:\Temp\Silex_Portable_Test" }),
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$failures = New-Object System.Collections.Generic.List[string]
$logDir = Join-Path ([System.IO.Path]::GetTempPath()) "silex_relocation_logs"

# Runs a native program with its output in files (never through a PowerShell pipe: in Windows
# PowerShell 5.1 a line on stderr would become a terminating error, and a child process inheriting
# the pipe of ctest would keep ctest waiting). Returns @{ Code; Out }; kills it after $TimeoutSec.
function Invoke-Native([string]$File, [string[]]$Arguments, [int]$TimeoutSec = 300) {
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $name = [System.IO.Path]::GetFileNameWithoutExtension($File)
    $out = Join-Path $logDir "$name.out.txt"
    $err = Join-Path $logDir "$name.err.txt"
    $p = Start-Process -FilePath $File -ArgumentList $Arguments -NoNewWindow -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $p.Handle  # keeps the process handle: without it ExitCode stays empty
    $code = $null
    if ($p.WaitForExit($TimeoutSec * 1000)) { $p.WaitForExit(); $code = $p.ExitCode }
    else {
        try { $p.Kill() } catch { }
        $code = "timeout after $TimeoutSec s"
    }
    $text = ""
    foreach ($f in $out, $err) { if (Test-Path $f) { $text += (Get-Content $f -Raw) + "`n" } }
    return @{ Code = $code; Out = $text }
}

# Processes started from the relocated folder (a headless silex left by a failed test...)
function Stop-Leftovers([string]$Folder) {
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Folder, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
function Report([string]$level, [string]$text) {
    Write-Host ("[{0}] {1}" -f $level, $text)
    if ($level -eq "FAIL") { $failures.Add($text) }
}

$BinDir = (Resolve-Path $BinDir).Path
$sourceRoot = Split-Path -Parent $PSScriptRoot
Write-Host "RelocationTest: $BinDir -> $Target"

# ---- 0. isolated copy ----------------------------------------------------------------------
if (Test-Path $Target) { Stop-Leftovers $Target; Remove-Item $Target -Recurse -Force }
New-Item -ItemType Directory -Force $Target | Out-Null
Copy-Item -Path (Join-Path $BinDir "*") -Destination $Target -Recurse -Force
$exe = Join-Path $Target "silex.exe"
if (-not (Test-Path $exe)) { throw "silex.exe missing from $BinDir" }

# the copy must not see any developer override
foreach ($v in "SILEX_RESOURCE_DIR", "SILEX_EXE", "SILEX_RECOVERY_DIR", "SILEX_PIPE") {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:SILEX_PORTABLE_ONLY = "1"
$env:SILEX_NO_RECOVERY = "1"
Push-Location $Target   # current directory = the copy (never the build tree)
try {
    # ---- 1. no compiled-in path ------------------------------------------------------------
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [System.Text.Encoding]::Unicode.GetString($bytes)
    $leaks = @()
    foreach ($p in @($sourceRoot, $sourceRoot.Replace('\', '/'), $BinDir, $BinDir.Replace('\', '/'))) {
        if ($ascii.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $unicode.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $leaks += $p }
    }
    if ($leaks.Count -eq 0) { Report "OK" "silex.exe contains no path of the source / build trees" }
    else { Report "FAIL" ("silex.exe contains build paths: " + ($leaks -join ", ")) }

    # ---- 2. installation diagnostic --------------------------------------------------------
    $r = Invoke-Native $exe @("--doctor") 120
    $doctor = $r.Out
    $code = $r.Code
    Write-Host $doctor
    if ($code -eq 0) { Report "OK" "silex --doctor: exit code 0" } else { Report "FAIL" "silex --doctor: exit code $code" }
    if ($doctor -match [regex]::Escape((Join-Path $Target "resources"))) {
        Report "OK" "resources resolved next to the relocated executable"
    } else { Report "FAIL" "the doctor did not use $Target\resources" }

    # ---- 3. targeted tests -----------------------------------------------------------------
    $unit = "Reliability.*:Finitions.*:V05Components.*:V05Repair.*:V05Recorder.*:Examples.*:V04Lot3.*:" +
            "V03.HlrProjectionOfReferenceBracket:V03.ProjectedEdgeFollowsTheModel:V03.SchemaV03RoundTripAndOlderFiles"
    $r = Invoke-Native (Join-Path $Target "silex_tests.exe") @("--gtest_filter=$unit", "--gtest_brief=1") 400
    $out = $r.Out
    $code = $r.Code
    Write-Host $out
    $ran = [regex]::Match($out, "\[==========\] (\d+) tests? from").Groups[1].Value
    if ($ran -eq "") { $ran = "0" }
    if ($code -eq 0 -and [int]$ran -ge 10) { Report "OK" "silex_tests: $ran resource-dependent tests passed from the copy" }
    else { Report "FAIL" "silex_tests from the copy: exit code $code, $ran test(s)" }

    $ipc = "Ipc.McpDocumentCycleHeadless:Ipc.V05MakersAndTutorHeadless:Ipc.AutosaveCrashRecoveryGui"
    $r = Invoke-Native (Join-Path $Target "silex_ipc_tests.exe") @("--gtest_filter=$ipc", "--gtest_brief=1") 300
    $out = $r.Out
    $code = $r.Code
    Write-Host $out
    $ran = [regex]::Match($out, "\[==========\] (\d+) tests? from").Groups[1].Value
    if ($ran -eq "") { $ran = "0" }
    if ($code -eq 0 -and [int]$ran -eq 3) { Report "OK" "silex_ipc_tests: $ran end-to-end tests passed from the copy" }
    else { Report "FAIL" "silex_ipc_tests from the copy: exit code $code, $ran test(s)" }

    # ---- 4. MCP bridge self-test -------------------------------------------------------------
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python -and (Test-Path (Join-Path $Target "mcp_server.py"))) {
        $r = Invoke-Native $python.Source @((Join-Path $Target "mcp_server.py"), "--check") 120
        $out = $r.Out
        Write-Host $out
        if ($out -match "Silex binary: .*silex\.exe \(next to mcp_server\.py\)") {
            Report "OK" "mcp_server.py finds silex.exe next to itself"
        } else { Report "FAIL" "mcp_server.py --check did not find the relocated silex.exe" }
        if ($r.Code -ne 0) {
            Report "WARN" "mcp_server.py --check reported problems (missing Python packages are not part of this test)"
        }
    } else {
        Report "WARN" "Python not found: MCP bridge self-test skipped"
    }
}
finally {
    Stop-Leftovers $Target
    Pop-Location
    Remove-Item Env:SILEX_PORTABLE_ONLY -ErrorAction SilentlyContinue
    Remove-Item Env:SILEX_NO_RECOVERY -ErrorAction SilentlyContinue
}

if ($failures.Count -eq 0) {
    if (-not $Keep) { Remove-Item $Target -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host "RelocationTest: PASSED"
    exit 0
}
Write-Host ("RelocationTest: FAILED ({0}) - the copy is kept in {1}" -f $failures.Count, $Target)
exit 1
