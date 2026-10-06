# Handoff — 2026-09-29: deploying rime.peekstar.eu (continues HANDOFF-2026-09-28-m18-trackH.md)

## Done today (edge + host, all MEASURED)
- DNS: `rime.peekstar.eu` and `turn.rime.peekstar.eu` → 95.89.215.226 (Luca).
- #222 (deploy files) merged.
- **blackStar `b278a54` deployed** (Luca: "now"): UDP 50000–50002 DNAT to 10.77.0.22 live in CT 113's
  nftables; routes rime → 10.77.0.22:443/80, turn.rime → 10.77.0.22:5349 (tls) / :80 (http); backend
  output now `tcp dport { 80, 443, 5349 }`. All other sites 200 after the restart (fleet 000 = its
  client-cert requirement, same before).
- PVE firewall (backups `starbase:/root/universe-maint/fw-backup-2026-09-29-rime-turn/`): 113.fw OUT
  tcp 5349 → 10.77.0.22; 122.fw IN net1 from 10.77.0.1 tcp 443, 80, 5349. Compiled, live in iptables.

## In flight (worktrees + specs + reports in /home/next/projects/rime-wt/)
| tmux | branch | model | brick |
|---|---|---|---|
| rime-relay | m18/track-H-relay | Opus 5 | signalling routes ↔ transport, /ice via TURN |
| rime-page | m18/track-H-page | Sonnet 5 | the browser page |
| rime-fwd | m18/track-H-forwarded | Sonnet 5 | client address from X-Forwarded-For (loopback only) |
| rime-dbuild | m18/track-H-deploy-build | Sonnet 5 | build natively in CT 122 (glibc 2.39 vs 2.44), ship The Block, prereqs.sh |

## Remaining before first visit
Merge the four → run `prereqs.sh` in CT 122 (caddy from Cloudsmith, coturn) → `install.sh` →
check certs (Caddy, then coturn sync) → CT 122 reply routing unit → browser test (Chrome, then Firefox).

## 2026-09-29 evening
- **#225** relay (rebased onto #223/#224; gates green locally) and **#226** deploy-build are open, CI running.
- `prereqs.sh` has been RUN in CT 122 (caddy v2.11.4, coturn, libssl-dev; both services stopped). Two fixes are on #226: the keyring name (.asc → .gpg) and a rerun-safe stale source list.
- The unit already carries `--media-*`/`--turn-*`/`--trust-forwarded-from-loopback` (from #222); no change needed.
- **Decision (Luca, 2026-09-29): `InputEvent.code` for keys = USB HID usage IDs (page 0x07).** Rejected: freezing `platform::Key` ordinals (keyboard.hpp forbids serializing them) and `KeyboardEvent.code` strings (break the fixed 37-byte payload). The ADR-0054 is to land with the input brick: branch `m18/track-H-input`, Opus 5 tmux `rime-input`, spec `rime-wt/input-spec.md`.
- Next: merge 225 + 226 → run `install.sh` from a CLEAN worktree (it refuses untracked files) → start caddy/coturn/gateway → certs → media reply routing → browser test.

## 2026-09-29 late — LIVE
- #225 relay and #226 deploy-build merged. `install.sh` has been run; the live build is from #227's branch (fd42cac). All five units are active; Let's Encrypt issued certificates for rime.peekstar.eu and turn.rime.peekstar.eu; TURN over TLS :443 verifies end to end.
- #227 (open): first-run fixes. **coturn had been running on Ubuntu's defaults**, because its unit reads /etc/turnserver.conf; the fix is a drop-in. It was stopped as soon as this was seen, and was never internet-exposed.
- The mail route was Luca's call: **copy the host's Resend key**. Done: CT 122's postfix relays through [smtp.resend.com]:465; a test to delivered@resend.dev was `sent`. The credential is not in git.
- #228 (open): `rime-gateway invite` plus an auth-store lock. The page cannot register anyone without it.
- The Windows `transport::tests::a_pli_from_the_peer_becomes_a_keyframe_request_event` failed once on #226 (a 5 s timeout) and passed on the rerun. Watch it.
- Next: merge 227 and 228 → install.sh from main → mint Luca's invitation → browser test. Then the input brick (rime-input tmux: 5 commits so far, ADR-0054).

## 2026-09-29 21:40 — invitation out
- #227 and #228 merged (their only red was a relay test race already red on main). **#229** fixes that race
  (`relay::tests::a_keyframe_request_from_the_browser_side_reaches_the_engine`: `to_engine` writes, then counts;
  the test now `rig.wait`s for the counter). Merged green 2026-09-29.
- **Deployed main 3ee09ae** via `install.sh` from a clean worktree (`rime-wt/wt-deploy`; log `rime-wt/install-3ee09ae.log`).
  All five units active; coturn listens only on 127.0.0.1:5350.
- **Invitation minted for ls@peekstar.eu** (Luca's choice, 2026-09-29), valid 7 days; code given to Luca in chat.
- Input brick: `rime-input` tmux (personal profile) hit its session limit mid-gates at 20:08; it auto-resumes at
  00:50. When `rime-wt/input-report.md` appears: review diff, clang-format, clang -fsyntax-only on new TUs, PR.

## 2026-09-29 22:30 — phone sign-in (pairing) brick queued
- Firefox on Linux can't do passkeys (no platform authenticator, no hybrid/QR). **Luca's decision:** the desktop
  shows a QR → the phone scans it and signs in with its passkey → the phone shows a code → typed into the desktop
  → the desktop gets a session. **Amends ADR-0048 Decision 1**; the new ADR-0055 lands with the brick.
- gpt-6-astra consulted ($0.30; `rime-wt/pair-consult-{prompt,out}.md`). Adopted: fresh passkey assertion on the
  phone every time, code only works in the desktop's own browser, 8 digits / 5 tries, 5-min pairing, no phone
  session as a side effect, a mail notice, and an **account-confirm step** (stops account substitution — astra's
  catch). Differences: an Origin check instead of CSRF tokens (cookies are already SameSite=Strict), 128-bit ids;
  per-session revocation deferred.
- Spec `rime-wt/pair-spec.md`; worktree `rime-wt/wt-pair` (branch `m18/track-H-pair`); Opus 5 tmux `rime-pair`
  — CANCELLED: Luca chose (22:15) to build it NOW with an Opus agent on the `code` account (week 97%). Report → `rime-wt/pair-report.md`.
- Luca registers on the PHONE with the existing invitation (unredeemed: auth.log is still one 107-byte record);
  the code was re-sent by mail to ls@peekstar.eu at his request.
- **Pairing built** (Opus 5 subagent, ~12 min) and reviewed by me (Opus 5.5): **PR #230**, branch rebased onto #229.
  Local gates exit 0. Deviations the agent made (accepted): approve is `POST /<id>/approve/<challenge>` with the raw
  assertion body; an added read-only `/<id>/describe`; desktop polls every 4 s (limiter 20/min/IP, phone+desktop
  share a home IP). Next: CI green → merge → install.sh from a clean main worktree → Luca tests the flow.

## 2026-09-30 morning — input PR, page redesign, five parallel engine agents
- **Keys/mouse dead on the live site = the input brick never landed** (rime-input tmux hit its limit mid-gates; tmux killed).
  Finished by me: rebased on 628a89b, build exit 0 + 76/76 ctest, clang -fsyntax-only on 4 new TUs exit 0,
  web check exit 0, falsified (skip `translator.translate` → rime_app_tests red). **PR #231**.
- **PR #232** (stacked on #231): full-window stream stage, Fullscreen (+Keyboard Lock on Chromium),
  "click to play" hint, redesigned page (top bar, auth cards, catalogue grid, frost tokens). Retarget to main after #231.
- Deploy after merge: install.sh from a clean main worktree (the live build is 664b285 = pair branch).
- turn.rime.peekstar.eu = coturn (TURN over TLS on 443 via blackStar :5349 route) — media fallback when UDP 50000–50002 is blocked.
- **Parallel engine agents** (Opus 5.5 subagents of the lead session; rules `rime-wt/engine-rules.md`; reports `rime-wt/<brick>-report.md`;
  worktrees `rime-wt/wt-<brick>`, no pushes): m18.4-micro-raster (GPU software raster = CPU oracle), m18.5-page-streaming
  (bounded page cache, retirement-gated), m18.6-vg-dag (replacement DAG cook — only leaves existed), m19.1-heightfield
  (terrain asset + collision, CPU-only; **starts M19 before M18 closes, at Luca's request for parallel work**),
  m20.1-game-definition (engine-owned GameDefinition, dedicated mode never creates a device).
  Measured burn at ~4 min: ~2.5M cache-read + 0.2M cache-write + 19k output tokens per agent.
- ADR numbers: agents name ADRs by brick; renumber at merge (0054 input, 0055 pair are taken).

## 2026-09-30 ~11:30 — agents done, PRs open (all reviewed by the lead; targeted tests re-run MEASURED)
| PR | branch | state |
|---|---|---|
| #230 | m18/track-H-pair | phone sign-in (ADR-0055) — open; LIVE build is from it (664b285) |
| #231 | m18/track-H-input | browser keys+mouse (ADR-0054) — CI 8/8 green |
| #232 | m18/track-H-page-design | full-window stream + redesign — stacked on #231; Windows rerun of the transport flake pending |
| #233 | m20.1-game-definition | GameDefinition + dedicated mode (ADR-0056) |
| #234 | m18.5-page-streaming | page pool/cache (ADR-0057) |
| #235 | m18.4-micro-raster | GPU micro raster + hybrid merge (ADR-0058) — **changed the CPU oracle's fill rule (was mirrored top-left); Luca to confirm** |
| #236 | m18/track-H-flaky | (another session) PLI test flake fix |
| #237 | m18.6-vg-dag | replacement-DAG cook, payload v2 (ADR-0059) |
- #234 and #235 both touch `virtual_geometry_visibility_pass` → the second to merge rebases.
- **M19.1 heightfield agent still running** (worktree `rime-wt/wt-m19.1-heightfield`, report `rime-wt/m19.1-heightfield-report.md`); its ADR must be renumbered to 0060.
- Deploy (after #230/#231/#232 merge): install.sh from a clean main worktree, then Luca browser-tests input.
