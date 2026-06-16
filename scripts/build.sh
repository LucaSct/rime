#!/usr/bin/env bash
# Rime — one-command build for the whole project (C++ engine + Rust tools).
#
# This wraps the two toolchains (Conan/CMake and Cargo) behind a single command, so
# "build everything and run the tests" is one step both locally and in CI. It is
# deliberately thin and readable: it runs the same commands documented in CLAUDE.md, in
# order, and echoes each stage. Run scripts/setup.sh first if a toolchain is missing.
set -euo pipefail

usage() {
    cat <<'EOF'
Rime build — build the C++ engine and Rust tools, and run their tests.

Usage: scripts/build.sh [options]
  --preset dev|release   configuration to build (default: dev)
  --no-tests             build only; skip ctest and cargo test
  --cpp-only             build just the C++ engine
  --rust-only            build just the Rust tools
  --clean                remove build/<preset> and tools/target first
  -h, --help             show this help
EOF
}

preset="dev"; run_tests=1; do_cpp=1; do_rust=1; clean=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        --no-tests) run_tests=0; shift ;;
        --cpp-only) do_rust=0; shift ;;
        --rust-only) do_cpp=0; shift ;;
        --clean)    clean=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "build.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

# Map our CMake preset names to the CMAKE_BUILD_TYPE that Conan must resolve binaries for.
case "$preset" in
    dev)     build_type="Debug" ;;
    release) build_type="RelWithDebInfo" ;;
    *) echo "build.sh: unknown preset '$preset' (expected dev or release)" >&2; exit 2 ;;
esac

# Always operate from the repo root (this script lives in scripts/).
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

if [ "$clean" -eq 1 ]; then
    say "clean"
    rm -rf "build/$preset" tools/target
fi

if [ "$do_cpp" -eq 1 ]; then
    # Locate Conan: prefer one on PATH, else the isolated venv that setup.sh creates.
    if command -v conan >/dev/null 2>&1; then
        conan="conan"
    elif [ -x "$HOME/.rime-tools/bin/conan" ]; then
        conan="$HOME/.rime-tools/bin/conan"
    else
        echo "build.sh: conan not found — run scripts/setup.sh first" >&2; exit 1
    fi

    say "C++: conan install ($build_type)"
    "$conan" install . -of "build/$preset" \
        -s build_type="$build_type" -s compiler.cppstd=20 --build=missing

    say "C++: cmake configure ($preset)"
    cmake --preset "$preset"

    say "C++: cmake build ($preset)"
    cmake --build --preset "$preset"

    if [ "$run_tests" -eq 1 ]; then
        say "C++: ctest"
        ctest --test-dir "build/$preset" --output-on-failure
    fi
fi

if [ "$do_rust" -eq 1 ]; then
    # Make cargo reachable even from a shell that hasn't sourced ~/.cargo/env yet.
    if ! command -v cargo >/dev/null 2>&1; then
        # shellcheck disable=SC1091
        . "$HOME/.cargo/env" 2>/dev/null || true
    fi
    command -v cargo >/dev/null 2>&1 || { echo "build.sh: cargo not found — run scripts/setup.sh first" >&2; exit 1; }

    # rust-toolchain.toml lives in tools/, so run cargo from there (a subshell keeps cwd).
    cargo_flags=""
    [ "$preset" = "release" ] && cargo_flags="--release"

    say "Rust: cargo build"
    ( cd tools && cargo build $cargo_flags )
    if [ "$run_tests" -eq 1 ]; then
        say "Rust: cargo test"
        ( cd tools && cargo test $cargo_flags )
    fi
fi

say "done ($preset)"
