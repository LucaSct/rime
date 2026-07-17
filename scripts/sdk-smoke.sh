#!/usr/bin/env bash
# Rime — SDK smoke test (M6.8): prove the *installed* package is usable out of tree.
#
# The engine's own tests build in-tree, where every target and header is already in scope. That
# proves the code works; it does NOT prove the SDK works — that a downstream project can install
# Rime, `find_package(rime CONFIG)`, link rime::*, and build against <prefix>/include with nothing
# but the install. This script proves exactly that, end to end:
#
#   build engine -> cmake --install to a throwaway prefix -> configure the out-of-tree
#   tests/sdk_consumer against that prefix -> build it -> run it on a cooked mesh
#
# plus two install-hygiene guards: no private/backend headers escaped into the SDK, and no absolute
# build-tree paths leaked into the exported targets file (the classic BUILD_INTERFACE mistake). It
# is the runnable form of the "Rime's SDK/package story lands here" commitment (ADR-0016 rule 5).
# See docs/design/sdk.md. Mirrors scripts/build.sh's Conan discovery so it is self-sufficient.
set -euo pipefail

preset="dev"
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        -h|--help)  echo "Usage: scripts/sdk-smoke.sh [--preset dev|release]"; exit 0 ;;
        *) echo "sdk-smoke.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

case "$preset" in
    dev)     build_type="Debug" ;;
    release) build_type="RelWithDebInfo" ;;
    *) echo "sdk-smoke.sh: unknown preset '$preset' (expected dev or release)" >&2; exit 2 ;;
esac

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

build_dir="build/$preset"
toolchain="$repo_root/$build_dir/conan_toolchain.cmake"

# ── 1) Build the engine (incremental; conan install first if the toolchain isn't there yet) ──────
if [ ! -f "$toolchain" ]; then
    if command -v conan >/dev/null 2>&1; then conan="conan"
    elif [ -x "$HOME/.rime-tools/bin/conan" ]; then conan="$HOME/.rime-tools/bin/conan"
    else echo "sdk-smoke.sh: conan not found — run scripts/setup.sh first" >&2; exit 1
    fi
    say "conan install ($build_type)"
    # AV1 codecs built optimized even under Debug (see scripts/build.sh) — keeps the resolved codec
    # packages identical to the main build (no cache thrash) and dodges their debug-only asserts.
    "$conan" install . -of "$build_dir" -s build_type="$build_type" -s compiler.cppstd=20 \
        -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" --build=missing
fi

say "build engine ($preset)"
cmake --preset "$preset"
cmake --build --preset "$preset"

# ── 2) Install to a throwaway prefix ─────────────────────────────────────────────────────────────
prefix="$build_dir/sdk-prefix"
rm -rf "$prefix"
say "install SDK -> $prefix"
cmake --install "$build_dir" --prefix "$prefix"

# ── 3) Install-hygiene guards ────────────────────────────────────────────────────────────────────
# (a) No private/backend headers may appear in the public SDK: the RHI's Vulkan headers, the
#     embedded SPIR-V, the platform's private seam header, or the generated xdg-shell glue. The
#     PRIVATE include discipline (engine/rhi, engine/platform) must survive installation.
say "guard: no private headers leaked into the SDK"
leak="$(find "$prefix/include" \
    \( -iname '*vulkan*' -o -iname '*.spv.h' -o -name 'platform_backend*' -o -name 'xdg-shell*' \) \
    -print)"
if [ -n "$leak" ]; then
    echo "FAIL: private/backend headers escaped into the SDK:" >&2
    echo "$leak" >&2
    exit 1
fi

# (b) No absolute build-tree path may appear in the exported targets file — that would mean a
#     BUILD_INTERFACE include dir leaked into the install interface (the config would only work on
#     the machine that built it). The $<BUILD_INTERFACE:>/$<INSTALL_INTERFACE:> splits prevent this.
if grep -rqF "$repo_root" "$prefix/lib/cmake/rime/"; then
    echo "FAIL: an absolute build path leaked into the installed CMake package:" >&2
    grep -rnF "$repo_root" "$prefix/lib/cmake/rime/" >&2
    exit 1
fi

# ── 4) Configure + build the OUT-OF-TREE consumer against the installed prefix ───────────────────
# rime_DIR points find_package straight at the installed config; the Conan toolchain is what makes
# Rime's third-party dependencies (fmt, volk, VMA, turbojpeg, lz4, …) findable in the consumer's
# context — the documented v1 dependency stance. The consumer is configured in complete isolation
# from the engine build tree.
consumer_build="$build_dir/sdk-consumer-build"
rm -rf "$consumer_build"
say "configure + build out-of-tree consumer"
cmake -S "$repo_root/tests/sdk_consumer" -B "$consumer_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -Drime_DIR="$repo_root/$prefix/lib/cmake/rime"
cmake --build "$consumer_build"

# ── 5) Run it — headless tick + cooked-mesh load ─────────────────────────────────────────────────
say "run consumer (headless tick + load a cooked mesh)"
"$consumer_build/sdk_consumer" "$repo_root/tests/assets/fixtures/cube.rmesh"

say "SDK smoke: PASS"
