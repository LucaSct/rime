#!/usr/bin/env bash
# Rime — developer environment setup. Checks for the toolchains the build needs and, where
# it is safe and user-local, installs the missing ones. Safe to run repeatedly: it only
# acts on what is absent.
#
#   Auto-installed (user-local, reversible):
#     - Conan (C++ deps)  -> an isolated venv at ~/.rime-tools
#     - Rust (cargo, …)   -> via rustup (stable)
#   Checked + guided only (system / OS-specific to install):
#     - CMake (>= 3.24), Ninja, a C++ compiler, and the Vulkan SDK (needed from M3).
#
# After setup, build with: scripts/build.sh
set -euo pipefail

ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$1"; }
say()  { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

say "System build tools (checked, not auto-installed)"
if command -v cmake >/dev/null 2>&1; then ok "cmake — $(cmake --version | head -1)"
else warn "cmake missing — install CMake >= 3.24  (brew install cmake | apt install cmake | winget install Kitware.CMake)"; fi
if command -v ninja >/dev/null 2>&1; then ok "ninja — $(ninja --version)"
else warn "ninja missing  (brew install ninja | apt install ninja-build | winget install Ninja-build.Ninja)"; fi
if command -v cc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1; then
    ok "C++ compiler found"
else warn "no C++ compiler found (install Xcode CLT, gcc, clang, or MSVC)"; fi

say "Conan (C++ dependencies)"
if command -v conan >/dev/null 2>&1; then
    ok "conan on PATH — $(conan --version)"
elif [ -x "$HOME/.rime-tools/bin/conan" ]; then
    ok "conan in ~/.rime-tools — $("$HOME/.rime-tools/bin/conan" --version)"
else
    warn "conan missing — installing into an isolated venv at ~/.rime-tools"
    python3 -m venv "$HOME/.rime-tools"
    "$HOME/.rime-tools/bin/pip" install --quiet --upgrade pip
    "$HOME/.rime-tools/bin/pip" install --quiet "conan>=2,<3"
    ok "installed $("$HOME/.rime-tools/bin/conan" --version)"
fi
# A default profile (autodetected from the compiler) is required for `conan install`.
conan_bin="$(command -v conan || echo "$HOME/.rime-tools/bin/conan")"
if ! "$conan_bin" profile path default >/dev/null 2>&1; then
    "$conan_bin" profile detect
    ok "created default Conan profile"
fi

say "Rust (cargo, rustfmt, clippy)"
if command -v cargo >/dev/null 2>&1 || [ -x "$HOME/.cargo/bin/cargo" ]; then
    # shellcheck disable=SC1091
    . "$HOME/.cargo/env" 2>/dev/null || true
    ok "cargo — $(cargo --version)"
else
    warn "rust missing — installing via rustup (stable, default profile)"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile default --default-toolchain stable
    # shellcheck disable=SC1091
    . "$HOME/.cargo/env"
    ok "installed $(cargo --version)"
fi

say "Vulkan runtime (to RUN the renderer; the build's Vulkan deps come from Conan)"
# Building needs no Vulkan SDK — Conan supplies the headers, volk, VMA, and glslang (see conanfile.py
# / docs/adr/0007). A Vulkan *runtime* is only needed to actually run the renderer: a GPU driver
# (ships the ICD), MoltenVK on macOS, or a software ICD (lavapipe) for headless machines.
if [ -n "${VULKAN_SDK:-}" ] || command -v vulkaninfo >/dev/null 2>&1; then
    ok "Vulkan runtime detected"
else
    warn "none found — the build still works (Conan supplies the Vulkan build deps). To *run* the"
    warn "renderer: a GPU driver, MoltenVK (macOS: brew install molten-vk vulkan-loader), or a"
    warn "software ICD (lavapipe). Validation layers come with the LunarG SDK: https://vulkan.lunarg.com"
fi

say "setup complete — next: scripts/build.sh"
