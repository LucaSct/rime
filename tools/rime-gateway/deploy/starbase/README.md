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
