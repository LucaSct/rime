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

# build.sh gets this from `set -e`; PowerShell has no equivalent. $ErrorActionPreference='Stop'
# above governs CMDLET failures and says nothing about a NATIVE program's exit code, so a failing
# conan/cmake/ctest merely printed its error and the script carried on to the next stage.
#
# That is not a hypothetical tidy-up. It is why CI's "build & test (windows-latest)" was GREEN while
# building no C++ whatsoever: `conan install` failed on an unresolvable libsvtav1/4.2.0 (see the
# export step below), configure/build/ctest each failed in turn against the wreckage, and then
# `cargo test` — which needs none of them — succeeded and became the script's exit status. The job
# "passed" in 66 seconds, and the Windows half of this engine went unbuilt and untested for months.
# Its duration was the only visible symptom.
#
# So: every native command goes through Run(), and a non-zero exit stops the build.
function Run {
    param([Parameter(Mandatory)][string]$Exe,
          [Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        # ASCII only in this interpolated string, deliberately. Windows PowerShell 5.1 (powershell.exe,
        # still the Windows default) reads a .ps1 with no BOM in the machine's ANSI codepage, where
        # the third byte of a UTF-8 em dash decodes to U+201D — a character PowerShell accepts as a
        # STRING DELIMITER. The literal then ends early and the rest of the line is parsed as code:
        # "Unexpected token 'failed' in expression or statement". Comments and single-quoted strings
        # elsewhere in this file are unaffected, which is why only this one had to change.
        throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

if ($Clean) {
    Say 'clean'
    Remove-Item -Recurse -Force "build/$Preset", 'tools/target' -ErrorAction SilentlyContinue
}

if (-not $RustOnly) {
    # Locate Conan: prefer one on PATH, else the isolated venv that setup.ps1 creates.
    $conan = if (Get-Command conan -ErrorAction SilentlyContinue) { 'conan' }
    elseif (Test-Path "$HOME/.rime-tools/Scripts/conan.exe") { "$HOME/.rime-tools/Scripts/conan.exe" }
    else { throw 'conan not found — run scripts/setup.ps1 first' }

    # Our own recipes (libsvtav1/4.2.0) must reach the Conan cache before install can resolve them —
    # Conan Center stops at 2.2.1, so without this `conan install` fails with "Package
    # 'libsvtav1/4.2.0' not resolved". build.sh has done this since the 4.2.0 pin (ADR-0034); this
    # script did not, which left the one-command Windows build unusable on a clean machine. Exporting
    # only copies the recipe into the cache — fast and idempotent, so it runs unconditionally.
    # Mirrors scripts/conan-export-local.sh; keep the two in step.
    Say 'C++: conan export (local recipes)'
    Get-ChildItem (Join-Path $repoRoot 'third_party/conan-recipes') -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'conanfile.py') } |
        ForEach-Object { Run $conan export $_.FullName }

    Say "C++: conan install ($buildType)"
    # AV1 codecs (SVT-AV1 + dav1d) built optimized even under Debug — see scripts/build.sh for why
    # (their debug asserts otherwise flaked macOS CI; a Release C library mixes in safely).
    # -c tools.cmake.cmaketoolchain:generator=Ninja is load-bearing on Windows and a no-op elsewhere.
    # With an MSVC profile Conan's CMakeToolchain otherwise assumes the *Visual Studio* generator and
    # writes CMAKE_GENERATOR_PLATFORM=x64 into conan_toolchain.cmake — but CMakePresets.json pins the
    # Ninja generator, and Ninja rejects a platform specification:
    #   CMake Error: Generator Ninja does not support platform specification, but platform x64 was
    #   specified.  /  CMAKE_CXX_COMPILER not set, after EnableLanguage
    # Conan must be told which generator the presets use. build.sh needs no equivalent: on Linux the
    # toolchain emits no platform/toolset at all.
    Run $conan install . -of "build/$Preset" -s build_type=$buildType -s compiler.cppstd=20 `
        -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" `
        -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing

    Say "C++: cmake configure ($Preset)"; Run cmake --preset $Preset
    Say "C++: cmake build ($Preset)"; Run cmake --build --preset $Preset

    if (-not $NoTests) {
        Say 'C++: ctest'
        Run ctest --test-dir "build/$Preset" --output-on-failure
    }
}

if (-not $CppOnly) {
    # rust-toolchain.toml lives in tools/, so run cargo from there.
    $cargoArgs = if ($Preset -eq 'release') { @('--release') } else { @() }
    Push-Location tools
    try {
        Say 'Rust: cargo build'; Run cargo build @cargoArgs
        if (-not $NoTests) { Say 'Rust: cargo test'; Run cargo test @cargoArgs }
    }
    finally { Pop-Location }
}

Say "done ($Preset)"
