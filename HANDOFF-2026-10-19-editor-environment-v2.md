# Editor navigation and environment V2 — 2026-10-19

Read `CLAUDE.md`, the global rulebook at
`/home/next/.claude-switch/profiles/default/CLAUDE.md`, and
`docs/verification/environment-v2-2026-10-19/README.md` before resuming.

## Ready to use

```sh
./scripts/city-v2.sh
```

The native editor was reopened on the RTX 3060 as PID 92772 during verification;
that process has since exited without an error in its output.
It loads the persistent `build/release/city-v2.rscene`, initially copied from
`samples/99-the-block/city-v2.rscene`: 630 entities, 569 destructible previews,
an enabled PlayerSpawn, fire, smoke, water and frost sheets. Preview animates
the editor; Stop Preview, then Run game saves and launches the weapon player.
The stream remains 960×540 with correct letterboxing. The live copy has already
been navigated and saved since launch; **do not reset it to the source**.

Standalone Linux development build, tested with repository and home hidden:

```sh
/home/next/rime-exports/city-v2-2026-10-19/the_block
```

The older `city-demo-2026-10-19` export is untouched. The new bundle uses the
committed showcase, not subsequent edits to the live editor copy. Re-export a
saved live map to a **new** directory with `scripts/export-city.sh --scene`
(destination is the first argument). Bundle manifest:
`b396c7cdabebd1f908de61d4ac6726e06f1c2ed35b89c0f41027e1adc74331b3`.

## Implemented

- Fixed competing C++/Rust camera caches: movement persists through Preview
  ticks, release, Pause and the first movement after Stop. Fitted viewport
  coordinates also exclude letterbox bars from navigation/picking/gizmos.
- Added typed ThermalCell reference pickers with readable names, exact IDs,
  null/missing handling and normal undo/redo.
- Added multiscale animated water normals, branching blue frost and blended
  phase detail; textured smoke with birth/fade and bounded fire lighting;
  tapered moving flame sprites, separate from additive weapon effects.
- Added authored PlayerSpawn with shared host/game registration, explicit
  refusals, parent composition, authoritative initial position/client look and
  recovery. Outliner → Gameplay discovers it.
- Full city and scope views now consume saved authored environment components.
  Runtime FX/thermal state is cosmetic and bounded; maps without enabled
  components retain the old behavior and allocate no phase atlas.
- Added the persistent showcase launcher and updated creator guide,
  module docs, roadmap and ADR-0098/0099.

## Parallel work and account observations

Claude Sonnet 5.5 on the `personal` subscription implemented the phase maps and
spawn resolver in two isolated worktrees. Root reviewed and integrated both,
tightened quaternion validation instead of accepting arbitrary normalization,
and verified the actual server/predicted spawn after startup. Native delegates
implemented camera reconciliation, entity references, FX shaders and the runtime
bridge while root implemented presentation integration, the demo and evidence.
All six completed clean worktrees were removed; their branches remain.

The `code` profile refreshed credentials but returned organization-disabled
subscription access, not an exhausted window. `personal` was measured working;
the last recorded usage was 6% of its window and 0% weekly. These are dated
observations, not availability promises. No premium gateway consultation or
paid API work was used. Reports/specs live in
`/home/next/reports/rime-environment-v2-2026-10-19/`; selected reports are
committed with verification. No Claude job remains active.

## Validation and delivery

Branch `feat/environment-editor-v2`, based on `b5bf53b` from
`feat/city-editor-play-launch` (draft PR #343). Nothing merged.

- Release build, 151 GUI tests, Rust workspace tests, strict default/GUI clippy,
  formatting and license checks pass.
- Targeted GPU checks and 52 changed-TU ASan/UBSan cases pass. Dependency
  archives were not fully instrumented; Vulkan driver leak checks were off.
- Real GUI navigation and picker selection/undo/redo/save proofs pass.
- Full CTest: **92/94**. Inherited collapse assertions remain red. The other
  failure reads the user's existing ±1 GW courtyard experiment; the canonical
  scene copy passes 2160 ticks. No assertion or user edit was weakened.
- Bundle: 32 Python checks, 11 cook fixtures, integrity/refusal checks and an
  actual isolated Vulkan window pass. Final runtime receipt: two source FX,
  three flames, one light, two phase surfaces/cells, atlas allocated, zero
  thermal refusals; all three weapon assets load.
- Hardware performance **failed**, not qualified: GPU clocks were unstable,
  frame/simulation p99 exceeded budgets, and the old baseline fingerprint did
  not match. The rejected report/log are in
  `docs/perf/environment-v2-2026-10-19/`. No baseline or GPU setting changed.

The user's tracked courtyard remains dirty with blob
`794da29c6968416bdebd1ee34a6f6f95e190ed8c`; its existing EOF blank line is not
ours to remove. Earlier untracked notes/handoffs are preserved. Space at final
verification was about 15.8 GiB free; own sanitizer build artifacts were cleaned
after keeping their logs. Do not consume the runner reserve with another build tree.

## Remaining boundaries / next bricks

Performance and collapse/stack CI gates remain open: no merge. The phase atlas
uses 22.17 MiB plus driver overhead and eighth-step phase/32-frame detail.
This is polished surface presentation, not water flow, ice collision or
authoritative combustion. Environment poses still use the city's flat transform
contract; spawn overlap validation and teams remain future work.

The editor protocol lacks per-edit revisions and Play-session identities;
pending-pose/same-pose ownership ambiguity is documented in ADR-0098. A robust
revision handshake is a next camera brick. Other next bricks are player
possession inside Preview, event wiring, prefab library and adaptive editor
stream resolution. Avoid presenting those as implemented.

No browser deployment changed.

## Follow-up: timber basins and fire dynamics

Draft PR **#344** is the delivery PR for `feat/environment-editor-v2`, stacked
on #343. The follow-up adds 32 static timber boards (662 total entities) to the
source showcase and the saved live copy without rewriting existing entity text.
Both sheets have grounded wooden sides, caps and posts. SlabRole kind 12 derives
the existing wood material and cube mesh; editor labels read “Timber board”.
Fire now has a steadier root and independently surging/lifting/fading side tongues
with smooth eight-second loop joins. Claude Sonnet 5.5 implemented the fire leaf;
root implemented surrounds and verified the integration.

Updated standalone: `/home/next/rime-exports/city-v2-timber-2026-10-19/the_block`.
Both earlier exports remain intact. The new bundle passed isolation, integrity,
refusal and actual weapon-window checks. SHA-256:
`7f4cca9c8c2abe7e1c490142cb24de16171d422e04e71512c510dfca70c9d0eb`.

See `docs/verification/timber-fire-2026-10-19/README.md`: 151 GUI checks, 7
blockkit cases, 10 VFX cases, 9 authored runtime Vulkan cases, and 17 focused
ASan/UBSan+LSan cases pass. Initial concurrent GUI child-process deadline
failures are retained; the idle rerun passed with no assertion/deadline change.
Full CTest was not repeated for this presentation follow-up. The fresh hardware
run again failed clock stability, budgets and fingerprint comparison; rejected
receipt is in `docs/perf/timber-fire-2026-10-19/`. Nothing merged; gates remain open.
