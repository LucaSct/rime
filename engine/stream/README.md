# engine/stream — graphics streaming (capture → encode → transport)

`rime::stream` is the engine-side of Rime's **graphics-streaming** track: it takes a frame the engine
has rendered, gets it off the GPU, (soon) encodes it, and hands it to a transport so a thin remote
client can display it and send input back. One stack, three consumers — dev streaming now, the
editor viewport at M9, and remote play later — see [ADR-0016](../../docs/adr/0016-editor-is-a-client-of-the-engine.md)
and [docs/design/graphics-streaming.md](../../docs/design/graphics-streaming.md).

It sits above the RHI in the layer cake and depends only on the **RHI interface** (`rime::rhi`),
never a backend — so it captures from the Vulkan backend today and any future D3D12/Metal backend
unchanged. It is a **removable** feature module (the engine builds without it), and is designed to be
**shippable**: games built on Rime host streamed sessions through it (the UE Pixel Streaming analog).

## Status (Track S, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| S0.2 | **frame tap** — `FrameStreamer`: copy a rendered texture to the CPU (RHI readback), double-buffered, with a measured per-frame cost | landed |
| S0.3 | **codec** — `FrameEncoder`/`FrameDecoder`: JPEG (wire) + LZ4 (lossless) + Raw, chosen by measurement ([ADR-0017](../../docs/adr/0017-streaming-codec.md)) | landed |
| S0.4 | **protocol** — `ProtocolConnection` + `FrameMessage`/`InputEvent`: a versioned, length-prefixed message stream over the S0.1 TCP sockets | landed |
| S0.5 | remote-view endpoint (`samples/04-remote-view`): headless server + headless client (see + control) | landed (windowed client pending) |

> The transport it streams over is the TCP sockets seam in `engine/platform`
> ([design](../../docs/design/net-sockets.md), S0.1); at M11 that graduates into `engine/net` and
> `engine/stream` layers on top of it.

## The frame tap (S0.2)

`FrameStreamer::create(device, extent)` pre-allocates two host-visible readback buffers.
`capture(color)` takes an already-rendered texture (created with `TextureUsage::TransferSrc`), copies
it to the CPU via the RHI's `copy_texture_to_buffer` → `submit_blocking` → `read_buffer`, and returns
a `FrameView` over the pixels — **double-buffered**, so a consumer can encode/send frame N while frame
N+1 is captured. Capture is **synchronous** in v0 (it stalls on the GPU copy); `stats()` records that
cost so we can prove the win when asynchronous readback lands (S1). It does not know how the frame was
drawn — the app renders, then hands the texture over — which keeps the tap decoupled from the renderer.

## The codec (S0.3)

`FrameEncoder`/`FrameDecoder` shrink a captured frame so it fits a network (raw 1080p30 ≈ 250 MB/s).
Three codecs behind one `Codec` enum: **Raw** (baseline), **LZ4** (lossless), **Jpeg** (lossy). The
choice was **measured**, not assumed — see [ADR-0017](../../docs/adr/0017-streaming-codec.md):

- **JPEG is the wire codec** — the only one under a WAN budget on all content (~2 MB/s for a 1080p
  scene at ~46 dB PSNR); LZ4's ratio collapses on smooth/shaded frames.
- **LZ4 is the lossless / local path** — exact and nearly-free to decode, for the M9 editor viewport.

The encoder/decoder are **stateful** (they own a reusable TurboJPEG handle, created lazily) and the
public header keeps libjpeg-turbo/lz4 **opaque** (behind `void*`), so consumers — including the thin
client (S0.5) — link the libraries transitively but never include their headers. Reproduce the
numbers with `samples/codec_bench` (GPU-free).

## The protocol (S0.4)

`ProtocolConnection` frames the codec's bytes for the wire and carries input back, over the S0.1 TCP
sockets. It is **versioned from day one** (a magic + version handshake on connect) because the M9
editor viewport rides this same protocol (ADR-0016). Wire shape — all little-endian:

```
Handshake (each side, once):  [ magic:u32 "RMS1" ][ version:u16 ]
Every message:                [ type:u16 ][ length:u32 ][ payload ]
```

`FrameMessage` (server → client) carries `seq | capture_us | codec | format | w | h | data`;
`InputEvent` (client → server) is one tagged struct for key/pointer events. Decoding **trusts
nothing** — an over-long length or a truncated read is refused, not misparsed. v0 is deliberately
dumb (blocking, one message at a time); the versioned header is what lets S1/S2 evolve it without
breaking the editor. Details in [docs/design/graphics-streaming.md](../../docs/design/graphics-streaming.md).

## Layout

```
include/rime/stream/   # public interface (no backend types, no codec-library headers)
  frame_streamer.hpp   #   S0.2 — the frame tap
  frame_codec.hpp      #   S0.3 — the codec (opaque handles)
  protocol.hpp         #   S0.4 — the versioned wire protocol
src/                   # implementation
  frame_streamer.cpp
  frame_codec.cpp      #   the only place <turbojpeg.h>/<lz4.h> are included
  protocol.cpp         #   little-endian (de)serialization + the framed connection
```

Proofs:
- `tests/stream/frame_streamer_test.cpp` — render a known clear colour off-screen, capture it, verify
  the CPU pixels and that consecutive captures land in independent buffers. Needs a device, so it runs
  on lavapipe in CI (GPU-free-skippable, like the RHI proofs).
- `tests/stream/frame_codec_test.cpp` — **GPU-free**: synthesizes frames, checks Raw/LZ4 bit-exact,
  JPEG close + small, and every malformed call refused. Runs on every CI OS, device or not.
- `tests/stream/protocol_test.cpp` — **GPU-free**: message serialization round-trips + malformed-input
  rejection, and a **loopback** test (handshake + input + a JPEG frame, end to end over `127.0.0.1`).
