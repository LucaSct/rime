# 13-networked-player — Milestone 12's proof ("The Player")

A headless server owns the simulation. Two clients connect, drive the **same scripted input tape**
over the **same lossy link**, and differ in exactly one thing: **client 0 predicts; client 1 does
not.**

That shape is the point. [ADR-0035](../../docs/adr/0035-vision-demo-m12.md) §1 asks for own-input
response *"≤ 1 tick, against a prediction-off control showing ≥ RTT ticks, so prediction is provably
**the reason**"* — and the cheapest way to be wrong about that is to measure two runs on two links
and compare numbers that were never comparable. Here the control is a peer in the same match: same
server, same tick, same scripted losses, same tape, one boolean apart.

## What it asserts

| clause | how it can fail |
| --- | --- |
| own-input response ≤ 1 tick | the predicting client's *drawn* pose does not move on the tick its own input was sampled |
| the control waits ≥ RTT | the non-predicting client responds sooner than the link allows — which would mean the number is measuring something other than what it claims |
| both clients converge | either client's own avatar differs from the server's **by a single bit** at quiescence |
| remote motion is continuous | the largest single drawn step for the *other* player exceeds three ticks of walking — the lurch [m12.5](../../engine/replication/README.md) removed |
| the run was not vacuous | no packets dropped, no commands lost, no corrections, no shots landed, or the wall untouched |

Every clause is checked inside the binary, which exits non-zero if any fails. The clauses live in
the program rather than in a CMake assertion nobody reads.

## What it deliberately does not prove

Frame rate, GI, scale and "feels right" are **M13's** ([ADR-0036](../../docs/adr/0036-milestone-split-player-and-block.md)).
This binary never opens a window and never touches a GPU.

Networked *destruction* is M11's, and [`12-networked-destruction`](../12-networked-destruction)
proves it. Here the wall exists **server-side only**, so that "shoot" means something and so the
m12.3 weapon→destruction glue is exercised end to end in a real program rather than only in a test.
The clients do not mirror it: their claim is about the **player**.

**A real limit, stated rather than hidden:** the clients carry the same floor the server does —
a client that cannot see the ground cannot predict standing on it — but they do **not** carry the
wall's collision. The tape therefore keeps both players well clear of it. Walking into geometry only
one side knows about would produce a correction storm that says nothing about prediction and
everything about the level being half-loaded. Giving clients the full level is a content question
that M13 answers.

## Measuring latency with no clock

There is no clock synchronisation anywhere in this engine and M12 deliberately does not add one
(ADR-0033 A11). None is needed: both players stand still until a scripted tick, so "how long until
my own input moves me" is counted in **that client's own ticks**, between two events on one machine.
A difference of two local events has no origin to agree about — the same trick ADR-0030 §5 uses for
the remote-view latency number.

## Why the input tape is data

Input is a pure function of `(player, tick)` and never anything derived from the simulation. A tape
that aimed by querying the world would re-introduce the float dependence that makes a proof
unreproducible — and, worse here, would make the two clients' tapes diverge the moment their
predictions did, which is precisely the thing under test.

## Running it

```bash
ctest --preset dev -R networked_player      # cook fixture + the gated run
build/dev/bin/networked_player --headless --cooked build/dev/samples/13-networked-player/cooked
```

`--play` is accepted and does nothing yet: the windowed client is **m13.3**. The flag is here so the
invocation does not change when it arrives.
