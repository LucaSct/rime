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
  --preset dev|release       configuration to build (default: dev)
  --sanitizer off|address|thread
                             build the C++ engine with a sanitizer (default: off).
                             address = ASan+UBSan, thread = TSan. GCC/Clang only.
  --no-tests                 build only; skip ctest and cargo test
  --cpp-only                 build just the C++ engine
  --rust-only                build just the Rust tools
  --clean                    remove build/<preset> and tools/target first
  -h, --help                 show this help
EOF
}

preset="dev"; run_tests=1; do_cpp=1; do_rust=1; clean=0; sanitizer="off"
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        --sanitizer)   sanitizer="${2:?--sanitizer needs a value}"; shift 2 ;;
        --sanitizer=*) sanitizer="${1#*=}"; shift ;;
        --no-tests) run_tests=0; shift ;;
        --cpp-only) do_rust=0; shift ;;
        --rust-only) do_cpp=0; shift ;;
        --clean)    clean=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "build.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

case "$sanitizer" in
    off|address|thread) ;;
    *) echo "build.sh: unknown --sanitizer '$sanitizer' (expected off, address, or thread)" >&2; exit 2 ;;
esac

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
    # Build the AV1 codecs (SVT-AV1 encoder + dav1d decoder) OPTIMIZED even in a Debug engine build.
    # They are third-party C libraries we never step-debug, and their internal assert()s otherwise
    # run under Debug — a multithreaded SVT-AV1 encoder assertion (svt_aom_get_txb_ctx) intermittently
    # aborted the macOS CI. A Release (NDEBUG) codec compiles those out; the stable C ABI makes mixing
    # a Release codec into a Debug engine safe, and Debug builds get faster into the bargain.
    "$conan" install . -of "build/$preset" \
        -s build_type="$build_type" -s compiler.cppstd=20 \
        -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" --build=missing

    # Extra cache var layered on top of the preset. --sanitizer maps to the RIME_SANITIZER
    # option (see /CMakeLists.txt); off is the default and adds nothing. The ${var:+…} guard
    # expands to nothing when empty, so this stays safe under `set -u` (and on macOS bash 3.2).
    san_arg=""
    [ "$sanitizer" != "off" ] && san_arg="-DRIME_SANITIZER=$sanitizer"

    say "C++: cmake configure ($preset${san_arg:+, sanitizer=$sanitizer})"
    cmake --preset "$preset" ${san_arg:+"$san_arg"}

    say "C++: cmake build ($preset)"
    cmake --build --preset "$preset"

    # Under AddressSanitizer on Linux, LeakSanitizer runs at exit and flags a few allocations the
    # un-instrumented Vulkan loader makes inside vkCreateInstance for any GPU-touching test on
    # lavapipe — third-party leaks, not ours (scripts/lsan-suppressions.txt has the captured stack).
    # Point LSan at the shared suppression list so a local full-suite ASan run isn't reddened by that
    # DIRECT leak. (Its indirect driver-thread children are un-suppressible — see the file; CI splits
    # them off with detect_leaks=0 + a GPU-free leak pass, which is the reliable gate.) A caller who
    # already exported LSAN_OPTIONS wins (we only default it).
    if [ "$sanitizer" = "address" ] && [ -z "${LSAN_OPTIONS:-}" ]; then
        export LSAN_OPTIONS="suppressions=$repo_root/scripts/lsan-suppressions.txt"
    fi

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

    # Point the rime-ffi crate (M6.9) at the freshly-built C ABI so its tests link against the
    # current librime_capi. Only meaningful after a C++ build produced the library; on Linux/macOS
    # an rpath baked by the crate's build.rs then makes it discoverable at runtime. Windows has no
    # rpath, so we leave RIME_CAPI_DIR unset there and the FFI tests skip themselves (a documented v1
    # gap — see docs/design/ffi.md). Unset => the crate builds and its tests pass by skipping.
    capi_lib_dir="$repo_root/build/$preset/lib"
    if [ "$do_cpp" -eq 1 ] && { [ -e "$capi_lib_dir/librime_capi.so" ] || [ -e "$capi_lib_dir/librime_capi.dylib" ]; }; then
        export RIME_CAPI_DIR="$capi_lib_dir"
        say "Rust: RIME_CAPI_DIR=$RIME_CAPI_DIR (rime-ffi links the C ABI)"
    fi

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
