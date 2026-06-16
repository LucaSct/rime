# Rime — one-command build for the whole project (C++ engine + Rust tools) on Windows.
# Mirrors scripts/build.sh; the two are kept in step and the Windows path is exercised by
# CI on the Windows runner (Milestone 0.5). Run scripts/setup.ps1 first if a tool is
# missing.
#
# Usage: scripts/build.ps1 [-Preset dev|release] [-NoTests] [-CppOnly] [-RustOnly] [-Clean]
[CmdletBinding()]
param(
    [ValidateSet('dev', 'release')][string]$Preset = 'dev',
    [switch]$NoTests,
    [switch]$CppOnly,
    [switch]$RustOnly,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

# Always operate from the repo root (this script lives in scripts/).
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
$buildType = if ($Preset -eq 'release') { 'RelWithDebInfo' } else { 'Debug' }
function Say($m) { Write-Host "`n== $m ==" -ForegroundColor Cyan }

if ($Clean) {
    Say 'clean'
    Remove-Item -Recurse -Force "build/$Preset", 'tools/target' -ErrorAction SilentlyContinue
}

if (-not $RustOnly) {
    # Locate Conan: prefer one on PATH, else the isolated venv that setup.ps1 creates.
    $conan = if (Get-Command conan -ErrorAction SilentlyContinue) { 'conan' }
    elseif (Test-Path "$HOME/.rime-tools/Scripts/conan.exe") { "$HOME/.rime-tools/Scripts/conan.exe" }
    else { throw 'conan not found — run scripts/setup.ps1 first' }

    Say "C++: conan install ($buildType)"
    & $conan install . -of "build/$Preset" -s build_type=$buildType -s compiler.cppstd=20 --build=missing

    Say "C++: cmake configure ($Preset)"; cmake --preset $Preset
    Say "C++: cmake build ($Preset)"; cmake --build --preset $Preset

    if (-not $NoTests) {
        Say 'C++: ctest'
        ctest --test-dir "build/$Preset" --output-on-failure
    }
}

if (-not $CppOnly) {
    # rust-toolchain.toml lives in tools/, so run cargo from there.
    $cargoArgs = if ($Preset -eq 'release') { @('--release') } else { @() }
    Push-Location tools
    try {
        Say 'Rust: cargo build'; cargo build @cargoArgs
        if (-not $NoTests) { Say 'Rust: cargo test'; cargo test @cargoArgs }
    }
    finally { Pop-Location }
}

Say "done ($Preset)"
