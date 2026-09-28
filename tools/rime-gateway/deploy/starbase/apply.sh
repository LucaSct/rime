# SPDX-License-Identifier: Apache-2.0
#!/bin/bash
# Runs inside CT 122 as root (install.sh pushes it with the bundle). Safe to run again.
set -euo pipefail
bundle=/root/rime-gateway-bundle.tgz
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -C "$work" -xzf "$bundle"

# Service user
if ! id -u rime >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin -d /var/lib/rime -m rime
fi

install -d -m 755 /opt/rime/bin /opt/rime/web /etc/rime /var/lib/rime /etc/coturn /etc/caddy

# Binaries and helper scripts
install -m 755 "$work/rime-gateway/rime-gateway" /opt/rime/bin/rime-gateway
install -m 755 "$work/rime-gateway/rime-turn-proxy" /opt/rime/bin/rime-turn-proxy
install -m 755 "$work/rime-gateway/deploy/"*.sh /opt/rime/bin/

# Static web assets
cp -a "$work/rime-gateway/web/"* /opt/rime/web/

# Caddy configuration
install -m 644 "$work/rime-gateway/deploy/Caddyfile" /etc/caddy/Caddyfile

# Rime configuration (operator-supplied catalogue.conf may be added here)
install -m 640 -o rime -g rime "$work/rime-gateway/deploy/catalogue.conf" /etc/rime/catalogue.conf 2>/dev/null || true

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
