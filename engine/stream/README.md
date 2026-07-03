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
| S0.3 | codec v0 (JPEG vs LZ4 on RGBA — decide by measurement) | planned |
| S0.4 | versioned length-prefixed frame + input protocol | planned |
| S0.5 | the thin client (`samples/04-remote-view`) | planned |

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

## Layout

```
include/rime/stream/   # public interface (no backend types)
  frame_streamer.hpp
src/                   # implementation
  frame_streamer.cpp
```

Proof: `tests/stream/frame_streamer_test.cpp` — render a known clear colour off-screen, capture it,
verify the CPU pixels and that consecutive captures land in independent buffers. Needs a device, so it
runs on lavapipe in CI (GPU-free-skippable, like the RHI proofs).
