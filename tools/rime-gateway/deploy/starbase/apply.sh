#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Runs inside CT 122 as root (install.sh runs it straight from the stage dir build-in-ct.sh just
# produced, as `dev`). Safe to run again.
set -euo pipefail
stage="$(cd "$(dirname "$0")" && pwd)"

# Service user
# Prerequisites, checked up front: a missing one used to surface half-way through as an unrelated
# failure (a chown to a group coturn's package creates), leaving a half-installed host. caddy and
# coturn come from prereqs.sh (run once, by hand); this script never touches apt.
for tool in caddy turnserver nft ip openssl; do
  command -v "$tool" >/dev/null || { echo "apply: '$tool' is not installed — run prereqs.sh first" >&2; exit 1; }
done
getent group turnserver >/dev/null || { echo "apply: group 'turnserver' missing (is coturn installed?)" >&2; exit 1; }

if ! id -u rime >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin -d /var/lib/rime -m rime
fi

install -d -m 755 /opt/rime/bin /opt/rime/web /opt/rime/games /etc/rime /var/lib/rime /etc/coturn /etc/caddy

# Binaries and helper scripts
install -m 755 "$stage/bin/"* /opt/rime/bin/
install -m 644 "$stage/deploy/turn-egress.nft" /opt/rime/bin/turn-egress.nft

# The game(s). Each is a directory under games/, copied wholesale so a game that grows runtime assets
# (cooked content, a manifest) is installed the same way its binary is, with no per-file list here to
# fall out of sync.
if [ -d "$stage/games" ]; then
  for game in "$stage/games/"*/; do
    name="$(basename "$game")"
    rm -rf "/opt/rime/games/$name"
    cp -a "$game" "/opt/rime/games/$name"
    chmod -R a+rX "/opt/rime/games/$name"
  done
fi

# Static web assets (the front end is a separate, still-landing brick — absent is not an error here).
if [ -d "$stage/web" ]; then
  cp -a "$stage/web/"* /opt/rime/web/
fi

# Caddy configuration
install -m 644 "$stage/deploy/Caddyfile" /etc/caddy/Caddyfile

# Rime configuration. First deploy writes a default catalogue naming the-block; an operator who has
# since hand-edited it is never overwritten — this only ever fires when the file does not exist yet.
if [ ! -f /etc/rime/catalogue.conf ]; then
  cat > /etc/rime/catalogue.conf <<'EOF'
[the-block]
title = The Block
program = /opt/rime/games/the-block/the-block-host
args = --viewport
surfaces = play
EOF
  echo "apply: wrote a default /etc/rime/catalogue.conf (the-block only)"
fi

# Shared TURN secret: created once, read by both the gateway and coturn.
if [ ! -f /etc/rime/turn-secret ]; then
  openssl rand -base64 48 > /etc/rime/turn-secret
  chown rime:turnserver /etc/rime/turn-secret
  chmod 640 /etc/rime/turn-secret
fi

# coturn configuration: substitute the real secret into the placeholder.
install -m 640 -o root -g turnserver "$stage/deploy/turnserver.conf" /etc/coturn/turnserver.conf
sed -i "s|__TURN_SECRET__|$(cat /etc/rime/turn-secret)|" /etc/coturn/turnserver.conf

# systemd units
install -m 644 "$stage/deploy/"*.service "$stage/deploy/"*.path /etc/systemd/system/

systemctl daemon-reload
systemctl enable -q rime-media-routing rime-gateway rime-turn-proxy coturn caddy sync-turn-cert.path
systemctl restart rime-media-routing rime-gateway rime-turn-proxy coturn caddy
systemctl start sync-turn-cert.path

sleep 1
for unit in rime-media-routing rime-gateway rime-turn-proxy coturn caddy; do
  printf '%-28s %s\n' "$unit" "$(systemctl is-active $unit)"
done
