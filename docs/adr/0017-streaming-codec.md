# ADR-0017: The S0 streaming codec — JPEG on the wire, LZ4 for lossless

- Status: Accepted
- Date: 2026-07-03

## Context

Track S streams a rendered frame from the engine to a thin client (dev streaming now, the editor
viewport at M9, remote play later — [ADR-0016](0016-editor-is-a-client-of-the-engine.md)). The
transport is TCP (S0.1, [net-sockets.md](../design/net-sockets.md)); the frame tap gets pixels onto
the CPU (S0.2, `FrameStreamer`). Between them sits the **codec**, and it is not optional: raw RGBA at
1080p30 is **~250 MB/s**, a non-starter on any real link (a hosted server's ~100 Mbit uplink is
~10–12 MB/s; even a LAN is ~100 MB/s). Something has to shrink the frame an order of magnitude or two.

The plan named two candidates and required the choice be made **by measurement**, with the numbers
recorded: **JPEG** (libjpeg-turbo, BSD — SIMD-accelerated, lossy) vs **LZ4** (BSD — the fastest
lossless byte compressor). The engine *ships* this codec (games built on Rime host streamed sessions),
so the license must be Apache-2.0-safe and **GPL x264 is out** regardless of how well it would compress.

## Measurement

`samples/codec_bench` (GPU-free — it synthesizes representative frames and runs them through
`FrameEncoder`/`FrameDecoder`) on the dev server (16-core, lavapipe box), single-threaded, JPEG
quality 80 / 4:2:0. Four frame types span the content a viewport actually produces. **1080p, per
frame at 30 fps:**

| content  | codec | ratio | enc MB/s | dec MB/s | wire MB/s @30 | PSNR |
| -------- | ----- | ----: | -------: | -------: | ------------: | ---: |
| gradient | lz4   |  5.9× |      640 |     3580 |         42.5  | ∞    |
| gradient | jpeg  | 142×  |     3520 |     4750 |          1.75 | 49.5 |
| scene    | lz4   | 32.4× |     2080 |     9090 |          7.67 | ∞    |
| scene    | jpeg  | 133×  |     3740 |     5390 |          1.87 | 46.1 |
| ui       | lz4   | 234×  |     4570 |    29100 |          1.06 | ∞    |
| ui       | jpeg  | 66.8× |     3720 |     5270 |          3.73 | 48.6 |
| noise    | lz4   |  1.0× |    25100 |    64800 |        249.8  | ∞    |
| noise    | jpeg  |  6.0× |     1490 |      870 |         41.6  |  9.0 |

(Raw is 1.0× / 249 MB/s wire on every row — the baseline the plan predicted.) What the numbers say:

- **JPEG is uniformly under budget on real content** — 1.75–3.7 MB/s wire (15–30 Mbit/s) whatever the
  frame, at **46–50 dB PSNR** (visually near-lossless). That is *better* than the plan's 10–20 MB/s
  estimate. It fits a WAN link with room to spare.
- **LZ4's ratio swings with content** — superb on flat UI (234×) but only 5.9×/32× on the
  gradient/shaded-scene frames a 3-D viewport mostly *is*, i.e. **42.5 / 7.67 MB/s — over the WAN
  budget** — and 1.0× (it can even grow the frame) on noise. It cannot be the wire codec.
- Non-obvious, and the reason to measure: libjpeg-turbo's SIMD DCT **out-throughputs LZ4 on
  scene/gradient** (3.5–3.7 vs 0.6–2.1 GB/s) — LZ4 only wins on the flat UI frame and on noise (where
  it bails out fast). "Lossless is cheaper" is false here for the content that matters.

720p (the hosted-WAN target) tells the same story at ~0.44× the numbers (raw 110 MB/s; JPEG scene
1.0 MB/s / 45 dB).

## Decision

Ship **both**, behind one `Codec` enum, and use each for what it is good at:

- **JPEG (`Codec::Jpeg`) is the S0 wire codec.** It is the only candidate that fits the bandwidth
  budget *regardless of frame content*, at quality the eye barely questions. v0 defaults: **quality
  80, 4:2:0** chroma subsampling — the standard streaming trade (chroma at half resolution is nearly
  invisible on photographic/shaded content; it is where high-contrast UI *text* would show, a
  per-content choice deferred to S1). One reusable TurboJPEG handle per stream amortises setup.
- **LZ4 (`Codec::LZ4`) is the lossless / local option.** Kept not for bandwidth but because it is
  **exact** and its decode is almost free (3.5–29 GB/s) — what a local or LAN path wants when
  artifacts are unwelcome: the **M9 editor viewport** over a local socket, or loopback debugging.
- **Raw (`Codec::Raw`) is the baseline** — no compression, a uniform "encode then send" path for
  loopback and for the benchmark's reference.

The codec is a stateful `FrameEncoder`/`FrameDecoder` pair in `engine/stream` whose **public header
hides both libraries behind opaque `void*` handles**, so the thin client (S0.5) decodes with only
`rime::stream` on its include path. Both libraries are linked **PRIVATE**.

## Consequences

- **+** Streaming is feasible on a real network from day one: a 1080p scene is ~1.9 MB/s (15 Mbit/s).
- **+** The engine gains its first shipped **codecs**, both permissive (libjpeg-turbo BSD-3/IJG, lz4
  BSD-2) — Apache-2.0-safe per [third_party/README](../../third_party/README.md), recorded in
  [NOTICE](../../NOTICE). No GPL x264 anywhere near the engine.
- **+** A lossless path exists for the editor without a second decision later.
- **−** JPEG is **intra-frame only** — every frame is a full keyframe, so it uses far more bandwidth
  than an inter-frame codec (H.264/AV1) would on a mostly-static scene. Acceptable for S0 (still a
  10–140× win, dead simple, no GOP/latency coupling); **hardware inter-frame encode is S1**.
- **−** 4:2:0 softens fine coloured text; quality/subsampling are fixed, not adaptive. Both are S1
  knobs, not S0 rewrites.
- **−** Two `std::vector` allocations still occur in the naive path (encode into `out`, decode into a
  frame buffer); the S0.2 double-buffer already amortises the capture side, and codec buffers are
  reused across a stream by the caller.

## Alternatives considered

- **H.264 via x264** — the best ratio by far, but x264 is **GPL**: license-poison for a shipped
  engine (the exact trap ADR-0016 flagged). Ruled out on principle.
- **H.264/HEVC/AV1 hardware encoders** (NVENC / QuickSync / VideoToolbox / AMF) — where remote play is
  going, and inter-frame coding is a huge further win. But they need GPU/OS plumbing and a real
  latency pipeline (M5 GPU timestamps). Explicitly **S1**; JPEG is the software floor they fall back to.
- **openh264** (BSD, Cisco-licensed binary) / **SVT-AV1** (royalty-free) — viable *software* ship-safe
  inter-frame codecs, but both carry GOP/bitrate-control/latency machinery that is premature for a
  "keep v0 dumb" brick. Candidates for the S1 software path.
- **PNG / zlib (lossless)** — DEFLATE compresses a little better than LZ4 but is far slower to both
  encode and decode; for the lossless *local* path LZ4's speed matters more than its ratio, so LZ4 wins.
- **QOI** (fast lossless) — charming and simple, but LZ4 is faster, already ubiquitous, and we needed
  a general byte compressor anyway. No reason to add a third.
- **One codec only** — rejected: the wire wants *lossy+small*, the editor wants *lossless+exact*.
  These are genuinely different needs, and carrying both costs almost nothing (one small enum).
