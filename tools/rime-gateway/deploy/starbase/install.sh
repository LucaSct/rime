# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env bash
# Stream rime-gateway's source into CT 122 "rime" on the starbase, build it THERE, and install it.
#   tools/rime-gateway/deploy/starbase/install.sh
#
# Why nothing is built here any more: this workstation is CachyOS (glibc 2.44); CT 122 is Ubuntu
# 24.04 (glibc 2.39). A binary linked against the newer glibc refuses to start against the older one
# (measured 2026-09-29) — glibc symbol versioning is backwards- but not forwards-compatible. So the
# only correct build is one that runs on the machine that will run the binary.
set -euo pipefail

repo_root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$repo_root"
if [ -n "$(git status --porcelain)" ]; then
  echo "commit first: the edge runs what master holds" >&2
  exit 1
fi

sha=$(git rev-parse --short HEAD)
remote_src="/srv/dev/deploy/src-$sha"
remote_build_script="$remote_src/tools/rime-gateway/deploy/starbase/build-in-ct.sh"
remote_stage="/srv/dev/deploy/stage-$sha"

echo "== streaming git archive $sha to CT 122:$remote_src =="
ssh starbase "pct exec 122 -- runuser -u dev -- mkdir -p '$remote_src'"
# `git archive` walks the committed tree, not the working directory, so the dirty-tree check above
# is what makes this an accurate copy of what `git status` just called clean.
git archive HEAD | ssh starbase "pct exec 122 -- runuser -u dev -- tar -C '$remote_src' -x"

echo "== build-in-ct.sh (as dev, in CT 122) =="
# `bash` explicitly, not relying on the shebang: this repo's convention is the SPDX line FIRST and
# `#!/usr/bin/env bash` second, which is correct for every other invocation here (always `bash
# script.sh` or piped into `bash -s`) but means the file does not start with `#!` — a direct exec
# falls back to /bin/sh (dash on this container), which rejects `set -o pipefail` outright.
ssh starbase "pct exec 122 -- runuser -u dev -- bash '$remote_build_script'"

echo "== apply.sh (as root, in CT 122) =="
ssh starbase "pct exec 122 -- bash '$remote_stage/apply.sh'"

echo "installed rime-gateway $sha"
