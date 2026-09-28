# ADR-0052 — The browser decodes AV1 from a WebRTC video track; lossless LZ4 is a LAN-only extra

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca ("option one as standard over internet and option three as additional feature
  for LAN-only play")
- **Relates to:** [ADR-0045](0045-hosted-front-end-v1.md) decisions 1 and 3 (the wire stays AV1;
  WebRTC from the start), [ADR-0049](0049-webrtc-transport.md) (the transport seam this extends),
  [ADR-0030](0030-streaming-v1.md) §4 (codec negotiation by the client's preference list),
  [ADR-0017](0017-streaming-codec.md) (LZ4's measured ratio)

## Context

#206 built the WebRTC transport behind its seam, and it carries **one** thing: the input
DataChannel. The browser page still has to receive pixels, and there were three ways to do it:

1. the engine's AV1 bitstream sent as a **WebRTC video track**, decoded natively by the browser;
2. AV1 frames sent over the **DataChannel** and decoded in the page with WebCodecs;
3. **lossless LZ4** frames over the DataChannel, decoded by a WASM LZ4 build onto a canvas.

#200 had already made the engine able to serve either codec — `stream::choose_codec` walks the
*client's* preference list — so this is a choice about the page and the gateway, not the engine.

## Decision 1: over the internet, AV1 on a WebRTC video track

The engine's AV1 packets leave the gateway on a video track (`str0m` 0.24 ships an AV1 packetizer,
`src/packet/av1.rs`, so there is no new dependency) and the browser decodes them with its own,
usually hardware, decoder. This is the default and the only path offered to a peer that has not been
told otherwise.

**Why the track and not the DataChannel (option 2).** A video track brings WebRTC's jitter buffer,
its loss recovery and its congestion feedback with it. Frames over a *reliable* DataChannel bring
back exactly the head-of-line stall ADR-0045 decision 3 chose WebRTC to avoid; over an *unreliable*
one, we would be writing our own keyframe-recovery protocol next to the one the browser already has.

**Loss recovery reuses what exists.** A browser that loses a reference frame sends RTCP PLI/FIR. The
gateway turns that into the engine protocol's `KeyframeRequest` (`0x0103`,
`protocol.hpp:73-75`), which the encoder already honours (`video_codec.hpp:123`). No new wire
message.

**The seam grows, and stays byte-shaped.** ADR-0049's rule is that only owned buffers, opaque SDP and
connection state cross `transport.rs`'s boundary. The video path keeps it: in goes an encoded frame
(owned bytes, a keyframe flag, a capture timestamp); out comes a "the peer wants a keyframe" event.
No `Mid`, `Pt` or `MediaTime` leaves the module.

**What it costs.** Safari and older mobile stay out, as ADR-0045 already listed ("not yet,
deliberately"). The editor viewport is lossy (46.1 dB PSNR at 435 kbit/s in the ADR-0045 bench).
And `str0m`'s AV1 packetizer has never met a real browser here: Chrome interop is **untested until
the first page runs**, which is why that page is the next brick rather than a later one.

## Decision 2: lossless LZ4, for LAN play only, and switched on by the operator

A second path sends LZ4 frames over the DataChannel to a WASM LZ4 decoder in the page. It is exact
to the pixel, and it is for a LAN.

**Why LAN only — measured, not assumed.** At 1080p30, LZ4 costs **1.06 MB/s** on flat UI,
**7.67 MB/s** on a shaded scene and **42.5 MB/s** on a gradient
(`docs/design/graphics-streaming.md`, ADR-0017). AV1 is 435 kbit/s (0.054 MB/s) at 720p30. The two
were measured at different resolutions, so the ratio is indicative, not exact, but a shaded scene
over LZ4 is roughly 140× the AV1 stream, and on the starbase uplink that is one visitor starving
everyone else.

**Who decides it is a LAN.** The **operator**, with a gateway flag that is off by default — not the
page, and not a guess from the peer's address. The page can *ask* for LZ4 by putting it in its codec
preference list; the gateway removes it from that list unless the flag is set. The gateway enforces
it, not the page, for the same reason the editor band is filtered in the gateway (ADR-0049): a
policy that only the client checks is a policy any client can skip.

The peer's address cannot answer "is this a LAN?" anyway. Behind the TLS terminator that ADR-0048's
consequences require, the peer the gateway sees *is* the terminator. The flag is honest about being
an assertion by the person who knows the network.

**What it costs.** A WASM build in the page's toolchain, and a second decode path to keep working.
It lands **after** the internet path, not alongside it: an extra feature that arrives first tends
to become the one that gets tested.

## Rejected

- **AV1 over the DataChannel with WebCodecs** (option 2): gains no dependency, and costs us a hand-
  written jitter buffer and loss recovery, or else a return to head-of-line stalls.
- **LZ4 as the default:** roughly 20× to 780× the AV1 bandwidth, depending on content (the same
  cross-resolution caveat applies). Fine on a LAN,
  unusable over a home uplink.
- **Inferring "LAN" from the peer address:** wrong behind any proxy, and one proxy is already planned.

## Consequences

- `transport.rs` gains a video send path and a keyframe-request event. Its loopback test has to show
  an AV1 frame reaching a `str0m` peer through a negotiated video track, and a PLI coming back as the
  event. Like ADR-0049's run loop, this is proven with a real handshake, not by inspection.
- The signalling routes in `api.rs` stop answering 501 once there is a page to answer.
- The first browser run is the interop proof for decision 1. If Chrome rejects `str0m`'s AV1
  packetization, that is a finding about decision 1's cost, and it gets recorded here — it is not a
  quiet fallback to option 2.
