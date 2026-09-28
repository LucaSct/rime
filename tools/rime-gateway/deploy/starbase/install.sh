# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env bash
# Build rime-gateway and rime-turn-proxy and install them into CT 122 "rime" on the starbase.
#   tools/rime-gateway/deploy/starbase/install.sh
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [ -n "$(git status --porcelain)" ]; then
  echo "commit first: the edge runs what master holds" >&2
  exit 1
fi
cargo build --release -p rime-gateway --features auth
version=$(git rev-parse --short HEAD)
mkdir -p dist/rime-gateway
cp target/release/rime-gateway target/release/rime-turn-proxy dist/rime-gateway/
cp -r rime-gateway/web dist/rime-gateway/web
cp -r rime-gateway/deploy/starbase dist/rime-gateway/deploy
tar -czf dist/rime-gateway-bundle.tgz -C dist rime-gateway
scp -q dist/rime-gateway-bundle.tgz starbase:/root/universe-maint/rime-gateway-bundle.tgz
ssh starbase 'pct push 122 /root/universe-maint/rime-gateway-bundle.tgz /root/rime-gateway-bundle.tgz && rm /root/universe-maint/rime-gateway-bundle.tgz && pct exec 122 -- bash -s' < rime-gateway/deploy/starbase/apply.sh
echo "installed rime-gateway $version"
