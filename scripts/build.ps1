# Rime - one-command build for the whole project (C++ engine + Rust tools) on Windows.
# Mirrors scripts/build.sh; the two are kept in step and the Windows path is exercised by
# CI on the Windows runner (Milestone 0.5) - under `shell: pwsh`, note, so CI does NOT see
# the PowerShell 5.1 encoding trap described in Run() below. Run scripts/setup.ps1 first if
# a tool is missing.
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
# `cargo test` - which needs none of them - succeeded and became the script's exit status. The job
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
        # the third byte of a UTF-8 em dash decodes to U+201D - a character PowerShell accepts as a
        # STRING DELIMITER. The literal then ends early and the rest of the line is parsed as code:
        # "Unexpected token 'failed' in expression or statement". The rule is the whole FILE, not
        # just this line: an em dash surviving in a comment or a single-quoted string is inert only
        # while no double-quoted string has left a string open above it, and when one does the error
        # is reported against the innocent line where the runaway string finally closes. That is how
        # scripts/setup.ps1 was unable to start from M0.4 until this commit; its header has the account.
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
    else { throw 'conan not found - run scripts/setup.ps1 first' }

    # Our own recipes (libsvtav1/4.2.0) must reach the Conan cache before install can resolve them -
    # Conan Center stops at 2.2.1, so without this `conan install` fails with "Package
    # 'libsvtav1/4.2.0' not resolved". build.sh has done this since the 4.2.0 pin (ADR-0034); this
    # script did not, which left the one-command Windows build unusable on a clean machine. Exporting
    # only copies the recipe into the cache - fast and idempotent, so it runs unconditionally.
    # Mirrors scripts/conan-export-local.sh; keep the two in step.
    Say 'C++: conan export (local recipes)'
    Get-ChildItem (Join-Path $repoRoot 'third_party/conan-recipes') -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'conanfile.py') } |
        ForEach-Object { Run $conan export $_.FullName }

    Say "C++: conan install ($buildType)"
    # AV1 codecs (SVT-AV1 + dav1d) built optimized even under Debug - see scripts/build.sh for why
    # (their debug asserts otherwise flaked macOS CI; a Release C library mixes in safely).
    # -c tools.cmake.cmaketoolchain:generator=Ninja is load-bearing on Windows and a no-op elsewhere.
    # With an MSVC profile Conan's CMakeToolchain otherwise assumes the *Visual Studio* generator and
    # writes CMAKE_GENERATOR_PLATFORM=x64 into conan_toolchain.cmake - but CMakePresets.json pins the
    # Ninja generator, and Ninja rejects a platform specification:
    #   CMake Error: Generator Ninja does not support platform specification, but platform x64 was
    #   specified.  /  CMAKE_CXX_COMPILER not set, after EnableLanguage
    # Conan must be told which generator the presets use. build.sh needs no equivalent: on Linux the
    # toolchain emits no platform/toolset at all.
    #
    # -s:b compiler.cppstd=17 is the other Windows-only flag, and it is about the BUILD context, not
    # ours. Plain -s sets only the HOST profile; the build profile keeps whatever `conan profile
    # detect` chose, and on MSVC that is cppstd=14. glslang -- the offline shader compiler, a build
    # requirement -- pulls in spirv-tools, which refuses to compile below 17:
    #   spirv-tools/1.4.313.0: Cannot build for this configuration: Current cppstd (14) is lower
    #   than the required C++ standard (17).  /  ERROR: There are invalid packages
    # build.sh needs no equivalent because `conan profile detect` on GCC/Clang picks gnu17 already,
    # which is also why 17 and not 20: it is the value the Linux build context has been proving
    # green all along, rather than a new one for these two tools to meet.
    #
    # This one is worth knowing HOW it hides. It only bites when Conan actually has to build those
    # tools, and whether it does depends on the MSVC version: Conan Center has prebuilt binaries for
    # compiler.version=194, so a machine on VS 17.14 downloads them and never exercises the build
    # profile at all. The CI runner is on 195, finds no match, builds from source, and fails in
    # `conan install` before a single file of ours is compiled. A green local build proves nothing
    # here -- the two machines are not running the same steps.
    # '-s:b' is QUOTED, and must stay that way. Bare, PowerShell reads -s:b as its own -Name:Value
    # parameter syntax, splits it there, and hands conan `-s:` and `b` as two arguments -- which it
    # rejects with "ambiguous option: -s: could match -s, -s:b, -s:h, -s:a". Quoting passes the token
    # through untouched. Same trap for -o:b / -c:b, should either ever be needed. (No comment lines
    # inside the argument list below, either: a backtick continuation cannot span one.)
    Run $conan install . -of "build/$Preset" -s build_type=$buildType -s compiler.cppstd=20 `
        '-s:b' compiler.cppstd=17 `
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
    # Point the rime-ffi crate (M6.9) at the freshly-built C ABI so its live tests link the current
    # rime_capi instead of compiling themselves out. build.sh does this with RIME_CAPI_DIR plus an
    # -Wl,-rpath baked into the test binary so the loader finds the library without LD_LIBRARY_PATH.
    # Windows has no rpath, which is why this was left undone (docs/design/ffi.md called it deferred)
    # and why those four tests have reported themselves ignored on every Windows run there has been.
    #
    # PATH is the Windows answer: it is what the loader actually searches for a DLL. Two directories,
    # because two different tools want two different files -- the LINKER needs the import library
    # build/<preset>/lib/rime_capi.lib, the LOADER needs build/<preset>/bin/rime_capi.dll. Nothing
    # else was ever missing: the DLL has always exported its symbols properly (engine/capi compiles
    # with RIME_CAPI_BUILD, which flips the header's macro to dllexport), so this closes the gap
    # rather than working around it. Verified: with both set, all four live tests run and pass here.
    #
    # Guarded on both files existing, mirroring build.sh's .so/.dylib check. -RustOnly, or a tree
    # configured with RIME_BUILD_CAPI=OFF, leaves the variable unset and the tests honestly ignored
    # -- which is now a visible `0 passed; 4 ignored` rather than a green `4 passed` that ran nothing.
    if (-not $RustOnly) {
        $capiLibDir = Join-Path $repoRoot "build/$Preset/lib"
        $capiBinDir = Join-Path $repoRoot "build/$Preset/bin"
        if ((Test-Path (Join-Path $capiLibDir 'rime_capi.lib')) -and
            (Test-Path (Join-Path $capiBinDir 'rime_capi.dll'))) {
            $env:RIME_CAPI_DIR = $capiLibDir
            $env:PATH = "$capiBinDir;$env:PATH"
            Say "Rust: RIME_CAPI_DIR=$capiLibDir (rime-ffi links the C ABI)"
        }
    }

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
