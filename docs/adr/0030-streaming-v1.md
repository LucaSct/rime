# ADR-0030: Streaming v1 — inter-frame video (AV1), async readback, and the latency ledger

- Status: Accepted
- Date: 2026-07-17

## Context

Track S0 shipped the whole streaming loop — the frame tap (`FrameStreamer`, S0.2), the codec
(ADR-0017: JPEG on the wire, LZ4 lossless/local, S0.3), the versioned `RMS1` protocol (S0.4), and a
thin client (S0.5). It works, and it is the foundation for three consumers: dev streaming (done), the
**M9 editor viewport** (next), and remote play (S2/S3). S1 is the bridge from "works" to "good
enough to build an editor on and, later, to ship remote play."

Three limitations of S0 define what S1 must remove — each already *measured or documented* in the S0
code, per the "measure before optimize" rule:

1. **JPEG is intra-frame only.** Every frame is a full keyframe (ADR-0017 Consequences), so a
   mostly-static viewport pays full-frame bandwidth 30×/s even when nothing moves. On a WAN link
   that is the dominant cost. Inter-frame coding (send only what changed) is the single biggest win
   left, and ADR-0017 explicitly deferred it: *"hardware inter-frame encode is S1."*
2. **Readback is a synchronous GPU stall.** `FrameStreamer::capture()` records
   `copy_texture_to_buffer` then `submit_blocking` (submit-**and-wait**) then `read_buffer`. The RHI
   interface exposes only `submit_blocking()` and `wait_idle()` — there is no non-blocking submit and
   no completion token. `CaptureStats::last_ms` records this glass-to-CPU stall precisely *"so the day
   we make it async, we can prove it"* (`frame_streamer.hpp`). That day is S1.
3. **No honest cross-machine latency.** `FrameMessage::capture_us` is a single timestamp, *"exact on
   loopback; cross-machine latency needs clock sync — an S1 concern"* (`protocol.hpp`). There is no
   end-to-end (glass-to-glass) number, so nothing to tune remote play against.

S1 is also the **hard entry gate for M9** (ADR-0016: the editor is a client of the engine on this
protocol). Its last brick, s1.4, is the editor viewport's transport (a local UDS/named-pipe wire,
LZ4-lossless). Decision taken at kickoff (Luca, 2026-07-17): **build the full S1 track (s1.0–s1.4)
before M9**, so the editor starts on a proven wire rather than an improvised one. This ADR is s1.0 —
the decision the other four bricks cite.

Consistent with the other `.0` decision bricks (ADR-0024/0026/0029), this is a **pure decision brick:
proof is the ADR, no code**. Unlike ADR-0017 — where both candidate codecs (libjpeg-turbo, lz4) were
trivial to integrate and so were benchmarked up front — a real AV1/H.264 integration *is* the bulk of
s1.2's work. So s1.0 makes the selection on the decisive licensing-and-architecture grounds and
**defers the confirming latency bench to s1.2's integration** (which is empowered to escalate to a
hardware encoder if the software path misses budget — see §3). Recording an unmeasured lean and then
measuring it is the honest sequence when the measurement costs a milestone-brick to obtain.

## Decision

### 1. The inter-frame codec is AV1 — SVT-AV1 to encode, dav1d to decode — decided on licensing

The choice is dominated by the same axis that ruled out x264 in ADR-0017: **we ship this codec**
(games built on Rime host streamed sessions), so its license must not compromise Apache-2.0
distribution. That test is decisive here:

- **H.264 / HEVC** ride the MPEG-LA / Via Licensing **patent pools**. openh264's grant covers
  *Cisco's own* binary distribution, not a from-source build embedded in another product — it muddies
  Apache-2.0 shipping exactly the way ADR-0016 warned. Same class of trap as x264's GPL, reached by a
  different road.
- **AV1** is **royalty-free** by design (Alliance for Open Media). **SVT-AV1** (the encoder) is
  BSD-3-Clause + the AOM Patent License 1.0; **dav1d** (the decoder) is BSD-2-Clause. Both pass the
  same ship-safe test as libjpeg-turbo (BSD-3/IJG) and lz4 (BSD-2), pulled the same way (Conan;
  recorded in NOTICE per `third_party/README.md`).

Performance is the secondary axis, and it is adequate rather than free: SVT-AV1 at its low-latency
presets (8–13) is real-time-capable at 720p and workable at 1080p on this 16-core box, and it is
built for exactly this — scalable multi-core encode. The **confirming bench is s1.2's** (extend
`samples/codec_bench` with SVT-AV1 the way ADR-0017's bench measured JPEG/LZ4: GPU-free, shaded-scene
content, encode ms/frame + bitrate at target quality). AV1 also cleanly supersedes VP9 (better ratio,
the SVT-AV1/dav1d tooling is where the ecosystem's energy is), so VP9 is not considered further.

Wire-enum change (values are wire-stable — append, never renumber, per `frame_codec.hpp`):

```
enum class Codec : uint8_t { Raw = 0, LZ4 = 1, Jpeg = 2, Av1 = 3 /* new: inter-frame wire codec */ };
```

The existing three keep their jobs: **JPEG** stays the *stateless intra fallback* (no GOP, no codec
state, the dumb software floor every client can decode); **LZ4** stays the *lossless/local* codec (the
M9 editor viewport rides this over s1.4); **Raw** stays the baseline. **AV1** is the new *inter-frame
wire codec* for bandwidth-constrained (WAN) links.

### 2. Async readback (s1.1) — and the one RHI primitive it needs

The stall in §Context-2 cannot be hidden by threading alone: a worker that calls `submit_blocking`
still serializes the GPU. The honest fix is a **completion primitive the RHI does not yet expose**.

- **RHI top-up:** a non-blocking submit that returns a completion token (a lightweight fence /
  `SubmitId`) plus `poll(token)` / `wait(token)`. The Vulkan backend **already fences frames-in-flight**
  for the swapchain (M3.4) — s1.1 *exposes* that machinery through the interface, it does not invent
  it. `submit_blocking()` is preserved as the trivial `submit() + wait()` composition, so every
  existing caller (asset uploads, the M3 offscreen proofs) is unchanged.
- **The readback ring:** `FrameStreamer` grows from its 2-slot double buffer (`kSlots = 2`) to an
  N-deep ring (N = frames-in-flight + a small readback latency). `capture()` records the copy, submits
  **non-blocking**, and returns immediately with a token; a later `drain()` harvests the frames whose
  tokens have completed. The glass-to-CPU stall moves off the capture call entirely.
- **Latest-wins drop policy:** when encode or transport falls behind, in-flight frames are **dropped,
  never queued unboundedly**. A viewport wants the *newest* frame, not a backlog; bounded latency
  beats completeness for interactive streaming. `CaptureStats` gains a dropped-frame counter, and the
  s1.1 proof asserts the `last_ms` stall measured in S0 is gone (the before/after the S0 comment
  promised).

### 3. Hardware encoders are seams, not dependencies

`VideoEncoder` / `VideoDecoder` interfaces, in the RHI-backend spirit: **software SVT-AV1 + dav1d is
the reference implementation** and the CI/lavapipe path (this box has no hardware encoder — it must
stay provable GPU-free). **VideoToolbox** (Mac), **VAAPI / QuickSync**, **NVENC** slot in per-platform
behind the same interface, proven opportunistically on hardware that has them (Mac first). The
handshake negotiation (§4) selects the best encoder/decoder pair both ends support. This is also the
escape hatch for §1's performance risk: if software AV1 misses the 720p30 budget on target hardware,
s1.2 makes a hardware encoder the default where present — **no interface or protocol change**, because
the codec identity on the wire (`Codec::Av1`) is independent of which encoder produced it.

### 4. Protocol evolution — negotiation, parameter sets, keyframe request

The `RMS1` envelope already carries an unknown `type:u16` transparently and reserved room to grow
(`protocol.hpp`); S1 spends some of that room and **bumps `kProtocolVersion`** (S0 clients then fail
the handshake cleanly rather than mis-decode an AV1 stream — the version field exists for exactly
this). New Frame-family message types (distinct from the M9 editor range 0x0200–0x02FF):

- **Capabilities exchange** in/after the handshake: the client advertises the decoders it has
  (`Av1`/`Jpeg`/`LZ4`/`Raw`); the server picks the codec both support, best-first. Same-host defaults
  to LZ4 (the editor); a WAN client that advertises AV1 gets AV1.
- **`StreamConfig` (parameter-set) message:** the codec-init a stateful decoder needs *before* the
  first frame — the AV1 sequence header, the SPS/PPS analog. Sent on stream start and on
  resolution/codec change.
- **`KeyframeRequest` message (client → server):** force an intra (keyframe) — used now on
  connect/resize/decoder-reset, and the **S2 loss-recovery seam** (a dropped keyframe is
  unrecoverable without it). Fixed-bitrate + keyframe-on-request is the whole rate story for v1;
  adaptive rate control is S2.

All negotiation/codec values are wire-stable: append, never renumber.

### 5. The latency ledger (s1.3 implements)

Per-frame timestamps at **seven stages** — capture-submit, readback-complete, encode-done, wire-out,
client-recv, decode-done, present — carried on the frame (extending `FrameMessage::capture_us` into a
small stamp set). **Clock model: no NTP assumption.** One-way delay is *estimated* from echoed
timestamps: the client echoes the server's stamps back on the input backchannel, the server halves the
round trip (symmetric-path assumption, stated as such). The ledger is reported live in the client HUD
and in stats dumps. This is what turns "glass-to-glass" from a loopback-only number into an honest
cross-machine one — the instrument S2 and remote-play tuning are otherwise flying blind without.

### 6. What S1 does *not* do

Congestion control / rate adaptation / FEC / NACK (**S2** — its transport ADR converges with M11.0,
one `engine/net` core, two consumers) · multi-client sessions (**S3**) · audio (**AU** track) ·
browser / WebCodecs client (**s2.4**) · sophisticated rate control (fixed bitrate + keyframe-on-request
is v1; the ledger measures whether more is warranted) · shared-memory zero-copy readback (a documented
s1.4 seam, gated on measuring that UDS+LZ4 is actually the bottleneck first).

## Consequences

- **+** Inter-frame coding: a mostly-static viewport costs a fraction of JPEG's per-frame-keyframe
  bandwidth — the biggest single streaming win still on the table, taken **ship-safe**.
- **+** The capture stall dies *measurably* — S0 recorded the before (`CaptureStats::last_ms`), s1.1
  records the after; the double-buffer comment's promise is kept.
- **+** An honest end-to-end latency number for the first time (the ledger), the instrument remote-play
  needs.
- **+** M9 inherits all of it but **rides the LZ4-lossless local path** (s1.4), so the video codec is
  the WAN path — *off the editor's critical path*. An AV1 hiccup can never block the editor.
- **+** A clean HW-encoder story (§3) without committing to any HW dependency; CI stays GPU-free.
- **−** Two new shipped dependencies (SVT-AV1, dav1d) — both permissive and license-gated, but real
  build surface (Conan packages, NOTICE entries).
- **−** Software AV1 is CPU-heavy; hitting the real-time budget is a measured risk carried into s1.2
  (mitigated by the bench + the HW-encoder escape hatch).
- **−** The RHI gains a completion primitive — an interface change that touches every submit path;
  contained by keeping `submit_blocking` as `submit() + wait()` (zero behaviour change for existing
  callers, asserted by their untouched tests).
- **−** `kProtocolVersion` bumps: an S0 client cannot talk to an S1 server (intended; the handshake
  rejects it cleanly instead of mis-parsing).

## Alternatives considered

- **H.264 via openh264.** The decode-ubiquity argument (VideoToolbox, browsers, Android MediaCodec) is
  genuine and was weighed seriously. It loses on the shipping test: the MPEG-LA patent pool is the same
  *class* of distribution risk that ruled out x264, and openh264's binary grant does not cover a
  from-source build shipped inside Rime. Decode ubiquity is answered instead by **shipping dav1d** (BSD-2,
  small, fast) with the client — we own the decoder rather than renting a patent-encumbered one.
- **Hardware H.264 only, no software codec.** Rejected: it leaves CI/lavapipe (no HW encoder) and any
  HW-less client with nothing better than intra-JPEG, and it makes S1 un-provable GPU-free. The software
  AV1 reference gives every platform an inter-frame floor and keeps the proof headless.
- **VP9.** Royalty-free too, but AV1 supersedes it on ratio and tooling momentum (SVT-AV1/dav1d). No
  reason to adopt the older codec.
- **Keep synchronous readback, just move it to a worker thread.** A worker blocked in `submit_blocking`
  still serializes the GPU queue; the completion primitive is the real fix, and the backend already owns
  the fences — threading around the stall would be pretending to fix it.
- **Shared-memory zero-copy transport for the local (editor) path.** Premature: s1.4 first measures
  whether UDS + LZ4 is even a bottleneck at editor-viewport rates on this box. Kept as a documented seam
  with the measurement attached, not built on spec.
- **Defer the latency ledger to S2.** Rejected: you cannot tune what you cannot measure, and the
  timestamps are nearly free to carry now. Building the ledger in S1 means S2's transport work starts
  with numbers instead of guesses.
