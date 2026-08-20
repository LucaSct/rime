# 12-networked-destruction — Milestone 11's "done when"

A dedicated headless server runs the canonical destruction simulation. Two clients connect, observe,
and **see the same wall break** — the same parts dead, the same islands detached, the same debris
composition — while receiving *different* bytes, because each client's relevancy set is its own. It
closes M11 the way [`10-destructible-wall`](../10-destructible-wall) closed M8: the milestone ends in
a runnable proof, not a compile.

## The tension this sample had to resolve first

The brick was specified as *"two clients **over loopback**"* and, in the same sentence, *"hash-verified
**in CI**, deterministic, scripted loss, never environment luck."* Those pull in opposite directions.
Real loopback UDP brings port binding, process orchestration, and a scheduler that behaves
differently on every CI runner. But a proof that never touches a socket is not a milestone proof of
networked destruction — it is a 51st unit test, and M11 already has fifty.

The engine already answered this, one layer down. `net::Link` is the datagram seam, with **`UdpLink`**
(real `platform::UdpSocket`) and **`ScriptedNetwork`** (in-process, virtual clock, seeded xorshift64)
as its two implementations. So the peers are written once, against `Link`, and the transport is a
flag. Nothing about the server, the clients, the shooter, or the assertions changes between them.

**What each variant can claim is not the same, and the sample says so rather than blurring it:**

| | `--transport=scripted` (default, CI) | `--transport=udp` (real sockets) |
|---|---|---|
| clock | virtual — nothing moves without `advance_time` | the OS's |
| loss / latency | **inputs**: specified, and asserted to have happened | whatever loopback does (≈ none) |
| what it proves | the peers agree **and** the exact packet economy that got them there | the same code survives a real socket, real MTU, real `recvfrom` |
| what it cannot prove | that a real socket works | anything exact — no counter is reproducible |

CI gates on the scripted run. The UDP run is a **convergence smoke**: same scenario, same assertions
about *agreement*, none about counts. Both ship, because each covers the other's blind spot — the
scripted run cannot see a socket bug, and the UDP run cannot see a packet-economy regression.

## What the hash proves, and when it is sampled

`destruction_net::shared_state_hash(world, map, destruction)` is the cross-peer witness: per-part
alive bits and health plus debris composition, walked in NetId order. Three properties make it the
right question here, and each is load-bearing:

- **It is exactly what must match despite relevancy differing.** The two clients sit at different
  viewpoints, so m11.5 culls different debris transforms for each. The hash covers *composition* —
  which parts died, what debris exists — which is **derived**, never sent. So the sample can demand
  **exact equality** from two clients that provably received different bytes. That is a much stronger
  statement than "two identical clients agree", and it is only available because composition is
  derived.
- **It deliberately excludes transforms.** Debris positions are corrected on a tolerance, not
  per-tick, and float trajectories are not cross-platform. A hash folding them would fail for a
  correct engine.
- **It is compared peer-to-peer, never against a golden constant.** A checked-in expected hash would
  make this proof a cross-platform float-determinism test, which is precisely the claim this project
  has already recorded as false ("same-binary determinism is not cross-platform"). The server's op
  list depends on contact impulses; those may legitimately differ between compilers. What may not
  differ is what the peers in *one run* agree on.

**When.** The peers are not in lockstep — two server ticks can land in one client tick — so comparing
per-tick across peers is meaningless and would fail against a correct engine. The hash is sampled at
**quiescence barriers**: the scripted match runs with overlapping in-flight traffic, and at a few
checkpoints the sample drives the network until every channel is idle and every client has acked
through the server's latest tick, *then* compares. Barriers localize a divergence to the shot that
caused it instead of only reporting that the end state differs.

## Scale, and what would make this proof stop being one

Too small is the quiet failure. Numbers are chosen so the M11 machinery is *exercised* rather than
trivially satisfied, and each one is asserted rather than hoped for:

| dimension | why it is that big |
|---|---|
| wall parts | enough that fracture yields debris in the hundreds — below that the byte budget never binds and m11.5 is dead code in this run |
| **two viewpoints, far apart** | if both clients saw everything, relevancy would be untested and the "different bytes, same hash" claim would be vacuous |
| tick count | long enough that debris settles *and* that a delta tick follows an entry tick, so the m11.4b/m11.6 paths both run |
| scripted loss | non-zero, so the reliable-ordered op path actually has to retransmit |

## How this proof is allowed to fail

The failure mode that matters is not a crash — it is **passing while the system is broken**. This
repo has shipped proofs that could not fail against the bug they named (m11.6c's own
session-sharing test ran on a lossless link and would have passed with the bug in place). So every
way this run could be vacuously green is itself an assertion:

- **the wall actually broke** — debris count and dead parts above a floor, or two intact walls hash
  equal and prove nothing;
- **loss actually happened** — `ScriptedNetwork::packets_dropped() > 0`; a "lossy" proof that dropped
  nothing is not one;
- **relevancy actually culled** — `entities_culled_irrelevant() > 0`, and the two clients' received
  byte counts **differ**;
- **the budget actually bit** — `entities_dropped_over_budget()` / `multipart_ticks()` non-zero;
- **nothing was quietly discarded** — `malformed_messages() == 0` on both clients.

And a **negative control**: the same run with one client's op stream deliberately corrupted must make
the hashes **mismatch**. Without it, "the hashes agree" is a claim that two numbers are equal, which
two empty worlds also satisfy.

## What this sample deliberately does not do

- **No player controller and no weapon.** The damage source is a server-side scripted match — timed
  shots at named parts (ADR-0033 **A6**) — because a controller is M12 scope and inventing one here
  would be guessing at M12's design. The clients' input rides the m11.6c path for **proof-of-flow
  only**: it is sent, deduplicated and acked, and nothing consumes it, because there is nothing to
  consume it yet.
- **No late join.** Every client connects before the match starts, and the sample asserts it. Late
  join needs baseline snapshots, which M11 named as a deferred fast-follow; a client joining mid-match
  would diverge, and the proof must not accidentally depend on that path being absent.
- **No transform quantization, no lag compensation, no clock offset.** All named M11 fast-follows.
  The tick tag is an ordering key, not a schedule (A11).

## Run it

```bash
# The deterministic proof (what CI gates on):
build/dev/bin/networked_destruction --headless [--cooked <dir>]

# The same match over real loopback UDP sockets:
build/dev/bin/networked_destruction --headless --transport=udp
```

Both are **GPU-free** — cooked geometry in, physics bodies and packets out — so they run on every CI
OS plus ASan/UBSan and TSan.
