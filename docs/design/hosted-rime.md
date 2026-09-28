# Hosted Rime — a browser front end, and the shape of a paid service

Status: **committed scope since 2026-09-27**, and this file is now the design appendix to
[ADR-0045](../adr/0045-hosted-front-end-v1.md) rather than a plan. It was written as "plan, not
scope", with the note that "an ADR is due when the first slice becomes committed scope"; Luca asked
for it to be built, so that ADR exists and records what changed.

> **§"The AV1 problem" below is SUPERSEDED by ADR-0045 decision 1.** Its premise — that software
> SVT-AV1 "competes for the cores the simulation needs", so the hosted path needs a `Codec` value for
> H.264 — did not survive measurement. `samples/codec_bench`, which already existed and had not been
> run for this question, puts SVT-AV1 at **2.49 ms/frame at 1280x720/30 pinned to one core** against
> a 33.3 ms budget. There is no `Codec::H264`, ADR-0030 is not amended, and its Apache-2.0 licensing
> rationale is not reversed. Everything else in this file stands.

Consulted 2026-09-25 (Opus 5.5, read-only); the seam citations below were re-verified against the
tree by the reviewer before being written down.

## The idea

`rime.peekstar.eu` — a browser page that is a GUI for Rime, with the engine itself running on a
server. Multiple users, eventually spending prepaid credits to create games and to play/stream them
interactively.

> **DNS exists as of 2026-09-27: `rime.peekstar.eu` resolves to this workstation's IP** (Luca created
> the record). **That is ahead of decision 1 below, and the gap is the dangerous part.** Decision 1
> says production runs on a separate machine and this box is for *un-authenticated LAN testing of the
> first slice only*; a public A record pointing here, plus a gateway listening on a public interface,
> is exactly the configuration decision 1 exists to prevent — this machine holds the owner's SSH
> keys, git credentials and this repository, and none of the containment below is built yet.
>
> So until the containment plan is in place: **the gateway binds to loopback or the LAN interface,
> never `0.0.0.0`**, and the DNS record is useful only for LAN testing and for having the name ready.
> A record pointing at a host is not an invitation to listen on it.

## Decisions taken (2026-09-25, Luca)

1. **Production runs on a separate machine, not this workstation.** This box is for
   un-authenticated LAN testing of the first slice only. What this costs: no paying user before
   that hardware exists. What it buys: a container escape or a GPU reset cannot reach the owner's
   SSH keys, git credentials or repository, and the dev machine's frame budget stays clean for
   milestone work. The containment plan below is what makes the second box *sufficient*, not what
   makes this box *acceptable*.
2. **Free public demo first; credits when there is a queue.** The payment stack — ledger,
   reservation, kill-on-zero, hosted checkout — is deferred, not cancelled. The service is still
   intended to cost money; it will not cost money before demand is demonstrated. What this costs:
   no revenue yet. What it buys: no prepaid-balance liability and no VAT/merchant-of-record
   obligation while the authoring surface is still one procedural building generator — which is the
   strongest argument against shipping this now, and the honest reason to wait.

## What already exists (measured)

The seam is not the RHI. It is `engine/stream` plus `engine/editorhost`, and most of it is built:

- `engine/stream/include/rime/stream/protocol.hpp:59` versioned `MessageType`; `:106`
  `FrameMessage`; `:168` `InputEvent` carrying client-clock stamps; `:205` `ProtocolConnection`.
- `engine/stream/include/rime/stream/video_codec.hpp:37-40` already reserves hardware encoders
  (NVENC named) behind the same interface.
- `engine/stream/include/rime/stream/frame_streamer.hpp:91` async `begin_capture()` / latest-wins
  `try_get_frame()`.
- `engine/stream/include/rime/stream/latency.hpp:19-27` input-to-photon measured offset-free by
  echoing the client's own stamp — the instrumentation any latency claim here must use.
- `engine/editorhost/include/rime/editorhost/editor_host.hpp:41` reserved `0x02xx` editor band;
  `:76` `message_affects_frame`, the idle-skip classifier that makes editor tenancy cheap.
- Headless rendering is real: `engine/rhi/include/rime/rhi/device.hpp:150`.
  **Corrected 2026-09-27 ([ADR-0046](../adr/0046-exported-games-and-the-blender-boundary.md) §1):**
  `rime_app_create_headless` was cited here as evidence for that and is not. The C ABI documents it
  as "no device, 60 Hz sim" (`engine/capi/include/rime/capi/rime.h:99`) — it cannot render, and
  exists to prove the FFI can spin the engine loop from another language. The off-screen render path
  is the RHI's, not the C ABI's.
- `samples/04-remote-view/main.cpp:170-186` is single-client, blocking `bind → accept()`.

## The shape

**A separate Rust gateway process.** It speaks `ProtocolConnection` to a per-session engine process
on one side and WebRTC/HTTP to the browser on the other. HTTP, TLS, auth, WebRTC and anything
tenant-shaped stay out of `engine/` — the engine gains at most "accept one connection on an fd I
hand you". Guardrail 2: the module must be removable.

**Process per session, not in-process contexts.** Otherwise one tenant's bad shader or asset takes
down every session, and there is no unit to kill or bill.

**Transport: WebRTC, H.264, input over DataChannel.** WebSocket + WebCodecs is a shorter path and is
good enough for the *editor*, but TCP head-of-line blocking turns one lost packet into a visible
stall and gives no honest congestion signal for adaptive bitrate.

**The AV1 problem (the consult's sharpest catch, verified).** The wire is AV1-only —
`frame_codec.hpp:44-49` is `Raw | LZ4 | Jpeg | Av1`, and `video_codec.hpp:40` asserts the wire
carries `Codec::Av1` "regardless of which encoder produced the bits". That is false on the intended
hardware class: **Ampere has no AV1 encode** (it arrives with Ada), so an RTX 3060's only hardware
path is NVENC H.264/HEVC. Software SVT-AV1 competes for the cores the simulation needs. A `Codec`
value for H.264 is therefore required, and being a `uint8_t` enum with room, it is additive. **This
needs its own ADR** amending ADR-0030's codec choice for the hosted path.

## The session API (v1, landed 2026-09-28)

`tools/rime-gateway/src/http.rs` + `api.rs`. **Hand-rolled HTTP/1.1 with no new dependency**, for the
reason `rime-protocol` has none: the wire is fully described by this repository. TLS and the public
hostname live in **blackStar**, the estate's edge (ADR-0047 §3), never here.

| route | method | answer |
| --- | --- | --- |
| `/api/catalogue` | `GET` | `{"games":[{"id","title","surfaces":["edit","play"]}]}` |
| `/api/sessions` | `POST` | `201` + `{"session","surface","game"}` and a `Location` header |
| `/api/sessions` | `GET` | `{"sessions":[{"session","surface"}]}`, id-ordered |
| `/api/sessions/<id>` | `GET` / `DELETE` | the one session, or `204` and it is reaped |
| `/api/sessions/<id>/offer`, `/answer` | — | `501`, naming the transport brick |

Four properties are enforced in code with a test each, not left to convention:

- **The client names a catalogue entry, never a program.** `CatalogueEntry` holds `program`/`args` and
  serialises neither, so a `POST` can only start something the operator listed. The alternative is a
  remote-execution service with a JSON front end.
- **Every length is bounded** — request line 8 KiB, headers 16 KiB *in aggregate* and 64 lines, body
  64 KiB, refused with `414`/`431`/`413` rather than truncated. Chunked encoding is `501` and wins over
  `Content-Length`, because a request carrying both is the smuggling ambiguity.
- **`0.0.0.0` is refused by `check_bind`.** v1 is un-authenticated, so which interfaces the gateway
  answers on *is* the containment boundary; a wildcard bind means a new interface exposes the service
  with no diff in this repository.
- **A failed launch hands the admission slot back.** Otherwise the cap leaks and the host reports
  itself full with nothing running — a symptom that arrives hours later with no other information.

Signalling is deliberately a refusal rather than a stub: an endpoint that answers plausibly and does
nothing is one a client would build against.

## What one box can actually serve

`docs/perf/2026-09-22-99-the-block-nvidia-geforce-rtx-3060.json` measures the flagship sample at
1080p: `frame` p50 **14.201 ms**, p95 **27.456 ms**, max **31.296 ms**. That is one session barely
holding 60 Hz and already missing at p95, *before* capture, encode and packetisation.

So the consult's §2 budget (55-75 ms input-to-photon at 1080p60) and its §3 ceiling contradict each
other, and §3 is the one to believe. **The honest launch tier is 720p30, or editor-only** — the
editor being cheap precisely because of `message_affects_frame`. Advertise that, not 1080p60.

GPU time runs out before VRAM, but VRAM is what kills the box: 12 GB with no partitioning means one
tenant's asset load evicts everyone. Vulkan offers no per-process quota and no preemption guarantee,
so the limit must be enforced outside the GPU — cgroup v2 caps, a refused-allocation budget in the
RHI allocator, and an admission controller that queues rather than oversubscribes. The failure to
design against: one long compute dispatch triggering a GPU reset and taking down every session.

## Untrusted content

Encouragingly, **there is no scripting engine in `engine/` today** (verified: no lua/wasm/scripting
hits). Keep it that way for v1 — the untrusted surface is assets and shaders only.

Threats, most likely first: (1) a malformed glTF/STL crashing or overflowing the importer — parsers
are where the CVEs are; (2) a shader that hangs the GPU and resets it for every tenant; (3) resource
exhaustion (uploads, entity counts, VRAM); (4) egress abuse — the box becomes someone's CDN;
(5) lateral movement to the host's own files, which decision 1 removes.

Containment before user #1: cook assets in a **GPU-less, network-less sandbox** (keep
`tools/asset-pipeline`'s existing offline split) under seccomp and a read-only rootfs; run each
session rootless with only the render device nodes, a dedicated unprivileged uid, no network
namespace beyond the gateway socket, cgroup CPU/memory caps, and a hard wall-clock kill.

## Metering, when credits arrive

Meter **wall-clock session-seconds at a declared tier** (`editor` / `play-720p30`), not GPU-seconds:
GPU-seconds are the fair unit and the unattributable one on consumer NVIDIA, and a user cannot
predict that bill. The meter lives in the gateway, driven by the supervisor's own monotonic clock,
never by anything the client sends; the client's only input is a keepalive, and its absence stops
billing. Count **encoded frames egressed** independently as a reconciliation check — if seconds and
frames disagree, something is wrong. Debit in ~10 s increments from a balance *reserved* at session
start; warn over the DataChannel near zero; at zero the supervisor autosaves and terminates. Payment
is four pieces: hosted checkout, one idempotent webhook, an append-only ledger, a reservation row.

## Sequencing

1. **One browser tab sees the block** — gateway + WebRTC + an NVENC H.264 backend behind
   `VideoEncoder`; one hardcoded session, no auth, no credits. *Proof:* `latency.hpp`'s
   input-to-photon ledger reports p50 under 100 ms from a real browser on the LAN, and
   `04-remote-view` still passes against the same protocol.
2. **Two concurrent editor sessions in containers** — rootless, cgroup caps, sandboxed cook.
   *Proof:* one session's deliberately-crashing asset does not perturb the other's frame times,
   and a `docs/perf/` record states the p95 cost of the second tenant.
3. **Accounts and a waitlist** — no payment code. *Proof:* a session dies on its wall-clock cap
   within a second, and a forged client keepalive cannot extend it.

Credits (ledger, reservation, checkout) follow only once the waitlist is full.

**Not yet, deliberately:** user scripting; multi-GPU or multi-node scheduling; autoscaling; AV1;
adaptive bitrate beyond WebRTC's own; cloud project storage beyond a single `.rscene` blob;
Kubernetes anything; audio (still a stub seam); Safari and mobile.

## What would make this worth building

The evidence to watch for, in order: (a) someone who is not the author authoring and playing
something they would call a game, unassisted, in the current editor; (b) a measured second
concurrent 720p session holding p95 under 33 ms; (c) unprompted demand. (a) is the one that matters
— until it happens, a paying user arrives to a grid of cubes.
