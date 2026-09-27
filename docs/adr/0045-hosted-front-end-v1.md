# ADR-0045: A hosted browser front end becomes committed scope; the wire stays AV1

- Status: Accepted
- Date: 2026-09-27

## Context

[`docs/design/hosted-rime.md`](../design/hosted-rime.md) (2026-09-25, landed as #191) planned a
browser page that is a GUI for Rime, with the engine running on a server. It was explicit about its
own status — *"plan, not scope. Nothing here is committed work"* — and about what would end that
status: *"an ADR is due when the first slice becomes committed scope."*

Luca asked for it to be built (2026-09-27), so this is that ADR. It records three decisions, two of
them Luca's and one of them a measurement that **removes** the blocker the plan named.

The plan's own sequencing put *play* first (§Sequencing step 1, "one browser tab sees the block")
and deferred the editor to step 2, on the grounds that the editor is the cheap tenant. Luca chose
both surfaces in the first deliverable, and WebRTC from the start. Those choices are recorded below
with what they cost, because both were taken against the cheaper recommendation.

## Decision 1: the wire stays AV1. ADR-0030 is NOT amended, and there is no `Codec::H264`

`hosted-rime.md` called this the plan's sharpest finding: the wire is AV1-only
(`frame_codec.hpp:44-49`), `video_codec.hpp:40` asserts the wire carries `Codec::Av1` "regardless of
which encoder produced the bits", and **Ampere has no AV1 encode** (it arrives with Ada). It
concluded that "software SVT-AV1 competes for the cores the simulation needs", that a `Codec` value
for H.264 was therefore *required*, and that this "needs its own ADR amending ADR-0030's codec
choice for the hosted path".

**That conclusion does not survive measurement.** `samples/codec_bench` already measures exactly this
and nobody had run it for this question. Release build, RTX 3060 box, 1280x720 at 30 fps, `scene`
content (which the bench documents as *harder* than a mostly-static editor viewport):

| content | preset | cores | encode ms/frame | wire | PSNR |
|---|---|---|---|---|---|
| scene | 12 | **1** | **2.49** | 435 kbit/s | 46.1 dB |
| scene | 12 | multi | 2.77 | 435 kbit/s | 46.1 dB |
| scene | 10 | multi | 2.89 | 371 kbit/s | 46.8 dB |

Against a 33.3 ms real-time frame, pinned to **one** core, software SVT-AV1 costs 2.49 ms — a 7.5%
duty cycle on one core. It does not compete for the cores the simulation needs at the 720p30 tier the
plan itself identified as the honest launch tier.

So the decision is to **change nothing**: no new wire value, no ADR-0030 amendment, no NVIDIA Video
Codec SDK dependency, and no reversal of ADR-0030 §1's licensing position. That position is the real
stake here and is worth restating, because "add an H.264 enum value" reads like a small additive
change and is not one: `video_codec.hpp:20` chose AV1 because "H.264/HEVC ride patent pools that
would muddy shipping Rime under Apache-2.0 — the same trap that ruled out GPL x264 in ADR-0017".
Adding H.264 to the wire would reverse a project-wide licensing choice in order to solve a
performance problem that measurement says does not exist.

**What this costs.** Browser-side AV1 decode in WebRTC is newer and less universally available than
H.264, so the compatibility matrix is narrower — Safari and older mobile are already listed as "not
yet, deliberately" in the plan, and this decision keeps them there. **What it buys:** the licensing
rationale stays intact, the hosted path adds no third-party SDK, and the encoder behind
`VideoEncoder` remains the same one CI exercises.

**What is explicitly NOT decided:** this measurement is synthetic content and excludes the GPU-to-CPU
capture that precedes encode. The capture cost is measured against the real viewport before the play
tier is advertised, not before the first tab renders. If hardware encode is ever wanted for headroom
rather than for licensing, NVENC slots in behind `VideoEncoder` with no wire change, exactly as
`video_codec.hpp:37-40` already reserves.

## Decision 2: both surfaces in the first deliverable, landed as a stack (Luca, 2026-09-27)

The first deliverable is **one tab that can both design and play** — the editor viewport over the
existing `0x02xx` editor band and the streamed game, not one then the other.

**What this costs**, stated plainly because the recommendation was the other way: it is roughly four
bricks of work behind a single user-visible milestone, and the play half forces the 720p30 tier
conversation immediately, because `docs/perf/2026-09-22-99-the-block-…json` has `frame` p95 at
**27.456 ms** at 1080p *before* capture and encode. **What it buys:** the thing Luca actually asked
for arrives whole rather than as a stream with no authoring, which is the half that would make a
visitor call it a game engine.

**The reviewability constraint is not waived.** A brick is what one PR can be reviewed as, so this
deliverable lands as a stack of small PRs under one umbrella, not one unreviewable commit. The
deliverable is the boundary; the brick is still the unit of review.

## Decision 3: WebRTC from the start (Luca, 2026-09-27)

Transport is WebRTC with input over DataChannel, as `hosted-rime.md` proposed, rather than the
shorter WebSocket + WebCodecs path.

**What this costs:** signalling, ICE and SDP before anything renders in a browser, so the first
slice's work is mostly transport plumbing rather than engine work. **What it buys:** no transport
rewrite when the play tier ships — TCP head-of-line blocking turns one lost packet into a visible
stall and gives no honest congestion signal for adaptive bitrate, and the play half of decision 2
means that tier is in the first deliverable rather than later.

## Consequences

- `docs/design/hosted-rime.md` stops being a plan and becomes this ADR's design appendix. Its
  §"The AV1 problem" is **superseded** by decision 1 above; everything else in it stands, including
  the separate-production-machine decision, free-demo-before-credits, process-per-session, the
  containment plan, and the metering unit.
- ADR-0030 is untouched. A future ADR may still add a hardware encoder behind `VideoEncoder`; it
  would be about headroom, not about licensing or about a wire change.
- The gateway is a **separate Rust process** speaking `ProtocolConnection` to a per-session engine
  process. HTTP, TLS, auth, WebRTC and anything tenant-shaped stay out of `engine/`, which gains at
  most "accept one connection on an fd I hand you" — guardrail 2, the module must be removable.
- v1 is LAN and un-authenticated, on this workstation, with no credits and no payment code, per
  `hosted-rime.md` decision 1. No paying user before the separate production box exists.
