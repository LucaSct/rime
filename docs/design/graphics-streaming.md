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
| Versioned protocol | `engine/stream` — `ProtocolConnection` (grows editor message types at M6/M9) | **S0.4 ✅** |
| Server endpoint + headless + windowed client | `samples/04-remote-view` (built *on* Rime) | **S0.5 ✅ · S0.7 ◐** (live WAN numbers pending) |

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
- **S0.4 — protocol v0 ✅.** Versioned, length-prefixed frame + input messages over TCP (the editor
  rides this at M9). Detailed below.
- **S0.5 — the endpoint ✅.** `samples/04-remote-view`, one binary, three roles: a **headless server**
  (render off-screen → tap → encode → stream, applying the input the client sends back), a **headless
  client** (`client --headless`: script input, receive + decode frames, report / write PPMs), and the
  **windowed client** (below). The first two run without a *display* on a lavapipe box — the full
  see-and-control loop, verified.
- **S0.6 — proof.** *Engine-side loopback ✅* — `tests/stream/loopback_stream_test.cpp` streams a
  real lavapipe-rendered frame through the whole pipe (tap → encode → protocol → decode) over
  `127.0.0.1` and checks it arrives bit-exact; runs in CI (the GPU-free codec/protocol tests join it).
- **S0.7 — S0 close-out ◐.** The **windowed client** (`client --window`) presents each decoded frame
  as a full-window textured quad through a swapchain (the 02-textured-quad present path, with a
  *dynamic* texture re-uploaded per frame) and forwards real window key/mouse/scroll events as
  `InputEvent`s. A single-process `--selftest` presents synthetic frames — the **windowed smoke** wired
  into CTest as `remote_view_window_smoke`, which **skips gracefully with no display** and runs for
  real under a virtual display (the Linux CI build job starts **Xvfb** so lavapipe presents to an
  X11 surface — closing the gap that had let the windowed path go untested, where a window-teardown
  crash lived undetected). Verified here under Xvfb: the full **server → windowed client** loopback
  presents live-decoded frames (ASan-clean). *Remaining (the only display-gated piece):* the live
  **cross-machine** session from the Mac — windowed client over WAN to the server on the Linux box —
  with glass-to-glass latency measured and recorded (720p30). That is the last unchecked S0 clause.

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

## The protocol (S0.4)

`protocol.hpp` defines how encoded frames go *down* and input events come *back*, framed over the
S0.1 TCP sockets. This is the seam that **outlives S0**: the M9 editor viewport rides this exact
protocol (ADR-0016), so it is **versioned from day one** and its message space is meant to grow
(editor edit/inspect messages), never to be renumbered.

Wire shape — all integers **little-endian** (chosen once so the bytes are identical on every
compiler/CPU; we serialize field-by-field, never memcpy a struct):

```
Handshake (once per side, right after connect):  [ magic:u32 = "RMS1" ][ version:u16 ]
Every message after that:                        [ type:u16 ][ length:u32 ][ payload:length ]
```

- **`FrameMessage`** (server → client): `[ seq | capture_us | codec | format | w | h | data… ]`.
  Everything the peer needs to decode: the `Codec` and an `ImageDesc`, then the encoded bytes. `seq`
  is for gap detection; `capture_us` is a monotonic stamp for latency (exact on loopback — S0.6).
- **`InputEvent`** (client → server): one tagged struct — key up/down, pointer move/button/scroll —
  so the server's event-injection loop (S0.5) stays a simple switch. Pixel format rides a **stable
  wire code**, independent of `rhi::Format`'s internal numbering, so renumbering that enum never
  breaks the wire.
- **`ProtocolConnection`** owns one `TcpSocket`, does the `handshake()` (rejects a wrong
  magic/version peer rather than misparsing it), and `send_message`/`recv_message` frame the
  envelope. Receiving trusts nothing: a length over `kMaxMessageBytes` (64 MiB) is refused, and a
  short/truncated read is a clean failure (`recv_exact`), not a misread.

v0 is deliberately dumb: **blocking, length-prefixed, one message at a time.** Non-blocking /
multiplexed I/O, compression negotiation, and TLS are later (S1–S2); the versioned header is what lets
them arrive without breaking the editor.

Proof: `tests/stream/protocol_test.cpp` — **GPU-free**. Message serialization round-trips (including
signed and float fields) and rejects truncated / unknown-codec / unknown-format payloads; a **loopback
integration test** over `127.0.0.1` shakes hands, sends input up, streams a JPEG frame down, and
decodes it back to pixels — codec + protocol + sockets, end to end, no device.

## S1 — inter-frame video, async readback, the latency ledger (in progress)

The bridge from S0's per-frame JPEG stills to a real video stream, and the **M9 editor's runway**
([ADR-0030](../adr/0030-streaming-v1.md)). Five bricks:

- **s1.1 async readback — landed.** S0's `capture()` is a synchronous glass-to-CPU stall
  (`CaptureStats::last_ms` measured it). s1.1 gives the RHI a non-blocking `submit()` returning a
  `SubmitTicket` + `is_complete()`/`wait()` (the Vulkan backend already fences frames-in-flight; S0
  exposed only `submit_blocking`/`wait_idle`) and turns the tap's double-buffer into a 3-slot readback
  ring: `begin_capture()` submits without waiting, `try_get_frame()` returns the newest completed frame
  and drops older ones (**latest-wins** — bounded latency over a backlog). *Before/after on this box
  (lavapipe, 256×256, 200 frames): the per-frame capture hot path fell from `capture()` ≈ 0.035 ms
  (submit + fence-wait + readback) to `begin_capture()` ≈ 0.009 ms (submit only) — ~4×, with the
  fence-wait + copy-out moved off the frame's critical path. The absolute stall is small on lavapipe's
  fast software copy; on real GPUs with a frame genuinely in flight the hidden wait is larger, which is
  the win the M9 viewport and remote play actually cash.* `samples/04-remote-view --serve` now drives
  the async pair; the contract is *drain the tap before freeing a captured-from texture* (an in-flight
  copy still reads it).
- **s1.2 the AV1 codec.** `Codec::Av1` (appended; JPEG stays the intra fallback, LZ4 the lossless/local
  path): **SVT-AV1** to encode, **dav1d** to decode, behind `VideoEncoder`/`VideoDecoder` seams so
  hardware encoders (VideoToolbox / VAAPI / NVENC) slot in per-platform. AV1 because it is royalty-free
  and ship-safe — H.264 rides the MPEG-LA patent pool, the same class of trap that ruled out GPL x264
  ([ADR-0017](../adr/0017-streaming-codec.md)). The confirming `codec_bench` numbers land here.
- **s1.3 input v2 + the latency ledger.** Timestamped input; per-frame stamps at seven stages
  (capture-submit → present); one-way delay from echoed timestamps (no NTP); reported in the client HUD.
- **s1.4 the local fast path.** UDS (POSIX) / named pipes (Windows) behind the socket seam, LZ4-lossless
  default — **the editor viewport's transport** and the one hard M9 gate.

The protocol grows (and `kProtocolVersion` bumps): codec negotiation in the handshake, a parameter-set
message (the AV1 sequence header), and a keyframe-request message (the S2 loss-recovery seam).

## Later (S2/S3)

**S2:** internet-grade — QUIC or WebRTC (congestion control, FEC/NACK), session/auth, NAT traversal, a
browser client (WebCodecs); its transport decision converges with M11.0 (one `engine/net` core, two
consumers). **S3:** remote-play productization — multi-session, bitrate ladders, frame pacing vs. the
game loop. One-to-many broadcast (Twitch-style, no backchannel) is a natural later *profile* of the same
tap+encode stack — flagged optional, scheduled only on request.
