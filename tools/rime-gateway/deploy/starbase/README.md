# SPDX-License-Identifier: Apache-2.0
# Rime hosted front-end deployment (CT 122 "rime")

These files deploy the Rime gateway and its TURN/media plumbing into the
Proxmox container `rime` (CT 122) on the starbase.

## Files

- `Caddyfile` — reverse proxy + static site for `https://rime.peekstar.eu`,
  PROXY v2 listener wrappers on 443 and 80, and certificate management for
  `turn.rime.peekstar.eu`.
- `rime-gateway.service` — the gateway process on loopback :8787.
- `rime-turn-proxy.service` — strips PROXY v2 from blackStar and forwards
  TURN TLS to coturn.
- `turnserver.conf` — coturn TLS listener on 127.0.0.1:5350, relay restricted
  to the gateway's media address.
- `sync-turn-cert.sh`, `.path`, `.service` — copies Caddy's TURN certificate
  to `/etc/coturn/` whenever Caddy renews it.
- `media-routing.sh`, `rime-media-routing.service` — policy routes replies
  from UDP 50000-50002 back through blackStar.
- `install.sh` / `apply.sh` — build, bundle, push to CT 122 and install.

## Order

1. Run `install.sh` from a developer machine; it builds and pushes the bundle.
2. Inside CT 122, `apply.sh` creates the `rime` user, installs files, creates
   the shared TURN secret, enables units, and starts/restarts services.
3. Place `catalogue.conf` in `/etc/rime/` if it is not bundled.
4. Caddy obtains both certificates; the path unit then copies the TURN cert
   to coturn and reloads it.

## Outside this directory (needed before the first deploy)

- **blackStar routes** (`~/projects/blackstar/deploy/blackstar.json`):
  `"rime.peekstar.eu": {"tls": "10.77.0.22:443", "http": "10.77.0.22:80"}` and
  `"turn.rime.peekstar.eu": {"tls": "10.77.0.22:5349", "http": "10.77.0.22:80"}`. TURN's TLS goes to
  `rime-turn-proxy`, not Caddy, so its certificate can ONLY come via HTTP-01 on port 80 (TLS-ALPN-01
  would land at coturn) — which is why the turn route needs its `http` backend.
- **CT 122's PVE firewall** (`net1`, from 10.77.0.1 only): TCP 443, 80, 5349. UDP 50000–50002 from any
  source is already in place (2026-09-28).
- **DNS**: A records for `rime.peekstar.eu` and `turn.rime.peekstar.eu` → the home address.
- **Packages in CT 122**: `caddy coturn nftables iproute2 openssl` (`apply.sh` checks and stops if one is
  missing), and `/etc/rime/catalogue.conf` written by the operator.
