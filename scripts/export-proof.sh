#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# export-proof.sh — m20.2 (ADR-0076-m20.2): M20's done criterion, run on this machine.
#
#   scripts/export-proof.sh [--preset release|dev] [--no-build] [--keep]
#
# 1. Export hello-game to a temp directory OUTSIDE the repository (scripts/export-game.sh).
# 2. Run it in-tree (build/<preset>/bin/hello_game) — the reference digest.
# 3. Run the BUNDLE's `dedicated` in a sandbox with NO VULKAN and NO REPOSITORY:
#      bubblewrap mounts an empty tmpfs over $HOME (the repository, the build tree, the Conan cache,
#      ~/.rime-tools) and over every Vulkan ICD/layer manifest directory, binds /dev/null over the
#      Vulkan loader itself, and starts the game with a scrubbed environment (`env -i`: no
#      LD_LIBRARY_PATH, no LD_PRELOAD) plus VK_ICD_FILENAMES / VK_DRIVER_FILES naming nothing. The
#      sandbox first PROVES each of those is hidden (a check that cannot see what it hid would
#      read as passing), then runs the game.
# 4. Its state digest must equal the in-tree run's — mid-game (120 ticks) and to the win.
# 5. `play --headless` from the bundle, in the same sandbox: the device factory is entered, finds no
#    loader, answers null, and play degrades to the simulation. Same digest.
#
# Then it FALSIFIES itself, because a proof that cannot fail proves nothing:
#   F1. delete the bundle's content/ and run it — once in the sandbox and once OUTSIDE it, where the
#       source tree is right there and readable. Both must fail, naming the paths tried; the second
#       is the one that matters (a moved binary must never fall back to the repository).
#   F2. compare the digest against a deliberately wrong one — the comparison must reject it.
#
# Linux only (bubblewrap). Without bwrap it stops and says so rather than running a weaker proof
# under the same name.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"

preset="release"; build_flag=(); keep=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="${2:?}"; shift 2 ;;
        --no-build) build_flag=(--no-build); shift ;;
        --keep) keep=1; shift ;;
        *) echo "usage: scripts/export-proof.sh [--preset release|dev] [--no-build] [--keep]" >&2; exit 2 ;;
    esac
done

failures=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*" >&2; failures=$((failures + 1)); }
step() { printf '\n── %s\n' "$*"; }

command -v bwrap >/dev/null || { echo "export-proof: bwrap (bubblewrap) not found — not run" >&2; exit 1; }

out="$(mktemp -d /tmp/rime-export-proof.XXXXXX)"
[ "$keep" -eq 1 ] || trap 'rm -rf "$out"' EXIT
case "$out/" in "$repo"/*) echo "export-proof: temp dir is inside the repo" >&2; exit 1 ;; esac

# ── 1. Export ─────────────────────────────────────────────────────────────────────────────────
step "export hello_game to $out (outside $repo)"
"$repo/scripts/export-game.sh" hello_game "$out" --preset "$preset" "${build_flag[@]}"
bundle="$out/hello_game"
game="$bundle/hello_game"
intree="$repo/build/$preset/bin/hello_game"

# The digest from a run's one-line report ("… digest 0123456789abcdef …").
digest_of() { sed -n 's/.* digest \([0-9a-f]\{16\}\).*/\1/p' <<<"$1" | tail -n 1; }
# The comparison, as a function so F2 can call it with a wrong answer.
same_digest() { [ -n "$1" ] && [ "$1" = "$2" ]; }

# ── The no-Vulkan, no-repository sandbox ──────────────────────────────────────────────────────
vk_loader="$(readlink -f "$(ldconfig -p | sed -n 's/.*libvulkan\.so\.1 (libc6,x86-64) => //p' | head -n 1)" 2>/dev/null || true)"
sandbox_args=(--dev-bind / / --tmpfs "$HOME" --chdir /)
hidden=("$HOME (repository, build tree, Conan cache, ~/.rime-tools)")
for d in /usr/share/vulkan /etc/vulkan /usr/local/share/vulkan; do
    if [ -d "$d" ]; then sandbox_args+=(--tmpfs "$d"); hidden+=("$d (ICD + layer manifests)"); fi
done
if [ -n "$vk_loader" ] && [ -f "$vk_loader" ]; then
    sandbox_args+=(--ro-bind /dev/null "$vk_loader")
    hidden+=("$vk_loader (the Vulkan loader, replaced by /dev/null)")
fi
clean_env=(env -i PATH=/usr/bin:/bin HOME=/nonexistent VK_ICD_FILENAMES=/nonexistent/icd.json
           VK_DRIVER_FILES=/nonexistent/icd.json VK_LAYER_PATH=/nonexistent)
in_sandbox() { bwrap "${sandbox_args[@]}" "${clean_env[@]}" "$@"; }

step "the sandbox hides what it claims to"
for h in "${hidden[@]}"; do echo "  hidden: $h"; done
in_sandbox /bin/sh -c "
    test ! -e '$repo' || { echo 'repository visible'; exit 1; }
    test ! -e '$intree' || { echo 'in-tree binary visible'; exit 1; }
    test -z \"\$(ls -A '$HOME')\" || { echo 'HOME not empty'; exit 1; }
    for d in /usr/share/vulkan /etc/vulkan /usr/local/share/vulkan; do
        if [ -d \"\$d\" ] && [ -n \"\$(find \"\$d\" -type f | head -n 1)\" ]; then echo \"\$d has files\"; exit 1; fi
    done
    if [ -n '$vk_loader' ] && [ -s '$vk_loader' ]; then echo 'loader still readable'; exit 1; fi
    test -z \"\${LD_LIBRARY_PATH:-}\" || { echo 'LD_LIBRARY_PATH set'; exit 1; }
    test -x '$game' || { echo 'bundle not visible'; exit 1; }
" && ok "repository, build tree, \$HOME, ICD manifests and the loader are all invisible; the bundle is visible" \
  || bad "the sandbox does not hide what it claims"
echo "  not hidden: /usr/lib system libraries (libc, libstdc++, X11/Wayland client libs the binary links), /dev, /proc"

# ── 2-4. In-tree vs bundle, dedicated ─────────────────────────────────────────────────────────
for ticks in 120 2000; do
    step "dedicated --ticks $ticks --autopilot: in-tree vs bundle-in-sandbox"
    ref_out="$("$intree" dedicated --ticks "$ticks" --autopilot 2>&1)" || bad "in-tree run exited $?"
    echo "$ref_out" | sed 's/^/  in-tree | /'
    grep -q "content root .*in-tree fallback" <<<"$ref_out" && ok "in-tree run used the in-tree fallback" \
        || bad "in-tree run did not say it used the in-tree fallback"

    set +e
    bun_out="$(in_sandbox "$game" dedicated --ticks "$ticks" --autopilot 2>&1)"; rc=$?
    set -e
    echo "$bun_out" | sed 's/^/  bundle  | /'
    [ "$rc" -eq 0 ] && ok "bundle dedicated exited 0" || bad "bundle dedicated exited $rc"
    grep -q "content root $bundle/content (the content/ directory beside" <<<"$bun_out" \
        && ok "bundle used content/ beside its executable" || bad "bundle did not use its own content/"
    grep -q "device never requested" <<<"$bun_out" && ok "dedicated never requested a device" \
        || bad "dedicated touched the device path"

    ref_d="$(digest_of "$ref_out")"; bun_d="$(digest_of "$bun_out")"
    same_digest "$bun_d" "$ref_d" && ok "digest $bun_d == in-tree $ref_d" \
        || bad "digest mismatch: bundle '$bun_d' vs in-tree '$ref_d'"
    [ "$ticks" -eq 120 ] && ref_120="$ref_d"
done

# ── 5. play --headless from the bundle, no Vulkan ─────────────────────────────────────────────
step "play --headless --ticks 120 --autopilot from the bundle, in the sandbox"
set +e
play_out="$(in_sandbox "$game" play --headless --ticks 120 --autopilot 2>&1)"; rc=$?
set -e
echo "$play_out" | sed 's/^/  bundle  | /'
[ "$rc" -eq 0 ] && ok "bundle play exited 0 with no Vulkan" || bad "bundle play exited $rc"
grep -q "device none" <<<"$play_out" && ok "play asked for a device and got none (degraded to simulation)" \
    || bad "play did not report 'device none'"
same_digest "$(digest_of "$play_out")" "$ref_120" && ok "play digest == in-tree dedicated digest $ref_120" \
    || bad "play digest '$(digest_of "$play_out")' != '$ref_120'"

# ── F1. No content: the run must fail and say where it looked ─────────────────────────────────
step "F1: delete the bundle's content/ — the run must fail, naming every path it tried"
rm -rf "$bundle/content"
for where in sandbox host; do
    set +e
    if [ "$where" = sandbox ]; then
        f1_out="$(in_sandbox "$game" dedicated --ticks 120 --autopilot 2>&1)"; rc=$?
    else
        f1_out="$("$game" dedicated --ticks 120 --autopilot 2>&1)"; rc=$?
    fi
    set -e
    echo "$f1_out" | sed "s/^/  $where | /"
    if [ "$rc" -eq 1 ] && grep -q "no content root found" <<<"$f1_out" \
        && grep -q "$bundle/content: missing" <<<"$f1_out" \
        && grep -q "in-tree fallback .*not considered" <<<"$f1_out"; then
        ok "[$where] exit 1, both candidates named, the source tree NOT used"
    else
        bad "[$where] a bundle without content did not fail as required (exit $rc)"
    fi
done

# ── F2. A wrong digest must be rejected ───────────────────────────────────────────────────────
step "F2: the digest comparison rejects a wrong digest"
wrong="$(printf '%016x' $(( 0x${ref_120} ^ 1 )))"
if same_digest "$wrong" "$ref_120"; then
    bad "the comparison accepted $wrong for $ref_120"
else
    ok "rejected $wrong (one bit off $ref_120)"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "export-proof: PASS"
else
    echo "export-proof: FAILED ($failures)" >&2
    exit 1
fi
