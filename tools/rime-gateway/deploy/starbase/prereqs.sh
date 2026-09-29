# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env bash
# Runs ONCE in CT 122, as root, before the first deploy. NOT part of the deploy pipeline on purpose:
# apply.sh only CHECKS that these packages exist and refuses to run if one is missing (see its
# prerequisite block) rather than installing them itself, so an ordinary deploy never touches apt.
#
# Ubuntu's own `caddy` package (2.6.2 on 24.04) predates the `proxy_protocol` listener wrapper the
# Caddyfile uses to accept blackStar's PROXY v2 (blackStar decrypts nothing; it peeks SNI and
# forwards the TCP stream, PROXY v2 header and all — see ADR-0053), so caddy comes from Caddy's own
# Cloudsmith repo instead, matching the estate's other containers.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "prereqs.sh: must run as root" >&2; exit 1; }

apt-get update
apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl gnupg

install -d -m 755 /usr/share/keyrings
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
    | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.asc
cat > /etc/apt/sources.list.d/caddy-stable.list <<'EOF'
deb [signed-by=/usr/share/keyrings/caddy-stable-archive-keyring.asc] https://dl.cloudsmith.io/public/caddy/stable/deb/debian any-version main
EOF

apt-get update
apt-get install -y caddy coturn nftables iproute2 openssl

# Ubuntu's coturn package ships DISABLED (TURNSERVER_ENABLED=0 in /etc/default/coturn) until an
# operator opts in; without this, apply.sh's `systemctl restart coturn` succeeds and starts nothing,
# which is a silent failure apply.sh has no way to detect (the unit is "active" either way). This is
# packaging default state, not one of the files apply.sh owns, so it belongs here rather than there.
if [ -f /etc/default/coturn ]; then
  if grep -q '^TURNSERVER_ENABLED=' /etc/default/coturn; then
    sed -i 's/^TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
  else
    echo 'TURNSERVER_ENABLED=1' >> /etc/default/coturn
  fi
fi

# Stop whatever the packages started against their own placeholder configs — apply.sh installs the
# real ones and enables/restarts both itself, so nothing should be running (or auto-starting on the
# next boot) in between.
systemctl disable --now coturn caddy 2>/dev/null || true

echo "prereqs.sh: caddy $(caddy version 2>/dev/null || echo '?'); coturn installed and pre-enabled."
echo "prereqs.sh: run install.sh from the workstation next."
