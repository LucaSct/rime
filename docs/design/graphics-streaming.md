# Graphics streaming (Track S)

*Design note for Rime's graphics-streaming track. Track S makes "the engine renders here, you see and
control it there" a first-class, shippable capability. This doc is the map; each brick has its own
code + comments. Strategy and the parallel-path rationale live in
[ADR-0016](../adr/0016-editor-is-a-client-of-the-engine.md); the milestone view is in
[ROADMAP.md](../ROADMAP.md) (Track S).*

## Why, and the one architecture

One pipeline serves three consumers at rising transport sophistication (ADR-0016):

```
   engine renders  ──►  capture (frame tap)  ──►  encode  ──►  transport  ──►  thin client presents
        ▲                                                                            │
        └──────────────────────────  input / edits  ◄───────────────────────────────┘
```

| Consumer | Transport / codec | When |
| --- | --- | --- |
| Dev streaming (headless server → dev machine) | TCP over LAN/VPN, cheap compression | **S0, now** |
| Editor viewport (M9) | local socket / shared memory, near-zero latency | M9 |
| Remote play — a shippable game feature | QUIC/WebRTC, hardware codecs | S1–S3 |

Building it once, behind interfaces, is why the editor (M9) becomes *assembly, not invention*: the
protocol is already proven, the viewport toolkit already graduated from the ICEM viewer.

## Where the pieces live

| Piece | Home | Brick |
| --- | --- | --- |
| TCP transport primitive | `engine/platform` sockets ([net-sockets.md](net-sockets.md)) → `engine/net` at M11 | **S0.1 ✅** |
| Frame tap (capture) | `engine/stream` — `FrameStreamer` | **S0.2 ✅** |
| Codec | `engine/stream` | S0.3 |
| Versioned protocol | `engine/stream` (grows editor message types at M6/M9) | S0.4 |
| Thin client | `samples/04-remote-view` (built *on* Rime) → an `apps/` home later | S0.5 |

`engine/stream` is a **removable** feature module that depends only on the RHI *interface* and the
platform transport — never a graphics backend — so it captures from Vulkan today and any future
backend unchanged. It is meant to **ship**: games built on Rime host streamed sessions through it (the
UE Pixel Streaming analog), which is why codec choices must stay Apache-2.0-safe (hardware encoders /
openh264 / royalty-free AV1 — never GPL x264 in the engine).

## S0 brick ladder

- **S0.1 — sockets seam ✅.** Blocking TCP behind the platform seam (BSD/Winsock). See
  [net-sockets.md](net-sockets.md).
- **S0.2 — frame tap ✅** (this module). Detailed below.
- **S0.3 — codec v0.** JPEG (libjpeg-turbo, BSD) vs LZ4 on raw RGBA — **decide by measurement**,
  record the numbers. Budget: raw 1080p30 ≈ 250 MB/s (no-go on WiFi); JPEG ≈ 10–20 MB/s (fine on LAN).
- **S0.4 — protocol v0.** Length-prefixed frame + input messages over TCP, **versioned header from
  day one** (the editor rides this at M9). Keep v0 dumb.
- **S0.5 — the client.** `samples/04-remote-view`: a thin Rime-built client (platform window + an RHI
  textured quad presenting decoded frames, plus keyboard/mouse → backchannel). macOS/MoltenVK first;
  runs anywhere Rime runs.
- **S0.6 — proof.** Run a headless sample on the Linux server, view + control it live from the Mac;
  measure and record glass-to-glass latency. CI: codec/protocol unit tests (GPU-free) + a loopback
  integration test on lavapipe frames.

## The frame tap (S0.2)

`FrameStreamer` (`rime/stream/frame_streamer.hpp`) turns a rendered GPU texture into CPU-side pixels
ready to encode. The capture is the same three RHI calls every pixel-proof test already uses:

```
copy_texture_to_buffer(color, readback)   // record: GPU texture → host-visible buffer
submit_blocking(cmd)                        // submit and WAIT (the v0 stall)
read_buffer(readback, cpu, bytes)           // read the host-visible bytes
```

Design choices, and why:

- **Decoupled from the renderer.** The app renders into a color texture (created with
  `TextureUsage::TransferSrc`) and *hands it to* `capture()`. The tap does not know or care how the
  frame was drawn — which is exactly how it will tap the editor viewport later.
- **Double-buffered.** Two readback buffers + two CPU staging buffers, alternated per frame, so a
  consumer (encoder/socket) can read frame N while frame N+1 is captured. A `FrameView` is valid
  across exactly one following `capture()`.
- **Synchronous, and measured — not pre-optimized.** v0 submits the copy and waits, so `capture()`
  stalls the pipeline. That is a deliberate, house-rule choice: the S0 bottleneck is encode +
  network, not this copy, so we **measure** the stall (`CaptureStats`) instead of engineering it away
  now. True asynchronous readback (a fence + a ring of in-flight copies, no wait) folds in at **S1**,
  behind this same interface, and `stats()` will prove the win.
- **RGBA8 only, for now.** The offscreen color target and the S0 codecs speak 8-bit RGBA/BGRA;
  other formats are rejected until a codec needs them.

Proof: `tests/stream/frame_streamer_test.cpp` renders a known clear colour off-screen, captures it,
and checks the CPU pixels + that consecutive captures land in independent buffers (double buffering).
GPU-gated (lavapipe in CI), like the RHI proofs.

## Later (post-M5)

**S1:** hardware encode (VAAPI/NVENC server-side, VideoToolbox decode on Mac; software fallback), real
async readback (needs M5's GPU timestamp queries for a true latency pipeline), gamepad backchannel.
**S2:** internet-grade — QUIC or WebRTC (congestion control, FEC/NACK), session/auth, NAT traversal, a
browser client (WebCodecs). **S3:** remote-play productization — multi-session, bitrate ladders, frame
pacing vs. the game loop. One-to-many broadcast (Twitch-style, no backchannel) is a natural later
*profile* of the same tap+encode stack — flagged optional, scheduled only on request.
