# Rime - developer environment setup on Windows. Mirrors scripts/setup.sh: auto-installs
# Conan (isolated venv) if missing, checks + guides for CMake/Ninja/MSVC/Rust/Vulkan.
# After setup: scripts/build.ps1
#
# KEEP THIS FILE ASCII-ONLY. Not a style rule - a non-ASCII byte here stops the script from
# running at all. Windows PowerShell 5.1 (powershell.exe: still what the Start menu, `powershell`
# in cmd, and a double-clicked .ps1 all give you) reads a BOM-less script in the machine's ANSI
# codepage rather than UTF-8. On a Western install that is Windows-1252, where an em dash's three
# UTF-8 bytes decode to 'a-circumflex', 'euro sign' and U+201D - and PowerShell honours U+201D as
# a DOUBLE-QUOTE DELIMITER. So one em dash inside a double-quoted string closes that string early,
# the `"` meant to close it opens a NEW one, and that one runs on down the file until it meets the
# next quote character - typically the U+201D of an em dash on an unrelated, single-quoted line.
#
# Two things follow, and both are worth knowing before you go fix the line the error points at:
#
#   * THE REPORTED LINE IS NOT THE GUILTY LINE. This script used to die with "The term 'run' is
#     not recognized as the name of a cmdlet" pointing at line 15 - a single-quoted Warn string
#     that is entirely innocent. The damage was done five lines above it, by the em dash in
#     `Ok "cmake - $((cmake --version)[0])"`. It also failed HALF-WAY: the first section header
#     printed, then the Conan bootstrap - the whole point of the script - never ran.
#   * NO CI JOB CAN SEE IT. ci.yml runs the Windows step under `shell: pwsh` (PowerShell 7, which
#     reads UTF-8 whatever the codepage), and it never invokes this script at all. The header here
#     claimed "Exercised by CI on the Windows runner" anyway; it was not, and the first thing a
#     Windows newcomer is told to run has been unable to start since M0.4 added it - the em dash
#     on the `cmake` line is in the file's very first revision (f894bd9, 2026-06-17).
#
# An em dash in a comment or a single-quoted string happens to parse fine (it merely prints as
# mojibake), but that is a fact about where the other quote characters fall, not a rule to lean
# on. Whole file ASCII, and the lint job greps for it. scripts/build.ps1 carries the same rule.
$ErrorActionPreference = 'Stop'
function Ok($m) { Write-Host "  [ok] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [!]  $m" -ForegroundColor Yellow }
function Say($m) { Write-Host "`n== $m ==" -ForegroundColor Cyan }

Say 'System build tools (checked, not auto-installed)'
if (Get-Command cmake -EA SilentlyContinue) { Ok "cmake - $((cmake --version)[0])" }
else { Warn 'cmake missing - winget install Kitware.CMake' }
if (Get-Command ninja -EA SilentlyContinue) { Ok 'ninja found' }
else { Warn 'ninja missing - winget install Ninja-build.Ninja' }
if (Get-Command cl -EA SilentlyContinue) { Ok 'MSVC (cl) found' }
else { Warn 'MSVC not found - install Visual Studio Build Tools with the C++ workload (run from a Developer prompt)' }

Say 'Conan (C++ dependencies)'
if (Get-Command conan -EA SilentlyContinue) { Ok "conan - $(conan --version)" }
elseif (Test-Path "$HOME/.rime-tools/Scripts/conan.exe") { Ok 'conan in ~/.rime-tools' }
else {
    Warn 'conan missing - installing into an isolated venv at ~/.rime-tools'
    python -m venv "$HOME/.rime-tools"
    & "$HOME/.rime-tools/Scripts/pip.exe" install --quiet --upgrade pip
    & "$HOME/.rime-tools/Scripts/pip.exe" install --quiet 'conan>=2,<3'
    Ok 'conan installed'
}
$conan = if (Get-Command conan -EA SilentlyContinue) { 'conan' } else { "$HOME/.rime-tools/Scripts/conan.exe" }
# A PROBE, not a report: setup.sh sends both streams to /dev/null. `2>$null` only silences
# stderr, so this printed the profile path into the middle of the run's output.
& $conan profile path default 1>$null 2>$null
if ($LASTEXITCODE -ne 0) { & $conan profile detect; Ok 'created default Conan profile' }

Say 'Rust (cargo, rustfmt, clippy)'
if (Get-Command cargo -EA SilentlyContinue) { Ok "cargo - $(cargo --version)" }
else { Warn 'rust missing - install from https://rustup.rs (rustup-init.exe), then re-run' }

Say 'Vulkan runtime (to RUN the renderer; the build''s Vulkan deps come from Conan)'
# Building needs no Vulkan SDK - Conan supplies headers/volk/VMA/glslang. A Vulkan runtime (GPU
# driver, or a software ICD like lavapipe for headless) is only needed to run the renderer.
# $env:VULKAN_SDK alone is the wrong question. The LunarG SDK sets that variable; a GPU DRIVER
# does not, and it is the driver that supplies the ICD you need in order to RUN anything. So this
# reported 'none found' on a machine whose GPU the windowed tests were at that moment presenting
# real frames on, and advised the user to go install a GPU driver. setup.sh has always accepted
# vulkaninfo on PATH as well; mirror it. That works on Windows without the LunarG SDK because the
# GPU driver installs vulkaninfo.exe into System32, which is on the machine PATH.
if ($env:VULKAN_SDK -or (Get-Command vulkaninfo -EA SilentlyContinue)) { Ok 'Vulkan runtime detected' }
else { Warn 'none found - the build still works (Conan supplies the Vulkan build deps). To run the renderer, install a GPU driver, or the LunarG SDK for validation layers: https://vulkan.lunarg.com' }

Say 'setup complete - next: scripts/build.ps1'
