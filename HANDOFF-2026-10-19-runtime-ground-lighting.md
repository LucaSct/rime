# Run-game floor follow-up — 2026-10-19

**Stopped at the owner's request. The tree/house-wall flicker is not yet verified
fixed. No unattended continuation is running.**

Branch `fix/runtime-ground-lighting`, based on `727c851` / draft PR #346.
Implementation: `84f3126` and `b340d44`; evidence `86f5525`. Draft
[PR #347](https://github.com/LucaSct/rime/pull/347) is pushed. Nothing merged or
browser-deployed. The local test results below are not remote CI completion.

## Latest clarification and exact resume point

The owner clarified the main symptom is **between buildings, beneath trees, and
tree silhouettes on house walls**. The verified floor GI-band correction below
is separate; do not present it as completing this clarified case.

A new actual Run-game recording captures a tree against a brick wall:
`/home/next/reports/rime-runtime-ground-2026-10-19/ground-tree-wall-current.mp4`.
The accompanying `tree-wall-current-contact.png` and `record-tree-wall.py` retain
the view/input sequence. Current production code already includes the GI query
correction and 160-unit shadow range. This is an observation recording, not a
measured before/after tree-shadow acceptance result.

Saved showcase sun travels approximately `(+0.990,-0.140,0)`. Useful targets:

- Tree `(10,0,-8)`, scale about 1.2769; receiver west wall `x≈11.85–12`,
  `z≈-8`, heights 3–8. Eye `(9,1.6,-5)`, yaw `-0.6435`, pitch `+0.4475`.
- Tree `(22,0,8)`, scale about 0.9027; receiver west wall `x≈23.85–24`.
  Eye `(21,1.6,5)`, yaw `-2.498`, pitch `+0.4475`.

The 26 runtime trees are static `PlayEnvironment` meshes, absent from the saved
editor scene; no wind/deformation explains camera-only changes. Full draw lists
retain off-screen casters; main-view shadow caster culling defaults off. Wall
shadow bias uses unperturbed normals. A remaining **hypothesis**, not GPU proof:
leaf masks have nine ordinary downsampled alpha mips with cutoff 0.5; the actual
mask's CPU coverage rises from about 52.11% at level zero to 100% at 2×2/1×1.
Different cascade/mip sampling can therefore change leaf-card silhouettes. Do
not implement a mip policy merely because this hypothesis sounds plausible.

Next experiment: measure fixed wall points during pure yaw and a short strafe,
inspect cascade participation/shadow visibility, then independently compare
the 160-unit cap versus uncapped maps and diagnostic mip-zero/opaque-leaf
controls. If instability occurs wholly inside one unchanged-scale cascade,
inspect its own depth image and light-depth clipping rather than blaming blends.

An external probe **compiled but has not been run**:
`tree-diagnostic.cpp`, `tree-main.inc.cpp`, `build-tree-diagnostic.py` in the raw
report directory. It preserves the production Play entry and additionally
supports `--tree-probe MODE OUTPUT`, modes `full`, `no-ddgi`, `no-ssr`, `sun`,
`clear`; `RIME_DIAG_SHADOW_DISTANCE=0/160` selects range. It exports HDR and
samples wall anchors. Its assumptions/coordinates/output need checking before
trusting any result. The generated binary/object were removed after retaining
hashes to preserve disk reserve; rebuild from these sources if resumed. The
initial plan is diagnostic only, with all features enabled in final acceptance.

## User-visible correction

The owner clarified that the editor was fine; actual Run game still changed
floor darkness while turning/walking. Root recorded the real RTX 3060 game at the
saved basin spawn, then in a separate scene copy with spawn X=22 in the street.
Short [before](docs/verification/runtime-ground-2026-10-19/ground-city-before.mp4)
and [after](docs/verification/runtime-ground-2026-10-19/ground-city-after.mp4)
recordings are retained. The initial editor proof was insufficient for this
symptom; settled basin yaw anchors were also already stable.

Replicated-city controls show a sharp floor band with SSR off and no band with
DDGI off. Sampling diffuse GI just above the receiving surface removes it while
leaving all runtime features enabled. In matched raw captures, its encoded-luma
step falls from 0.0412105 to 0.000644755. The exact atlas rejection/normalization
mechanism remains inferred; no atlas readback was made. The actual after-video
was visually inspected for this band, not declared a proof of every artifact.

Separately, the game no longer stretches local sun maps over its 3 km terrain
range: shadow coverage is capped at 160 units, retaining 3072-unit raster reach.
The new renderer setting defaults to zero and preserves uncapped fits bit-for-bit.
No map size, bias, PCF radius or resolution increase; no feature-disable fix.

Close a running game and click **Run game** again to load the new executable.
The editor's existing GI/SSR configuration is unchanged. Older standalone bundles
remain frozen; use the fresh bundle recorded below for standalone testing.

## Verification and review

Full Release build; RTX 3060 renderer **255/255**, 1,720,843 assertions, zero skipped.
New production-forward GPU checks cover floor/ceiling, exact/near probe planes,
world-anchored yaw, authored/local modes, measured black and occlusion. Original
fragment shader fails 51 receiving-side assertions; occlusion controls pass.
Fixed shader passes 2/241. Range checks and original stability tests pass 10/10294.
Required lavapipe renderer also passes 255/255 and 1,720,843 assertions. Full
CTest remains **92/94**, inherited collapse claims and edited courtyard; canonical
courtyard passes 2160 ticks. The full LastTest log was copied before export cooking.
Changed-TU Clang and CPU ASan/UBSan/LSan pass; GPU ASan/UBSan passes with driver
leak checks disabled. Existing closed-room/leak regressions remain green. No Rust
or threading changes, and no new TSan run.

Claude Sonnet 5.5 coded the isolated range leaf, native parallel work supplied
the production GI fixture and runtime-path analysis, and root owns the query
change, runtime integration, recordings and checks. Astra independently reviewed
the fix and cautioned against claiming a proven precise atlas mechanism or that
all shadows are fixed. Fable's prior unfunded routes were not retried. No paid API
spend; Claude personal final read 5% five-hour / 13% weekly (02:59 / Oct 26 10:59
local resets). Code usage reader was expired; that is not a fresh quota verdict.

Fresh standalone development build:
`/home/next/rime-exports/city-ground-lighting-2026-10-19/the_block`, manifest SHA-256
`a029f50332e276315da914caef4f9bb91d6cdd78a7fd1a3c63ef7e46227f979e`.
It exports the current saved live city, preserving the user's camera edits;
the diagnostic relocated spawn is not exported. Repository-hidden proof and
integrity/refusal receipts are retained with the evidence. Earlier exports remain.

The fresh hardware performance attempt was rejected: graphics clocks 210–1935 MHz
(93.9% spread), old-baseline driver mismatch, frame/simulation budget failures.
Raw report is in `docs/perf/runtime-ground-2026-10-19/`; no qualification waived.

See [ADR-0102](docs/adr/0102-runtime-floor-lighting.md) and
[full evidence](docs/verification/runtime-ground-2026-10-19/README.md).
Local raw reports: `/home/next/reports/rime-runtime-ground-2026-10-19/`.
The completed clean Claude worktree was removed; unmerged branch ref retained.
Own diagnostic/sanitizer binaries were removed after SHA-256 receipts. Other
sessions' worktrees/evidence were left alone; the 15 GiB reserve is enforced.

## Remaining scope

The offset is not clearance-aware: narrow gaps, tiny spacing and interpolated or
backface normals remain limits. Camera-relative GI support, approximate default
building SDF proxies, binary SSR hit/miss and editor/runtime lighting differences
remain separate work. The source/live city and user courtyard are preserved.
Collapse/performance/shipment gates remain open. General terrain walking,
gameplay ice and the creator interaction/objective/preset roadmap are unchanged.
