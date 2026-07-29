#!/usr/bin/env bash
# Export the Conan recipes we maintain ourselves (third_party/conan-recipes/*) into the local
# Conan cache. Must run before any `conan install`, because `conan install` resolves
# requirements against the cache and the configured remotes only — and a locally-maintained
# recipe is in neither until it is exported.
#
# Right now that is libsvtav1/4.2.0: Conan Center's newest SVT-AV1 recipe is 2.2.1, and we need
# 4.x (ADR-0034). See third_party/conan-recipes/libsvtav1/conanfile.py for why the recipe is a
# rewrite rather than a copy of Conan Center's.
#
# Exporting only copies the recipe into the cache — it does not build anything, so it is fast
# and idempotent, and callers run it unconditionally rather than probing the cache first.
#
# Usage:  scripts/conan-export-local.sh [path-to-conan]
# Sourced by build.sh / sdk-smoke.sh / editor-smoke.sh, which already resolved a Conan binary;
# run standalone it discovers Conan the same way they do.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

conan="${1:-}"
if [ -z "$conan" ]; then
    if command -v conan >/dev/null 2>&1; then conan="conan"
    elif [ -x "$HOME/.rime-tools/bin/conan" ]; then conan="$HOME/.rime-tools/bin/conan"
    else echo "conan-export-local.sh: conan not found — run scripts/setup.sh first" >&2; exit 1
    fi
fi

for recipe in "$repo_root"/third_party/conan-recipes/*/; do
    [ -f "$recipe/conanfile.py" ] || continue
    printf '\033[1m== conan export %s ==\033[0m\n' "$(basename "$recipe")"
    "$conan" export "$recipe"
done
