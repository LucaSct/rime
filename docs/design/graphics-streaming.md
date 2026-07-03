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
| Codec | `engine/stream` — `FrameEncoder`/`FrameDecoder` | **S0.3 ✅** |
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
- **S0.3 — codec v0 ✅.** JPEG (libjpeg-turbo) on the wire, LZ4 for the lossless path — decided by
  measurement ([ADR-0017](../adr/0017-streaming-codec.md)). Detailed below.
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

## The codec (S0.3)

`FrameEncoder`/`FrameDecoder` (`rime/stream/frame_codec.hpp`) shrink a captured frame enough to cross
a network — raw 1080p30 is ~250 MB/s, so the codec is what makes streaming feasible at all. Three
codecs behind one `Codec` enum (wire values fixed for the S0.4 protocol byte): **Raw** (baseline),
**LZ4** (lossless, BSD), **Jpeg** (lossy, libjpeg-turbo TurboJPEG API, BSD). The decision is
**measured**, not asserted — `samples/codec_bench` (GPU-free) is the harness; full numbers and the
rationale are in [ADR-0017](../adr/0017-streaming-codec.md). Headline (1080p30, JPEG q80/4:2:0):

| content | LZ4 wire MB/s | JPEG wire MB/s | JPEG PSNR |
| --- | ---: | ---: | ---: |
| shaded scene | 7.67 | **1.87** | 46 dB |
| gradient | 42.5 | **1.75** | 50 dB |
| flat UI | 1.06 | 3.73 | 49 dB |

- **JPEG is the wire codec.** The only one under a WAN budget (~10–12 MB/s) *regardless of content*,
  at near-lossless quality — LZ4's ratio collapses on the smooth/shaded frames a 3-D viewport mostly
  is (over budget), while JPEG stays ~2 MB/s. Defaults: quality 80, 4:2:0 subsampling.
- **LZ4 is the lossless/local path** — not for bandwidth (it loses there) but because it is exact and
  its decode is nearly free, which the **M9 editor viewport** (local socket, no artifacts) will want.
- Design choices, and why:
  - **Libraries stay hidden.** The public header speaks `std::byte` and opaque `void*` handles; only
    `frame_codec.cpp` includes `<turbojpeg.h>`/`<lz4.h>`. So the thin client (S0.5) decodes with only
    `rime::stream` on its include path, and the libraries link **PRIVATE**.
  - **Stateful, handle-reusing.** One `FrameEncoder` owns one TurboJPEG handle for a whole stream
    (created lazily on first JPEG frame) — real streaming, and a fair benchmark.
  - **Trusts nothing on decode.** LZ4 uses `_safe`; JPEG validates the stream's own dimensions against
    the expected `ImageDesc` before writing, so a corrupt/rogue frame can't overrun the output buffer.

Proof: `tests/stream/frame_codec_test.cpp` — **GPU-free**, so it runs on every CI OS: Raw/LZ4 are
checked bit-exact, JPEG close (small mean error) and small, and every malformed call (bad format,
size mismatch, corrupt input, dimension-lie) is refused rather than overrunning.

## Later (post-M5)

**S1:** hardware encode (VAAPI/NVENC server-side, VideoToolbox decode on Mac; software fallback), real
async readback (needs M5's GPU timestamp queries for a true latency pipeline), gamepad backchannel.
**S2:** internet-grade — QUIC or WebRTC (congestion control, FEC/NACK), session/auth, NAT traversal, a
browser client (WebCodecs). **S3:** remote-play productization — multi-session, bitrate ladders, frame
pacing vs. the game loop. One-to-many broadcast (Twitch-style, no backchannel) is a natural later
*profile* of the same tap+encode stack — flagged optional, scheduled only on request.
