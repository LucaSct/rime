# SPDX-License-Identifier: Apache-2.0
#!/bin/bash
# Runs inside CT 122 as root (install.sh pushes it with the bundle). Safe to run again.
set -euo pipefail
bundle=/root/rime-gateway-bundle.tgz
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -C "$work" -xzf "$bundle"

# Service user
# Prerequisites, checked up front: a missing one used to surface half-way through as an unrelated
# failure (a chown to a group coturn's package creates), leaving a half-installed host.
for tool in caddy turnserver nft ip openssl; do
  command -v "$tool" >/dev/null || { echo "apply: '$tool' is not installed (apt install caddy coturn nftables iproute2 openssl)" >&2; exit 1; }
done
getent group turnserver >/dev/null || { echo "apply: group 'turnserver' missing (is coturn installed?)" >&2; exit 1; }

if ! id -u rime >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin -d /var/lib/rime -m rime
fi

install -d -m 755 /opt/rime/bin /opt/rime/web /etc/rime /var/lib/rime /etc/coturn /etc/caddy

# Binaries and helper scripts
install -m 755 "$work/rime-gateway/rime-gateway" /opt/rime/bin/rime-gateway
install -m 755 "$work/rime-gateway/rime-turn-proxy" /opt/rime/bin/rime-turn-proxy
install -m 755 "$work/rime-gateway/deploy/"*.sh /opt/rime/bin/
install -m 644 "$work/rime-gateway/deploy/turn-egress.nft" /opt/rime/bin/turn-egress.nft

# Static web assets
cp -a "$work/rime-gateway/web/"* /opt/rime/web/

# Caddy configuration
install -m 644 "$work/rime-gateway/deploy/Caddyfile" /etc/caddy/Caddyfile

# Rime configuration (operator-supplied catalogue.conf may be added here)
# The catalogue is the operator's (which games this host serves); it is not bundled. Say so now rather
# than let the gateway refuse to start without a word from this script.
[ -f /etc/rime/catalogue.conf ] || echo "apply: WARNING /etc/rime/catalogue.conf is missing; rime-gateway will not start until it exists" >&2

# Shared TURN secret: created once, read by both the gateway and coturn.
if [ ! -f /etc/rime/turn-secret ]; then
  openssl rand -base64 48 > /etc/rime/turn-secret
  chown rime:turnserver /etc/rime/turn-secret
  chmod 640 /etc/rime/turn-secret
fi

# coturn configuration: substitute the real secret into the placeholder.
install -m 640 -o root -g turnserver "$work/rime-gateway/deploy/turnserver.conf" /etc/coturn/turnserver.conf
sed -i "s|__TURN_SECRET__|$(cat /etc/rime/turn-secret)|" /etc/coturn/turnserver.conf

# systemd units
install -m 644 "$work/rime-gateway/deploy/"*.service "$work/rime-gateway/deploy/"*.path /etc/systemd/system/

systemctl daemon-reload
systemctl enable -q rime-media-routing rime-gateway rime-turn-proxy coturn caddy sync-turn-cert.path
systemctl restart rime-media-routing rime-gateway rime-turn-proxy coturn caddy
systemctl start sync-turn-cert.path

sleep 1
for unit in rime-media-routing rime-gateway rime-turn-proxy coturn caddy; do
  printf '%-28s %s\n' "$unit" "$(systemctl is-active $unit)"
done

rm -f "$bundle"
