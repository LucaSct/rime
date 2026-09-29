#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copy Caddy's certificate for turn.rime.peekstar.eu into coturn's config directory.
set -euo pipefail

caddy_dir="/var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/turn.rime.peekstar.eu"
cert="$caddy_dir/turn.rime.peekstar.eu.crt"
key="$caddy_dir/turn.rime.peekstar.eu.key"
dest_dir="/etc/coturn"

if [[ ! -f "$cert" || ! -f "$key" ]]; then
    echo "Caddy certificate for turn.rime.peekstar.eu not found" >&2
    exit 1
fi

install -m 640 -o turnserver -g turnserver "$cert" "$dest_dir/turn.rime.peekstar.eu.crt"
install -m 640 -o turnserver -g turnserver "$key" "$dest_dir/turn.rime.peekstar.eu.key"

systemctl reload coturn
