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
- `install.sh` — run from a developer machine; streams a `git archive` of
  `HEAD` into CT 122 and drives the two steps below over `pct exec`. Builds
  nothing locally: this workstation's glibc (2.44) is newer than CT 122's
  (Ubuntu 24.04, 2.39), and a binary linked against the newer one refuses to
  start against the older one (measured 2026-09-29).
- `build-in-ct.sh` — runs inside CT 122 as `dev`. Builds `rime-gateway` +
  `rime-turn-proxy` (release, `--features auth`) and the `the_block_host`
  CMake target (`the-block-host`, samples/99-the-block — the hosted front
  end's first game), then stages them under `/srv/dev/deploy/stage-<sha>/`
  laid out exactly as `apply.sh` installs them.
- `apply.sh` — runs inside CT 122 as root, from the stage dir. Creates the
  `rime` user, installs files (including each game under `games/`), writes a
  default `catalogue.conf` naming `the-block` if none exists yet, creates the
  shared TURN secret, enables units, and starts/restarts services.
- `prereqs.sh` — run ONCE inside CT 122 as root, before the first deploy.
  Installs `caddy` (from Caddy's own repo, not Ubuntu's 2.6.2) and `coturn`
  and un-disables coturn's distro default. Not part of the deploy pipeline:
  `apply.sh` only checks these exist and refuses to run if one is missing, so
  an ordinary deploy never touches apt.

## Order

1. Once, before the first deploy: run `prereqs.sh` inside CT 122 as root.
2. Run `install.sh` from a developer machine; it streams the source into
   CT 122, builds it there as `dev`, stages the result, then runs `apply.sh`
   as root — which creates the `rime` user, installs files, creates the
   shared TURN secret, enables units, and starts/restarts services.
3. `catalogue.conf` in `/etc/rime/` is written with one `the-block` entry on
   the first deploy, if it does not already exist; an operator's later edits
   to it are never overwritten.
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
- **Packages in CT 122**: `caddy coturn nftables iproute2 openssl`, installed once by `prereqs.sh`
  (`apply.sh` only checks they exist and stops if one is missing). CT 122 also needs a native
  toolchain for the C++ half (cmake, g++, ninja, ~dev/.conan2, ~dev/.rime-tools) and `~dev/.cargo` for
  the Rust half — both already present as of 2026-09-29 (the perf-run checkout at `/srv/dev/rime`
  uses the same toolchain; `install.sh` never touches that checkout).
