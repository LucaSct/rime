# Walking shadow stability — 2026-10-19

Branch: `fix/walking-shadow-stability`, stacked on `feat/editor-player-preview`
at `38f44c3` (draft PR #345). Draft
[PR #346](https://github.com/LucaSct/rime/pull/346) is pushed. Nothing merged or
browser-deployed. Remote CI was queued at publication; local results below are
not a claim of completed remote CI.

## Ready to use

Reopen the native city editor with `./scripts/city-v2.sh`. Select Walk, start
Preview and click the viewport. The existing editor process must be restarted to
load the updated host. Run game still launches the armed full city separately.

Fresh standalone Linux development build:

`/home/next/rime-exports/city-stable-lighting-2026-10-19/the_block`

Manifest SHA-256:
`ba7f0766d2d80e900fcdcf47bf01bd3305dd164aeaca9fd96102ecdcd8557426`.
Repository-hidden execution and refusal/integrity checks pass. Earlier exports,
the saved live city and source maps remain intact. The user courtyard retains
blob `794da29c6968416bdebd1ee34a6f6f95e190ed8c` and is intentionally dirty.

## What changed

Sun cascades use a camera-local analytic fit, fixed light basis and guarded texel
snap. On the same 600-pose walk/turn sweep, fixed-world grid drift falls from
0.425367355 to 0.000122070 texel; XY scale variation becomes zero. Mesh and terrain
share continuous cascade composition, including overlapping fades. Local spot
maps refresh for moving, edited and removed casters while keeping static and
camera-only cache reuse. Alpha-masked materials filter before discard. A missing
camera invalidates temporal history. Editor lighting now enables authored sun
and spotlight shadows at the unchanged 960×540 stream size.

Commits: `621c0be` tracker, `9f6d8f1` shared sampling/material order,
`9446188` stable fit, `6b7ea91` motion/foliage regressions, `9507cb2` integration
and overlapping-band correction. Documentation/evidence follows those commits.

## Verification and collaboration

Full Release build and all **250 renderer cases** pass on RTX 3060 and lavapipe.
Full CTest is **92/94**, retaining the inherited collapse failures and edited
courtyard failure; canonical courtyard separately passes 2160 ticks. Changed-TU
CPU ASan/UBSan/LSan passes 16/8389; Vulkan ASan/UBSan passes 6/2208 with GPU driver
leak checks disabled. Exact pinned formatting and license checks pass. Real
editor Walk/release/Stop and isolated packaged-game window proofs pass. Full
evidence and reproduction helpers are in
[docs/verification/shadow-stability-2026-10-19](docs/verification/shadow-stability-2026-10-19/README.md).

Claude Sonnet 5.5 implemented the isolated cascade-fit leaf. Native parallel work
supplied the caster tracker and GPU motion/foliage proof; root integrated and
verified all changes. Astra audited the paths and found the intermediate
overlapping-band defect, now covered by a failing-before GPU case. Fable could
not consult: gateway HTTP 402 with no balance/allowance, then subscription CLI
`model_requires_usage_credits`. No paid API spend; $7 ceiling unconsumed. Final
Claude personal usage: 38% five-hour / 13% weekly, reset 22:00 local / Oct 26
11:00 local. Code organization OAuth still returns 403. Astra subscription CLI
works now; an older quota prediction was removed from the global rulebook.

The hardware performance attempt is retained as
[rejected evidence](docs/perf/shadow-stability-2026-10-19/README.md): unstable
graphics clocks, different baseline driver and exceeded frame/simulation budgets.
No qualified FPS claim or inherited gate waiver. Own three clean worktrees and
139.6 MB of generated scratch binaries/objects were removed after retaining
logs/hashes. Branch refs remain. Other sessions' worktrees and evidence were left
alone; more than 15 GiB remains free. Raw local reports:
`/home/next/reports/rime-shadow-stability-2026-10-19/`.

## Remaining scope

Sun specular/reflection matrix, jitter, velocity and roughness routing was
audited; SSR tracing is unchanged. Screen-space hit/miss popping and history
disocclusion remain separate work. Point-light cube shadows, terrain casting,
volumetric particle shadows and a full-body first-person actor are not added.
Synthetic rigid/foliage tests exercise common world-mesh behavior, not every
asset or a new skeletal animation system. Texture writes retaining the same
handle still need explicit shadow invalidation. Dynamic vertex overrides with
unknown bounds deliberately refresh all local maps.

After visual review, next renderer brick is a measured SSR disocclusion/hit
coverage proof; creator roadmap work remains one authored heater interaction and
exit objective, then reusable presets. General terrain walking, swimming,
gameplay ice, collapse/performance qualification and shipment remain open.
