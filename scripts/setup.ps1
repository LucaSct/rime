# Rime — developer environment setup on Windows. Mirrors scripts/setup.sh: auto-installs
# Conan (isolated venv) if missing, checks + guides for CMake/Ninja/MSVC/Rust/Vulkan.
# Exercised by CI on the Windows runner (Milestone 0.5). After setup: scripts/build.ps1
$ErrorActionPreference = 'Stop'
function Ok($m) { Write-Host "  [ok] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [!]  $m" -ForegroundColor Yellow }
function Say($m) { Write-Host "`n== $m ==" -ForegroundColor Cyan }

Say 'System build tools (checked, not auto-installed)'
if (Get-Command cmake -EA SilentlyContinue) { Ok "cmake — $((cmake --version)[0])" }
else { Warn 'cmake missing — winget install Kitware.CMake' }
if (Get-Command ninja -EA SilentlyContinue) { Ok 'ninja found' }
else { Warn 'ninja missing — winget install Ninja-build.Ninja' }
if (Get-Command cl -EA SilentlyContinue) { Ok 'MSVC (cl) found' }
else { Warn 'MSVC not found — install Visual Studio Build Tools with the C++ workload (run from a Developer prompt)' }

Say 'Conan (C++ dependencies)'
if (Get-Command conan -EA SilentlyContinue) { Ok "conan — $(conan --version)" }
elseif (Test-Path "$HOME/.rime-tools/Scripts/conan.exe") { Ok 'conan in ~/.rime-tools' }
else {
    Warn 'conan missing — installing into an isolated venv at ~/.rime-tools'
    python -m venv "$HOME/.rime-tools"
    & "$HOME/.rime-tools/Scripts/pip.exe" install --quiet --upgrade pip
    & "$HOME/.rime-tools/Scripts/pip.exe" install --quiet 'conan>=2,<3'
    Ok 'conan installed'
}
$conan = if (Get-Command conan -EA SilentlyContinue) { 'conan' } else { "$HOME/.rime-tools/Scripts/conan.exe" }
& $conan profile path default 2>$null
if ($LASTEXITCODE -ne 0) { & $conan profile detect; Ok 'created default Conan profile' }

Say 'Rust (cargo, rustfmt, clippy)'
if (Get-Command cargo -EA SilentlyContinue) { Ok "cargo — $(cargo --version)" }
else { Warn 'rust missing — install from https://rustup.rs (rustup-init.exe), then re-run' }

Say 'Vulkan SDK (needed from Milestone 3)'
if ($env:VULKAN_SDK) { Ok 'Vulkan SDK detected' }
else { Warn 'not found — install from https://vulkan.lunarg.com before M3' }

Say 'setup complete — next: scripts/build.ps1'
