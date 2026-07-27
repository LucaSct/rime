# The reliability layer — sequence spaces, the frontier ack, and the send window

*Design note for **m11.1** (ADR-0033 §3). Code: `engine/net/include/rime/net/reliable_channel.hpp` +
`src/reliable_channel.cpp`, on the `Link` seam (`engine/net/include/rime/net/link.hpp`). Proofs:
`tests/net/reliable_channel_test.cpp`.*

## What it is

UDP gives a game datagrams: maybe, out of order, maybe twice. M11 needs exactly two stronger
contracts, and this layer builds both over one socket:

- **Reliable-ordered** — every message arrives, exactly once, in send order. The destruction
  damage-op stream (ADR-0033 §2, amendment A1), spawn/despawn, and the session handshake ride
  this.
- **Unreliable-sequenced** — a message arrives at most once and only if it is *newer* than
  everything delivered so far; late ones are dropped, never resent. Snapshots ride this: stale
  state is garbage, and the next snapshot is already coming.

It is deliberately small — one message per datagram, no fragmentation (`kMaxPayload` keeps a
packet under the path MTU), u32 sequence numbers that do not wrap, a fixed resend timeout. Each
simplification is labeled in the code and is an additive seam, not a rewrite.

## Why each channel has its own sequence space

Sharing one sequence space between the two channels deadlocks: reliable-ordered delivery waits
for contiguous sequences, so if the missing sequence belonged to a lost *unreliable* packet —
which by contract is never resent — the reliable stream waits forever. Each channel numbers its
own packets; the channels interact only through the shared ack header.

## The frontier-anchored ack (the part that was earned by failing tests)

The classic scheme reports **"newest received seq + a bitfield of the 32 before it."** We built
that first; adversarial review + regression tests killed it with two reproducible bugs:

1. **The seq-0 false-ack deadlock.** A peer that has received *nothing* reports newest = 0, and
   the sender's `seq == ack` test reads that as "seq 0 acked" — the first packet is dropped from
   the resend queue while actually lost. The stream deadlocks at message 0.
2. **The unreportable late recovery.** A packet lost, then recovered *after* 32+ newer seqs
   arrived, is neither "newest" nor inside the backward bitfield — it can never be reported
   again, so the sender resends it (and burns bandwidth) forever.

The fix is to anchor the ack at the **delivery frontier** instead: `ack = deliver_seq_` ("next
expected"), with bit *i* of the bitfield reporting `received(deliver_seq_ + i)` — the buffered
out-of-order arrivals just past the frontier. Both bugs evaporate:

- `ack = 0` means "I have delivered nothing" — and clears nothing (`seq < 0` is impossible).
- The frontier never retreats, so anything ever delivered satisfies `seq < ack` *permanently* —
  late recoveries are reported forever, and the resend queue provably drains (the
  late-recovery regression test).

The receive window is exactly 32 so every seq the receiver can be holding has a bit. Widening
the window means widening the bitfield to u64 (4 more header bytes per packet); not worth it at
13-byte headers.

## The send window

The same 32 binds the sender: at most 32 transmitted-but-unacked packets in flight. Further
`sends` queue locally and `update()` pumps them as acks free budget. Without it, a fast sender
would spray hundreds of packets the receiver must drop (>32 past its frontier) and pay for each
again in resends. Backpressure beyond that: the queue caps at `kMaxPending` (256) and
`send_reliable` returns false — a dead peer must never OOM the server.

## Acks on the wire

Every packet — either channel — carries the current ack state in its 13-byte header
(`channel | seq | ack | ack_bits`), so acks flow on whichever direction has traffic. When
reliable traffic arrived and we have sent nothing for it to ride home on, `update()` emits one
header-only **AckOnly** control packet per tick. Headers are written/parsed with
`core::ByteWriter`/`ByteReader` — bounds-checked cursors, never hand-rolled parsing, because
this is the engine's first untrusted-remote-input surface.

## The driver pattern

A `ReliableChannel` never drains the `Link`. A driver owns the socket, polls once, and routes
each datagram to the channel whose peer sent it — one UDP socket serving N peers, which is what
a server *is*. m11.1's tests route by hand; m11.2's session layer is the driver.

## The deterministic proof harness

`ScriptedNetwork` replaces the internet with an in-process simulation — scripted loss, latency,
reordering (latency jitter reorders for free), and duplication — on a *virtual* clock, seeded by
a tiny xorshift64 (not `std::mt19937`: standard-library distributions are not
bit-portable). Same seed + same script ⇒ same trace, on every platform. That is what lets CI
prove "100 reliable messages, 30% loss, exact order, exactly once" — and prove the scenario was
actually lossy (the network counts drops; a 30%-loss test that dropped nothing fails its
precondition check).

## Deliberate limitations

- One message per datagram (coalescing is an m11.5+ optimization, measured first).
- No fragmentation (messages ≤ `kMaxPayload` = 1200).
- Fixed resend timeout (RTT-adaptive RTO is a measured follow-up).
- No sequence wrap (2³² packets ≈ 49 days at 1000/s; wrap arithmetic lands with sessions if
  ever needed).
