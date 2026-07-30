# ADR-0034: SVT-AV1 4.2.0 — a local Conan recipe, and low-delay's picture-in/picture-out contract

- Status: Accepted
- Date: 2026-07-29

## Context

`engine/stream` encodes the streamed viewport with SVT-AV1 ([ADR-0017](0017-streaming-codec.md),
[ADR-0030](0030-streaming-v1.md)). We were pinned to **libsvtav1/2.2.1**, which was Conan Center's
newest recipe when Track S1 landed. Upstream shipped **4.2.0** on 2026-07-13; the pin was 3 major
versions and roughly two years stale.

Two things forced the question:

**1. An intermittent ASan crash.** CI's ASan job SEGVs inside SVT-AV1 rarely but repeatedly, in
entropy coding and rate control. Investigation (2026-07-28/29) cleared our own usage of the library:
`svt_av1_enc_send_picture` copies the input planes synchronously before its async handoff, so
reusing one conversion buffer per stream is safe; and forcing a keyframe through
`EbBufferHeaderType::pic_type` is the documented mechanism in CBR, not the CRF-only CLI path. Two
plausible hypotheses about our code were checked against SVT's own source and **killed**. What
survived is the age of the pin: upstream's changelog records *"fixed race conditions in rate
control"* and an entropy-coding refactor in the 3.x–4.x line — the two subsystems in our stack
trace. That correlation is suggestive, not proof; we have not bisected upstream commits.

There is already a workaround in the tree for a *related* symptom: `scripts/build.sh` builds the
codecs `Release` even in Debug engine builds, because a multithreaded SVT-AV1 `assert()` in
`svt_aom_get_txb_ctx` intermittently aborted macOS CI. That is a second, independent data point that
2.2.1 misbehaves under concurrency.

**2. Low delay's contract changed under us.** SVT-AV1 2.3.0 made `svt_av1_enc_get_packet` *blocking*
in low delay, "enforcing a picture in, picture out model". Our encode loop was written against
2.2.1's non-blocking behaviour and drains until `EB_NoErrorEmptyQueue`. On any version ≥ 2.3.0 that
loop **deadlocks on its second call** — no further input is pending, and `EmptyQueue` is unreachable
before end-of-stream. This is a hang, not a compile error, so no amount of "it builds" would have
caught it.

## Decision

**1. Bump to libsvtav1/4.2.0, carried by a Conan recipe we maintain ourselves** at
`third_party/conan-recipes/libsvtav1/`. Conan Center's newest `libsvtav1` is 2.2.1, so there is no
recipe to bump *to* — the version string is not the change.

The recipe is a **rewrite**, not a copy of Conan Center's with the version edited, because SVT-AV1
4.x deleted the CMake knobs the 2.x recipe drives. A copied recipe would configure nothing and
silently produce a differently-built library:

| Conan Center's 2.x recipe does | In SVT-AV1 4.x |
| --- | --- |
| `BUILD_ENC` / `BUILD_DEC` | Gone — the decoder was removed from the project; the encoder is unconditional |
| `USE_EXTERNAL_CPUINFO` + `requires("cpuinfo/…")` | Gone — 4.x does its own CPU feature detection |
| `ENABLE_NASM` | Gone — x86 asm is gated on CMake's `check_language(ASM_NASM)`, so nasm must be *on PATH* |
| `option build_encoder/build_decoder` | Meaningless; dropped |

What did **not** change is the part our build glue depends on: the library is still `SvtAv1Enc`,
headers still install to `include/svt-av1`, and the pkg-config name is still `SvtAv1Enc`. The recipe
therefore keeps Conan Center's `encoder` component name, so `libsvtav1::encoder` — the target
`/CMakeLists.txt` already looks up — resolves unchanged and the CMake layer needs no edit.

Local recipes must be exported into the Conan cache before `conan install` can resolve them.
`scripts/conan-export-local.sh` does that, and is called from all four places that drive Conan:
`build.sh`, `sdk-smoke.sh`, `editor-smoke.sh`, and CI's TSan job.

**2. Adopt the picture-in/picture-out contract explicitly.** `encode()` now makes exactly one
`svt_av1_enc_get_packet` call per sent picture, with no poll loop and no sleep. In low delay the
library takes its blocking path regardless of the `pic_send_done` argument, so the call waits for our
frame's packet and the wait *is* the encode time.

## Consequences

**We lose encode()'s wedge timeout.** The old 10-second poll deadline turned a hung encoder into a
`false` return; the wait now happens inside SVT, where we cannot time it out without standing up a
watchdog thread. A genuinely wedged encoder hangs the calling stream thread instead. We accept this:
the bound is now the library's documented contract rather than our clock, and it is not a
configuration we can opt out of while staying in low delay. If it ever bites, the fix is a watchdog
around the stream thread, not a return to polling — polling is no longer possible.

**Vendoring a recipe is a maintenance debt, and a small one.** `third_party/conan-recipes/` is a new
seam. It should stay empty in the steady state: when Conan Center ships a 4.x recipe, delete the
directory and return to a plain `self.requires("libsvtav1/<version>")`. The recipe's own header
comment says so.

**The ASan crash is not yet proven fixed.** This ADR bumps the dependency on the strength of a
changelog correlation and a dead pin; it does not close the ASan investigation. The evidence we still
lack is a before/after soak on real Linux + ASan — the exact configuration that fails — which is now
the follow-up. Rare crashes need a run count to say anything: absence in a short run is not a fix.

**Not adopted: `RTC_BUILD`.** SVT-AV1 4.x exposes an `RTC_BUILD` CMake option aimed at real-time
communication, which is precisely our use case. It changes public compile definitions and therefore
encoder behaviour, so folding it into a version bump would confound this change with a tuning
change. It is worth measuring separately in `samples/codec_bench`.

**Superseded nothing.** [ADR-0017](0017-streaming-codec.md)'s choice of AV1-on-licensing and
[ADR-0030](0030-streaming-v1.md)'s "software SVT-AV1 as the CI-provable reference path, hardware
encoders as the destination" both stand. This is a stale pin corrected, not a codec reconsidered.
