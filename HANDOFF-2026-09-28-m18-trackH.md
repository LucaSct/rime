# Handoff — 2026-09-28: M18 Track H (auth), the clock gate, and the agent bricks

Written at a pause point. Everything below is **merged**, **in review with its CI state named**, or
**in flight with its location named**. Nothing here is a plan without a place on disk.

## Where things are

**Merged into `main`**, in order: **#206** WebRTC transport (ADR-0049) · **#207** the auth store
(ADR-0048 brick 1) · **#209** `derive_world_transforms` promotion · **#208** the clock-stability gate
(ADR-0050) · **#210** the ceremonies (brick 2) · **#211** the gate (brick 3). `main` is at `143ebf1`.

**Open / queued, each one step on top of the last** (second session, 2026-09-28 afternoon):

| PR / branch | what | state |
|---|---|---|
| ~~#212~~ account endpoints | Windows fix `f4e1956`; macOS flake was pre-existing (fixed in #214) | **merged** |
| **#217** `m18/track-H-auth-limits` | the rate limiter | rebased onto main (own diff hash-identical), first real CI running |
| `m18/track-H-gateway-binary` | `serve.rs` + binary | reviewed, reworked; rebased onto #217 (diff identical); **PR opens when #217 lands** |
| ~~#213~~ ADR-0052 | Luca's decode-path decision | **merged** |
| **#215** `m18/adr-0053-media-ingress` | ADR-0053: 3 UDP ports via blackStar + TURN/TLS now (Luca); SNI-on-`turns:` **verified** in Chromium 151 + Firefox 154; corrected to two host candidates (str0m ICE-lite rejects srflx, measured) | CI running |
| **#216** `m18/track-H-media-ports` | `media.rs`: port-lease pool + public address from DNS (codex draft, reviewed) | CI running |
| `m18/track-H-media-advertise` | `accept_offer_at`: public host candidate; NAT loopback test | pushed, **stacked on #214**; PR after #214 lands |
| blackstar `udp-media-forward` (local, no remote) | DNAT + metered forward chain, no UDP auto-ban | **committed `0ecbea4`, not merged to master, not deployed**. codex draft stopped on MY spec error (accept-established before meters); fixed with reply-direction accept + an order test that fails on the old order; real-kernel test passes. **Needs host-side PVE firewall changes (DESIGN.md)** |
| **#214** `m18/track-H-video-track` | AV1 video send path in `transport.rs` | drafted by codex `gpt-6-sol`, **reviewed and fixed** (ICE-lite reports `Completed`, never `Connected` — the draft's track was never writable; its sandbox could not bind UDP so its tests never ran); all gates 0; CI running |

**#212's Windows failure** was a real defect, not a flake: the PR added `webauthn-rs` +
`webauthn-authenticator-rs` as *unconditional* dev-dependencies, and `webauthn-rs` → `openssl-sys`
(ADR-0051), so `cargo test --workspace --exclude rime-auth` compiled OpenSSL on Windows. Now
`[target.'cfg(unix)'.dev-dependencies]` — their only users are `auth_api`'s unix-only tests. Proven
with `cargo tree --target x86_64-pc-windows-msvc`: openssl-sys 4 → 0, workspace-wide 0. **Any new
dev-dependency that reaches webauthn must be unix-gated the same way.**

Worktrees: `scratchpad/wt-{cer,gate,routes,limits,bin}`. `wt-cer` and `wt-gate` are landed and can be
removed. They share one `CARGO_TARGET_DIR` at `scratchpad/shared-target` — see the `/tmp` note below.

### Landing the rest of the stack

`main` is **not** branch-protected, so the `brick-delivery` shortcut applies and was used for #211:
after the parent squash-merges,

```bash
git rebase --onto origin/main <the branch's ACTUAL old parent> <branch>
git diff <old-tip> HEAD      # MUST be empty — an identical tree cannot test differently
git push --force-with-lease origin <branch>
```

and the branch can be merged on the CI it already has. **Take that old-parent SHA from `git log`, not
from memory** — I passed a rebased tip once, git replayed the parent's commits and reported a conflict
that did not exist. `#212` and `m18/track-H-auth-limits` have never been through CI, so they get real
runs, not the shortcut.

## ADR-0048 is complete through brick 3

1. **The store** (#207) — accounts, invitations, credentials, sessions, in an append-only CRC'd log.
2. **The ceremonies** (#210) — `ceremony.rs` (both WebAuthn ceremonies, JSON in and out so no
   `webauthn-rs` type reaches the gateway), `codes.rs` (HMAC'd eight-digit codes, rejection-sampled),
   `mail.rs` (plain SMTP to the estate's local postfix, which already holds the Resend key, so this
   crate holds none), `flow.rs` (the ordering rule: invitation → code mailed **to the address the
   invitation names** → passkey; login mails nothing; recovery needs **both** proofs in one call).
3. **The gate** (#211) — `identity.rs` gives the gateway its own `Principal`/`AccountRef`;
   `principal_of` is the single place a request becomes a person; every slot records an owner fixed at
   admission; somebody else's session is **404, not 403**; the 501 signalling routes check ownership
   *before* the 501; `require_account` defaults to true; per-account cap of 2, refusing with 429.

Beyond the ADR's three bricks: **#212** serves those flows over HTTP (small commands are flat JSON, a
signed ceremony response is the **entire body** of a URL that already names the ceremony), and
`m18/track-H-auth-limits` bounds that surface (20/min per client, 120/min globally, checked before any
parsing; the bucket map is capped and refuses rather than grows).

**Per-address rate limits are deliberately absent** and the module says why: a 429 keyed on an email is
an oracle for whether that address has an account, and an address is something anyone can assert, so
the limit becomes a way to lock a person out of their own recovery.

## The gateway binary — what review changed

The codex agent died with the first session (process gone, empty report) but left a near-complete,
uncommitted draft. Its catalogue parser, flags and eight tests were sound. Review found the **spec
itself** wrong on concurrency, and fixed it (commit `545b064`):

- **Single-threaded was a DoS.** `http.rs` bounds size, not time; one silent TCP connection stalled the
  whole gateway (login page included). Now: bounded thread per connection (64), a **whole-request
  deadline** re-derived before every read (a per-read timeout lets a trickler hold a thread for hours),
  routing under one `Mutex`. Three socket tests, each falsified against the flaw it targets.
- `accept` errors and a peer reset before `peer_addr()` no longer end `listen`; a poisoned router lock
  does (the supervisor restarts clean). Deadline miss → **408**.
- The binary **refuses to start** when accounts are required but not configured (it used to serve 401
  to everyone forever). `ServerConfig::socket_dir` removed (dead — the launcher owns it).
- **Known gap, recorded in a comment:** the limiter keys on the peer IP; behind the planned TLS
  terminator the peer *is* the terminator, so the TLS brick must pass the client address through or
  the per-client limit collapses into the global one. **The estate already has the mechanism**
  (`~/projects/blackstar/DESIGN.md` §2): blackStar prepends **PROXY v2** carrying the visitor's
  address, and each backend's Caddy accepts it **only from `10.77.0.1`**. So the TLS brick is the Atlas
  pattern — Caddy in CT 122 (PROXY v2 from 10.77.0.1 only, terminates TLS) → gateway on loopback with
  a forwarded-address header the gateway trusts **only from loopback**.

## Media ingress — deployment state (2026-09-28, measured on starbase)

Addresses (read live: `pct config` + `ip addr`): CT 113 blackstar eth0 **192.168.178.50** (DHCP), eth1
**10.77.0.1**; CT 122 rime eth0 **192.168.178.60** (DHCP), eth1 **10.77.0.22**.

- **FRITZ!Box: done by Luca** — UDP 50000–50002 → 192.168.178.50 (port sharing, *not* exposed host).
  Still advised: make 192.168.178.50 a fixed DHCP lease.
- **PVE firewall: done by Claude**, backups in `starbase:/root/universe-maint/fw-backup-2026-09-28-rime-media/`:
  113.fw `IN ACCEPT -i net0 -p udp -dport 50000:50002`; 113.fw `OUT ACCEPT -i net1 -dest 10.77.0.22 -p udp
  -dport 50000:50002` (placed BEFORE `OUT DROP -dest 10.0.0.0/8`, which would otherwise eat the forward);
  122.fw `IN ACCEPT -i net1 -p udp -dport 50000:50002` (any source — DNAT keeps the visitor address).
  `pve-firewall compile` exit 0; verified live in iptables on veth113i0/veth113i1/veth122i1.
- **Not yet:** deploy blackStar branch `udp-media-forward` (DNAT + metered forward chain); CT 122 policy
  routing so replies from :50000–50002 leave via 10.77.0.1, not eth0 192.168.178.60; the gateway itself.
  Until blackStar is deployed the rules are inert (its nftables still drops the UDP).
- CT 122 has **no** HTTPS-from-10.77.0.1 rule yet (earlier chat claimed it had; it did not) — add it with
  the TLS/Caddy brick.

## What is left after that

1. **The browser page**, and wiring the signalling routes to `transport.rs`. **Decided (ADR-0052, Luca):**
   AV1 on a WebRTC video track is the internet default (str0m's AV1 packetizer; PLI → the engine's
   existing `KeyframeRequest` 0x0103); lossless LZ4 over the DataChannel + WASM decoder is a **LAN-only
   extra behind an operator flag the gateway enforces** — lands after the internet path. Order: video
   track in `transport.rs` (#214) → signalling routes stop 501-ing → the page (first Chrome run is
   the interop proof) → LZ4/LAN.
2. **UDP media ingress — open, astra consulted, decision is Luca's.** blackStar admits TCP 80/443 only
   (FRITZ!Box forwards nothing else), so WebRTC media has no way in (ADR-0049 l.76). See the consult
   at `<this session's scratchpad>/ingress-out.md` and whatever Luca decided (record it as an ADR).
3. **A TLS terminator in the container.** ADR-0048's consequences correct `http.rs`'s claim that TLS
   lives in blackStar: blackStar peeks SNI and passes PROXY v2 through **without decrypting**, so Rime
   terminates its own TLS.
4. **Deploy at `rime.peekstar.eu`** — Luca, 2026-09-28: *"when the time has come and auth is done you can
   wire it together with rime.peekstar.eu"*. Standing authorization, conditional on the auth stack
   (#212 → limits → binary) being merged and the TLS terminator existing.
5. **The starbase bar.** Nothing blocks it any more — ADR-0050 replaced the unmeetable clock-pinned
   precondition with a stability gate this hardware can pass. It needs one `perf.sh` run in CT 122 that
   passes that gate, and `docs/perf/m18.3d-starbase/README.md` still says it is not that run.

## Decisions taken this session, and where they live

- **ADR-0050** — a perf report's precondition is clock **stability measured under load** (graphics
  spread ≤2% of median), not clock **pinning**, which Pascal cannot do and which the old pre-run idle
  check could not have detected anyway. Luca's amendment, with the measured evidence and the narrowness
  of that evidence both stated.
- **ADR-0051** — `rime-auth` is **not built on Windows**: `webauthn-rs-core` declares `openssl-sys`
  unconditionally and the runner has none. Recorded as a *narrowed guardrail rather than a fix*, with
  the exposure named and **vendoring OpenSSL as the recorded fallback** — implemented and green across
  40 tests before being reverted, so it is known to work and is one line.

## Conventions this session had to learn the hard way

- **codex's `workspace-write` sandbox denies UDP `bind`** (and git's index is read-only there, so it
  cannot commit). Any brick whose tests open sockets must have its tests run by me outside the sandbox;
  an agent report of "tests could not run" is not a pass.

- **Judge a build by its exit status** — and by the exit status of *the command CI runs*. A local
  `cargo clippy --workspace --all-targets` exits 0 on an unused import; CI runs
  `cargo clippy --all-targets -- -D warnings`, where it is an error. That reddened #211. Recorded in
  the `brick-delivery` skill. Also: `cmake --build … | tail -5; echo $?` reports *tail's* status.
- **A falsification that passes means the test was not testing the property.** The rate limiter's
  carried-remainder test asked at +9 and +10 seconds against a ten-second token and passed identically
  with the carry deleted, because a refill that grants nothing returns before touching the clock.
  Rewritten to +19 and +20, it bites.
- **A sampler that runs for a process's whole lifetime is not measuring the workload.** Read a clock
  under load or not at all (ADR-0050).
- `find engine tests` for clang-format — adding `samples` reformats 23 untouched files.
- `.ps1` files are **ASCII-only** and the lint job greps for it; there is no PowerShell on this box, so
  a `.ps1` change cannot be verified locally at all (`scripts/CLAUDE.md`).
- **One `CARGO_TARGET_DIR` for a stack of worktrees.** `/tmp` is a 16 GB tmpfs; three worktrees of this
  workspace held ~11 GB between them and filled it, which surfaces as `No space left on device` in the
  middle of an unrelated command.
  **But a shared target can hand you a STALE binary** (hit 2026-09-28, `wt-media`): cargo's dep-info
  records source paths *relative to the workspace*, so after switching worktrees it compares the
  *new* worktree's files' mtimes against the old output, and a file the old build never listed
  (`media.rs`) cannot trigger a rebuild at all. The gates "passed" on the other worktree's code.
  **Before building in a different worktree: `find tools -name '*.rs' -exec touch {} +`**, and
  confirm with `cargo test -- --list` that the tests you expect are actually in the binary.
## Update 2026-09-29 (morning)

- **Merged since the table above:** #214 video track, #215 ADR-0053, #216 media ports, #217 limits,
  #218 keyframe bit (protocol v4), #219 gateway binary, #220 public-address advertising, #221 TURN +
  PROXY v2, **#222 starbase deployment files** (config only — nothing deployed; its units already name
  the `--media-*`/`--turn-*` flags that the relay brick adds).
- **A reboot wiped `/tmp`** and with it the relay and page tmux sessions, their worktrees and the
  contract. Specs recovered from the 2026-09-28 session transcript; **everything now lives on disk in
  `/home/next/projects/rime-wt/`** (`contract.md`, `{relay,page}-spec.md`, `session-*.md`,
  `launch-*.sh`, worktrees `wt-relay`/`wt-page`, `shared-target`). Reports land at
  `rime-wt/{relay,page}-report.md`.
- **Relaunched on current `main`:** `m18/track-H-relay` → Opus 5 (tmux `rime-relay`; spec now also
  wires `/ice` to `turn.rs` via `--turn-uri`/`--turn-secret-file`); `m18/track-H-page` → Sonnet 5
  (tmux `rime-page`).
- **Routing until 2026-10-05 (Luca):** codex is out; devpass has ~$11 total, premium consults only,
  tight prompts; implementation goes to Sonnet 5 / Opus 5 sessions by difficulty.
