# ADR-0049 — The WebRTC implementation, and the seam that makes it replaceable

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca ("use str0m"); `gpt-6-astra` consulted and proposed the same, independently
- **Relates to:** [ADR-0045](0045-hosted-front-end-v1.md) decision 3 (WebRTC, already ratified),
  [ADR-0046](0046-exported-games-and-the-blender-boundary.md) §2 (the gateway ships in every bundle)

## Context

ADR-0045 decision 3 settled **that** the transport is WebRTC with input over a DataChannel. It did not
settle **which implementation**, and that mattered more than usual because `rime-gateway` and
`rime-protocol` had **zero third-party dependencies** by deliberate policy — `rime-protocol` is the
crate a C++ conformance fixture tests against, so the wire is fully described by this repository.

A WebRTC stack is the first real break in that policy. Which break it is also decides whether the
gateway inherits an async runtime.

## Decision: `str0m`, sans-IO, with the features cut back

Both the owner and an independent consult arrived at `str0m`. Agreement between two opinions is weak
evidence, so the reasoning matters more than the concurrence:

**Sans-IO is the load-bearing property.** `str0m` owns no sockets, spawns no threads and forces no
runtime on the caller. The gateway's HTTP server and process supervisor were **already written and
merged, blocking**. A full-stack alternative (`webrtc-rs` + tokio) would not have added a dependency
to new code — it would have forced a rewrite of two landed bricks. The cost we accept instead is that
the run loop is ours to get right.

**`default-features = false, features = ["rust-crypto"]`**, and both halves matter:

- the default set is `["aws-lc-rs", "examples"]`, and `examples` pulls in **`rouille`, an entire HTTP
  server framework**, which under ADR-0046 §2 would ship inside every exported game bundle;
- `rust-crypto` selects the pure-Rust backend, so there is no vendored C to build on three CI
  platforms.

**Licence: MIT OR Apache-2.0**, so Apache-2.0-safe under the same `third_party/` policy that admits
libjpeg-turbo (BSD-3/IJG) and lz4 (BSD-2).

**Rejected:** `webrtc-rs` (viral async runtime through already-merged blocking code), libwebrtc via FFI
(a C++ build and a second toolchain inside ours, for interop certainty we can get other ways), and
deferring to WebSocket + WebCodecs (would supersede ADR-0045 decision 3, and reinstates the TCP
head-of-line stalls that decision exists to avoid).

## The seam is the actual deliverable

`tools/rime-gateway/src/transport.rs` is the only module that knows WebRTC exists. Across its boundary
travel **owned byte buffers, SDP as opaque text, and connection state** — and nothing else. No `Rtc`,
no `Mid`, no futures, no SSRCs, no payload types, no `str0m` error type.

A replacement implementation could own an async runtime internally and nothing outside that file would
change. That is what makes this decision reversible, and it is why the seam was specified before the
crate was chosen rather than after.

**The surface policy stays out of the transport.** A play session must never carry the `0x0200..=0x02FF`
editor band, and that filter remains in `Session` on the raw wire code. The transport moves opaque
bytes and has no opinion about what they mean; giving it one would put a security decision in two
places, which is how one of them drifts.

## Consequences

- The zero-dependency policy now has exactly one documented exception in `rime-gateway`, with the
  reason recorded here. `rime-protocol` keeps none.
- The run loop is ours, including the timer contract. Getting it wrong does not fail loudly — it
  produces a connection that negotiates and then stalls, because nothing retransmits. It is therefore
  tested against a real ICE/DTLS/SCTP handshake rather than by inspection.
- **A sans-IO state machine cannot be woken**, so waking it is ours too: `submit` sends a datagram to
  the loop's own socket carrying 16 random bytes minted per transport. The mechanism is counted, not
  merely present — a zero `wakeups` count would mean the poll ceiling is carrying the load and the
  wakeup is dead code that looks alive.
- **Browser interop is not proved and cannot be** without a browser. The test proves the run loop, the
  socket plumbing, the SDP exchange and the wakeup against a conformant peer; the browser page brick is
  where interop gets exercised. Saying so is better than implying the connection test covers it.
- `check_bind`'s refusal of wildcard addresses applies to the **media socket** too, not only the HTTP
  listener. A media socket bound to `0.0.0.0` would be a side door in the same containment boundary.
- Media is UDP, and **the estate's public edge is a TCP SNI proxy**, so hosted media cannot traverse
  blackStar. That is an ingress problem, not a transport one; it is recorded here because the transport
  choice is what surfaced it.
