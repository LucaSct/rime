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

- **`control_packets.hpp` — the connectionless control codec.** ConnectRequest/Accept/Reject,
  Disconnect, Heartbeat, as a pure encode/decode pair each. No driver, no link, no clock, so it is
  unit-testable by feeding it byte spans — including every truncated prefix of every packet.
- **`session.hpp` — one peer relationship.** Its `ReliableChannel`, its connection state
  (Connecting → Connected → Closing), its liveness timers, and the inbox the game drains.
- **`net_driver.hpp` — the driver the reliability layer was written for.** Owns the endpoint→session
  routing table, polls the shared `Link` exactly once per tick, runs the handshake, reaps dead
  peers. Role-agnostic: `listen()` makes it a server, `connect()` makes it a client, both makes it a
  listen server.

### The session wire (m11.2)

Every datagram — control or channel — is framed `[salt:u32][payload...]`, so channel traffic costs
17 header bytes rather than m11.1's 13. The salt identifies the **incarnation**: when a peer dies
and reconnects from the same address, its old packets may still be in flight, and a fresh channel
restarts every sequence space at 0 — so a stale `seq 5` would look like legitimate early traffic and
be buffered into the new stream. The driver drops it before it ever reaches channel state. The first
payload byte then splits the two worlds: `< 0x80` is a `ReliableChannel` packet (byte-identical to
m11.1), `>= 0x80` is a control packet.

The handshake is **connectionless and validated before it can allocate** — protocol version, app id,
and schema hash are separate fields, compared separately, so a rejection names exactly what to fix
("server schema 0x…, client schema 0x…"). A wrong-build peer costs the server one reply datagram and
no state. See ADR-0033 amendment A7.

## What's next (the ladder)

m11.3 replication core (NetIds, reflection snapshots) →
m11.4 networked destruction → m11.5 relevancy/budgets → m11.6 interpolation/input → m11.7 the
`samples/12-networked-destruction` proof. See [docs/ROADMAP.md](../../docs/ROADMAP.md).

## Tests

`tests/net/` — reliable delivery and exact ordering under 30% loss, stale-drop and no-resend on
the sequenced channel, duplicate suppression, and bit-reproducibility (same seed ⇒ same trace).
All GPU-free; run with `ctest -R rime_net_tests`.

`rime_net_session_tests` adds the m11.2 proofs: handshake and hello exchange, schema/app-id
rejection *with no server-side allocation*, retry until someone is listening, reincarnation
replacing a stale session, graceful close, a bounded session table, garbage that poisons nothing,
and **peer death detected by timeout** — the milestone's proof. Plus one real-socket loopback case,
because the scripted harness proves the algorithm and something must prove the socket path.
