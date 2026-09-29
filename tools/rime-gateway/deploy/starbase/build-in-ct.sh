# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env bash
# Runs INSIDE CT 122 as `dev` (install.sh invokes it via `runuser -u dev --` right after streaming a
# `git archive` of the workstation's HEAD there). Builds rime-gateway, rime-turn-proxy and
# the-block-host for THIS container's glibc and stages them exactly as apply.sh will install them.
#
# Source lives at .../src-<sha>/, a FRESH extraction every deploy — install.sh never reuses one
# commit's directory for another, so this path is different on every run. That is deliberate (a
# stage must be traceable to the commit it came from) and it has a cost: neither cargo's nor cmake's
# incremental build state is keyed only on file content, so a source path that moves invalidates it
# even when nothing actually changed. The Cargo target dir and the CMake build dir are still pinned
# to STABLE paths outside the source tree, so what they keep across deploys is the expensive part
# that genuinely does not depend on our source path — every third-party crate and Conan package.
# Our own object files are rebuilt from scratch each deploy; see the report for measured cold/warm
# times.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
sha="${repo_root##*/src-}"
deploy_root="/srv/dev/deploy"
cargo_target="$deploy_root/cargo-target"
cmake_build="$deploy_root/build-release"
stage="$deploy_root/stage-$sha"

# `runuser -u dev --` (install.sh's invocation) changes the UID and $HOME but NOT the working
# directory — it inherits whatever `pct exec` started in, which is `/root`, a directory `dev` cannot
# even stat. Conan's workspace search reads `os.getcwd()` before it reads any argument, so it failed
# on a bare PermissionError before printing a single word about what it was doing (measured
# 2026-09-29). Every step below assumes a readable, writable cwd, so make that true explicitly rather
# than rely on the caller's.
cd "$repo_root"

say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

# Cargo and the Conan venv live under $HOME but `runuser -u dev --` runs a non-login shell, so PATH
# is whatever install.sh's ssh session had — mirror scripts/build.sh's defensive lookup rather than
# assume either is on it.
if ! command -v cargo >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "$HOME/.cargo/env" 2>/dev/null || true
fi
command -v cargo >/dev/null 2>&1 || { echo "build-in-ct.sh: cargo not found" >&2; exit 1; }
if command -v conan >/dev/null 2>&1; then conan="conan"
elif [ -x "$HOME/.rime-tools/bin/conan" ]; then conan="$HOME/.rime-tools/bin/conan"
else echo "build-in-ct.sh: conan not found" >&2; exit 1
fi

# ── Rust: rime-gateway + rime-turn-proxy ──────────────────────────────────────────────────────────
say "cargo build (release, --features auth)"
export CARGO_TARGET_DIR="$cargo_target"
( cd "$repo_root/tools" && cargo build --release -p rime-gateway --features auth )

# ── C++: the-block-host ───────────────────────────────────────────────────────────────────────────
say "conan export local recipes"
"$repo_root/scripts/conan-export-local.sh" "$conan"

say "conan install (RelWithDebInfo)"
"$conan" install "$repo_root" -of "$cmake_build" \
    -s build_type=RelWithDebInfo -s compiler.cppstd=20 \
    -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" --build=missing

# A stable BUILD dir pointed at a source path that moves every deploy: cmake refuses to reconfigure
# a cache recorded against a different source directory, so drop the cache (never the compiled
# objects — `cmake -S -B` regenerates build.ninja from a clean cache without touching those) rather
# than let that surface mid-deploy as a cryptic mismatch error.
rm -f "$cmake_build/CMakeCache.txt"

say "cmake configure (release)"
cmake -S "$repo_root" -B "$cmake_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$cmake_build/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

say "cmake build (the_block_host)"
cmake --build "$cmake_build" --target the_block_host

# ── Stage exactly as apply.sh will install it ─────────────────────────────────────────────────────
say "staging $stage"
rm -rf "$stage"
install -d -m 755 "$stage/bin" "$stage/games/the-block" "$stage/deploy"

install -m 755 "$cargo_target/release/rime-gateway" "$stage/bin/rime-gateway"
install -m 755 "$cargo_target/release/rime-turn-proxy" "$stage/bin/rime-turn-proxy"
install -m 755 "$cmake_build/bin/the-block-host" "$stage/games/the-block/the-block-host"

# The web front end (a separate, still-landing brick, ADR-0046 §2) is optional here on purpose: its
# absence must not fail a deploy of the API/game half, and apply.sh only installs what it finds.
if [ -d "$repo_root/tools/rime-gateway/web" ]; then
    cp -a "$repo_root/tools/rime-gateway/web" "$stage/web"
fi

deploy_src="$repo_root/tools/rime-gateway/deploy/starbase"
cp "$deploy_src/"*.service "$deploy_src/"*.path "$deploy_src/Caddyfile" "$deploy_src/turnserver.conf" \
   "$deploy_src/turn-egress.nft" "$stage/deploy/"
install -m 755 "$deploy_src/media-routing.sh" "$deploy_src/sync-turn-cert.sh" "$stage/bin/"
install -m 755 "$deploy_src/apply.sh" "$stage/apply.sh"
install -m 755 "$deploy_src/prereqs.sh" "$stage/prereqs.sh"
echo "$sha" > "$stage/COMMIT"

# Keep the last two of each generation; deploy dirs hold whole binaries (and now a git tree apiece),
# so an unpruned history fills the container's disk quietly rather than loudly. mtime-sorted via
# find, not `ls -t` (locale/format-dependent and shellcheck-unfriendly to parse).
for prefix in stage src; do
    mapfile -t old_dirs < <(find "$deploy_root" -maxdepth 1 -name "$prefix-*" -printf '%T@ %p\n' \
        | sort -rn | awk '{print $2}' | tail -n +3)
    for old in "${old_dirs[@]:-}"; do
        [ -n "$old" ] && rm -rf "$old"
    done
done

say "build-in-ct.sh: staged $stage"
