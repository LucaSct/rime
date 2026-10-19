# Run-game floor follow-up — 2026-10-19

Branch `fix/runtime-ground-lighting`, based on `727c851` / draft PR #346.
Implementation: `84f3126` and `b340d44`. Nothing merged or browser-deployed.

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
