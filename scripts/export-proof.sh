#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# export-proof.sh — m20.2 (ADR-0076-m20.2): M20's done criterion, run on this machine.
#
#   scripts/export-proof.sh [--preset release|dev] [--no-build] [--keep] [--out-dir NEW-DIR]
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
#   F3. change content bytes and remove a required notice — trusted verification must reject both.
#
# Linux only (bubblewrap). Without bwrap it stops and says so rather than running a weaker proof
# under the same name.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"

preset="release"; build_flag=(); keep=0; out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="${2:?}"; shift 2 ;;
        --no-build) build_flag=(--no-build); shift ;;
        --keep) keep=1; shift ;;
        --out-dir) out="${2:?--out-dir needs a new directory}"; keep=1; shift 2 ;;
        *) echo "usage: scripts/export-proof.sh [--preset release|dev] [--no-build] [--keep] [--out-dir NEW-DIR]" >&2; exit 2 ;;
    esac
done

failures=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*" >&2; failures=$((failures + 1)); }
step() { printf '\n── %s\n' "$*"; }

command -v bwrap >/dev/null || { echo "export-proof: bwrap (bubblewrap) not found — not run" >&2; exit 1; }

if [ -n "$out" ]; then
    [ ! -e "$out" ] || { echo "export-proof: --out-dir must name a new directory" >&2; exit 2; }
    mkdir -p "$out"
    out="$(cd "$out" && pwd)"
else
    out="$(mktemp -d /tmp/rime-export-proof.XXXXXX)"
fi
[ "$keep" -eq 1 ] || trap 'rm -rf "$out"' EXIT
case "$out/" in "$repo"/*) echo "export-proof: temp dir is inside the repo" >&2; exit 1 ;; esac

# ── 1. Export ─────────────────────────────────────────────────────────────────────────────────
step "export hello_game to $out (outside $repo)"
"$repo/scripts/export-game.sh" hello_game "$out" --preset "$preset" "${build_flag[@]}"
bundle="$out/hello_game"
game="$bundle/hello_game"
intree="$repo/build/$preset/bin/hello_game"

# Keep the TRUSTED verifier and digest outside the bundle. Mutating both manifest and payload
# can pass a self-consistency check, so negative controls must pin the original manifest bytes.
cp "$repo/scripts/game_bundle_manifest.py" "$out/trusted-verifier.py"
verifier="$out/trusted-verifier.py"
trusted_digest="$(python3 "$verifier" verify "$bundle")"
verify_bundle() { python3 "$verifier" verify "$bundle" --manifest-sha256 "$trusted_digest"; }
step "verify the entire copied payload outside the repository"
(cd / && verify_bundle) && ok "manifest, content, executable and required notices verified" \
    || bad "trusted bundle verification failed"

# The digest from a run's one-line report ("… digest 0123456789abcdef …").
digest_of() { sed -n 's/.* digest \([0-9a-f]\{16\}\).*/\1/p' <<<"$1" | tail -n 1; }
# The comparison, as a function so F2 can call it with a wrong answer.
same_digest() { [ -n "$1" ] && [ "$1" = "$2" ]; }

# ── The no-Vulkan, no-repository sandbox ──────────────────────────────────────────────────────
vk_loader="$(readlink -f "$(ldconfig -p | sed -n 's/.*libvulkan\.so\.1 (libc6,x86-64) => //p' | head -n 1)" 2>/dev/null || true)"
# Give the verified artifact a separate read-only path, even when CI stores it under HOME.
# This keeps the ORIGINAL empty-HOME assertion intact rather than exposing a cache exception.
sandbox_out="/tmp/rime-export-proof"
sandbox_bundle="$sandbox_out/hello_game"
sandbox_game="$sandbox_bundle/hello_game"
sandbox_args=(--dev-bind / / --tmpfs "$HOME" --tmpfs /tmp
              --ro-bind "$out" "$sandbox_out" --chdir /)
hidden=("$HOME (repository, build tree, Conan cache, ~/.rime-tools)")
# CI can check out under /var/lib while its account HOME is elsewhere. Hiding HOME then does
# not hide the source. Hide its parent as well, so even the source directory name is absent;
# the same test below must pass for both layouts. The artifact remains separately read-only.
case "$repo/" in
    "$HOME/"*|/tmp/*) ;;
    *)
        source_parent="$(dirname "$repo")"
        [ "$source_parent" != / ] || { echo "export-proof: source directly under / cannot be isolated this way" >&2; exit 1; }
        sandbox_args+=(--tmpfs "$source_parent")
        hidden+=("$source_parent (checkout outside HOME)")
        ;;
esac
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
in_sandbox /bin/sh -c '
    test ! -e "$1" || { echo "repository visible"; exit 1; }
    test ! -e "$2" || { echo "in-tree binary visible"; exit 1; }
    test -z "$(ls -A "$3")" || { echo "HOME not empty"; exit 1; }
    for d in /usr/share/vulkan /etc/vulkan /usr/local/share/vulkan; do
        if [ -d "$d" ] && [ -n "$(find "$d" -type f | head -n 1)" ]; then echo "$d has files"; exit 1; fi
    done
    if [ -n "$4" ] && [ -s "$4" ]; then echo "loader still readable"; exit 1; fi
    test -z "${LD_LIBRARY_PATH:-}" || { echo "LD_LIBRARY_PATH set"; exit 1; }
    test -x "$5" || { echo "bundle not visible"; exit 1; }
' check "$repo" "$intree" "$HOME" "$vk_loader" "$sandbox_game" \
  && ok "repository, build tree, HOME, ICD manifests and loader hidden; read-only bundle visible" \
  || bad "the sandbox does not hide what it claims"
in_sandbox python3 "$sandbox_out/trusted-verifier.py" verify "$sandbox_bundle" \
    --manifest-sha256 "$trusted_digest" && ok "trusted verifier runs with repository and HOME hidden" \
    || bad "verification depends on source-tree files"

echo "  not hidden: /usr/lib system libraries (libc, libstdc++, X11/Wayland client libs the binary links), /dev, /proc"

# ── 2-4. In-tree vs bundle, dedicated ─────────────────────────────────────────────────────────
for ticks in 120 2000; do
    step "dedicated --ticks $ticks --autopilot: in-tree vs bundle-in-sandbox"
    ref_out="$("$intree" dedicated --offline --ticks "$ticks" --autopilot 2>&1)" || bad "in-tree run exited $?"
    echo "$ref_out" | sed 's/^/  in-tree | /'
    grep -q "content root .*in-tree fallback" <<<"$ref_out" && ok "in-tree run used the in-tree fallback" \
        || bad "in-tree run did not say it used the in-tree fallback"

    set +e
    bun_out="$(in_sandbox "$sandbox_game" dedicated --offline --ticks "$ticks" --autopilot 2>&1)"; rc=$?
    set -e
    echo "$bun_out" | sed 's/^/  bundle  | /'
    [ "$rc" -eq 0 ] && ok "bundle dedicated exited 0" || bad "bundle dedicated exited $rc"
    grep -q "content root $sandbox_bundle/content (the content/ directory beside" <<<"$bun_out" \
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
play_out="$(in_sandbox "$sandbox_game" play --headless --ticks 120 --autopilot 2>&1)"; rc=$?
set -e
echo "$play_out" | sed 's/^/  bundle  | /'
[ "$rc" -eq 0 ] && ok "bundle play exited 0 with no Vulkan" || bad "bundle play exited $rc"
grep -q "device none" <<<"$play_out" && ok "play asked for a device and got none (degraded to simulation)" \
    || bad "play did not report 'device none'"
same_digest "$(digest_of "$play_out")" "$ref_120" && ok "play digest == in-tree dedicated digest $ref_120" \
    || bad "play digest '$(digest_of "$play_out")' != '$ref_120'"

# ── F1. No content: the run must fail and say where it looked ─────────────────────────────────
step "F1: delete the bundle's content/ — the run must fail, naming every path it tried"
mv "$bundle/content" "$out/saved-content"
for where in sandbox host; do
    set +e
    expected_content="$bundle/content"
    if [ "$where" = sandbox ]; then
        expected_content="$sandbox_bundle/content"
        f1_out="$(in_sandbox "$sandbox_game" dedicated --offline --ticks 120 --autopilot 2>&1)"; rc=$?
    else
        f1_out="$("$game" dedicated --offline --ticks 120 --autopilot 2>&1)"; rc=$?
    fi
    set -e
    echo "$f1_out" | sed "s/^/  $where | /"
    if [ "$rc" -eq 1 ] && grep -q "no content root found" <<<"$f1_out" \
        && grep -q "$expected_content: missing" <<<"$f1_out" \
        && grep -q "in-tree fallback .*not considered" <<<"$f1_out"; then
        ok "[$where] exit 1, both candidates named, the source tree NOT used"
    else
        bad "[$where] a bundle without content did not fail as required (exit $rc)"
    fi
done

mv "$out/saved-content" "$bundle/content"

# Tampering controls exercise the trusted verifier before launching the altered payload.
step "F3: changed content bytes and missing required notice must fail verification"
scene="$bundle/content/hello-game.rscene"
cp "$scene" "$out/saved-scene"
printf '\n' >> "$scene"
if verify_bundle >"$out/tampered-content.log" 2>&1; then
    bad "changed content passed verification"
else
    ok "changed content rejected before launch"
fi
mv "$out/saved-scene" "$scene"
mv "$bundle/NOTICE" "$out/saved-notice"
if verify_bundle >"$out/missing-notice.log" 2>&1; then
    bad "missing required notice passed verification"
else
    ok "missing required notice rejected before launch"
fi
mv "$out/saved-notice" "$bundle/NOTICE"
verify_bundle && ok "restored artifact verifies against the original digest" \
    || bad "restored artifact differs from verified payload"

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
    python3 - "$bundle/rime-game.toml" "$trusted_digest" "$out/proof.json" <<'PYJSON'
import json, sys, tomllib
from pathlib import Path
manifest = tomllib.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
result = {key: manifest[key] for key in ["schema_version", "game_id", "target_triple",
          "configuration", "source_revision", "source_dirty", "content_sha256"]}
result.update(manifest_sha256=sys.argv[2], proof="repository-hidden-linux-export",
              dedicated_ticks=[120, 2000], no_vulkan=True,
              negative_controls=["missing-content-host", "missing-content-sandbox", "wrong-simulation-digest",
                                 "modified-content", "missing-notice"],
              clean_second_machine=False, release_candidate=False)
Path(sys.argv[3]).write_text(json.dumps(result, sort_keys=True, indent=2) + "\n", encoding="utf-8")
PYJSON
    echo "export-proof: PASS — verified artifact $bundle; evidence $out/proof.json"
else
    echo "export-proof: FAILED ($failures)" >&2
    exit 1
fi
