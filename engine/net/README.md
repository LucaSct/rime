# `engine/net` — networking (M11)

The engine's networking module, built brick by brick per [ADR-0033](../../docs/adr/0033-networking-v1.md)
(server authority; destruction as event-replay + state as snapshots; own transport).

## What's here

- **`link.hpp` — the Link seam.** The thinnest statement of "move a datagram": `send` one packet,
  `receive` whatever arrived. Two implementations:
  - **`UdpLink`** — real traffic over `platform::UdpSocket` (m11.1's UDP backend).
  - **`ScriptedNetwork` / `ScriptedLink`** — a deterministic in-process network with scripted
    loss, latency, reordering, and duplication on a virtual clock, so every networking proof is
    GPU-free and bit-reproducible in CI. Loss is a test *input*, never environment luck.
- **`reliable_channel.hpp` — the reliability layer.** One peer-to-peer conversation offering the
  two delivery contracts the replication model is built from:
  - **reliable-ordered** (`send_reliable`) — exactly-once, in-order; events and spawn/despawn.
  - **unreliable-sequenced** (`send_unreliable`) — at-most-once, only-if-newer; snapshots.
  Sequence + ack-bitfield + resend design, each channel in its own sequence space; time is an
  input parameter so the same code runs on wall time in a game and virtual time in tests.

## What's next (the ladder)

m11.2 sessions (handshake, heartbeat) → m11.3 replication core (NetIds, reflection snapshots) →
m11.4 networked destruction → m11.5 relevancy/budgets → m11.6 interpolation/input → m11.7 the
`samples/12-networked-destruction` proof. See [docs/ROADMAP.md](../../docs/ROADMAP.md).

## Tests

`tests/net/` — reliable delivery and exact ordering under 30% loss, stale-drop and no-resend on
the sequenced channel, duplicate suppression, and bit-reproducibility (same seed ⇒ same trace).
All GPU-free; run with `ctest -R rime_net_tests`.
