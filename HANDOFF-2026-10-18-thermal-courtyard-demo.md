# Day-end handoff — working Frost Yard demo — 2026-10-18

Latest user request: finish with a working new/existing city scene demonstrating
updated features, at a good demo/day-end exit point. They also asked what is new
in browser/editor. **The working example is implemented and verified.** Native
editor has reflected smoke/fire + thermal authoring and live selected-cell thermal
readouts. Browser remains the existing streamed-play shell; no browser thermal
inspector/deployment was added or proven. Do not imply otherwise.

Branch `feat/thermal-courtyard-demo`, implementation **8727f98**. Evidence/handoff
is the following commit. Parent `feat/thermal-editor-authoring` **06d16da**,
draft **#340** https://github.com/LucaSct/rime/pull/340, containing 74cd474 authoring
implementation and proofs. Earlier field parent is c1ca901, draft #339. Nothing
merged; M18/performance and city/physics gates remain open.

## Run it

From `/home/next/projects/rime`:

```sh
./scripts/thermal-courtyard.sh
./scripts/thermal-courtyard.sh --editor
```

Wrapper builds target `thermal_courtyard`. First opens standalone; second uses
Cargo GUI editor and the sample's correct host/scene. **Space/P** pause/resume,
**R** restart, **Esc** close. Standalone auto-pauses at36seconds after both phase
transitions, before material-domain refusal. Editor Play is user-controlled.

Saved scene: `samples/thermal-courtyard/courtyard.rscene`,155entities317components.
Readme: `samples/thermal-courtyard/README.md`. Native screenshot:
`docs/verification/thermal-courtyard-2026-10-18/yard-native.png`.

Left: cooling10kgwater at-100kW → frozen260.236K. Center: heating10kgice at+100kW
→ liquid279.491K. Right: insulatedcontrol staysliquid273.15K. Accepted numerical
mass fraction drives pale/rough ice versus blue/smooth opaque appearance. Geometry
stays fixed; no thickness, flowing-water, refraction or collision claim. Explicit
heat sources are independent from cosmetic fire/light/smoke; no combustion authority.
Pips identify stations without relying on colour. Built-in meshes/materials, no
asset downloads or cook needed. Persistent sample content roles reconstruct draws
when loaded; no runtime mesh/material IDs in the saved scene.

## Implementation and verification

- New removable `fluids_render` with disabled-by-default `ThermalSurface` binding;
  stateless bounded accepted-phase conversion to transient factors. Existing
  numerical fluids and fluids_scene interfaces remain independent of rendering.
- SceneRenderer copies at most16 whole-entity RGB/roughness rows after submesh
  resolution, never ECS/shared material writes. Invalid/capped/duplicate input
  clears whole batch. Stale generations never retarget. Empty input restores
  authored look. Refused numerical steps retain accepted look with separate status.
- Same adapter in standalone/editor. Copied lens keeps narrow-window overview;
  captures use actual framebuffer size. Actual Application events prove controls.
- Full release buildpass. Newpixel/adapter3cases4789assertspass; actual accepted
  freezing radiance .423309 vsliquid .063166; copyownership, unchangedneighbor,
  empty/stale/invalid restoration exact. No goldens/cross-devicebitidentityclaim.
- ASan/UBSan3/4789pass with CI GPU policy detect_leaks=0; CPU2/616pass withLSan1.
  Initial GPU LSan368bytes/3 unresolved external-module allocations retained;
  do not call whole-engine leak-free. Instruments adapter/test/SceneRenderer,
  ThermalPreview andall3 numericalthermalTUs; otherarchivesnormalrelease.
- Clang actualflags fornew/changedTUs, finalsample rechecked. FullRustworkspace,
  GUI122, exactdefault+GUIClippy, pinnedformat/license/whitespace pass.
- Full lavapipeCTest **92/93**,exit8,293.71s: onlysame3 inherited blockcollapse/
  localityclaims. Inputcheckpasses. No thresholdsweakened.
- Realviewport120steps cooling/melting/control, Pause, Stop,newPlaypass; engine
  save + freshprocessreopen0skipped. Endpointselftest2160ticksunchangedauthoreddata.
  Disabled required controlstation safelyrefusesexit2. InstalledSDKadapterpass.
- RTX3060driver615.78.08 nativefunctionalcompletecapture0refusals; nativeWayland
  window and separately owned XvfbX11window bothpresent/capture. No hardwareperfclaim.
  Actualwindow output969x1025 versus requested1280x720; capture/lenshandleit.
- Correction: DISPLAY=:99 alone does NOT disable WAYLAND_DISPLAY. Earlier authoring
  evidence's "isolated Xvfb" interpretation was wrong. Appended correction there.
  New `window-verify.py` unsetsWayland, launches anownedunusedXvfb, asserts real
  presentation andstops onlyownserver. The CI-owned:99 display wasnot usable as
  a realwindow here; its inputtest canpass afterheadless fallback. Do not infer
  backend/window success from that testalone.

Evidence `docs/verification/thermal-courtyard-2026-10-18/README.md` retains commands,
logs, realcaptures andfinal source/binaryhashes. Smallfinalsamplecapture/lens/
status/refusal fixes hadfocusedfinalbuild, Clang,input/capture/numericalchecks;
no needanotherwhole-suite repeat absentnewchanges. Root reviewed allhunks and
checked generatedscene containsauthoredroles/references, no deriveddrawIDs.

## Consult/spend and next work

One new backgrounded tool-free Astra independentproposal, liveprices/availability
checked; response and exactgatewaydelta **$0.03271**. Earlierauthoringcall$0.04958;
these two total$0.08229 (not a claimaboutallpriorsessioncalls). Adopted transient
appearance/acceptedlifecycle/neighbor+stalepixelcontrols; simplified per-slot and
arbitraryPBR proposal towhole-entity RGB/roughness, fixedpalette/authoredconstant
sources. No newFablecall; previous thermalresearch applies. Consult isnot final
codereview. ADR0095recordsdecision. Privatebilling/credentialsnotcommitted.

Good stop point now. Next optionalbrick: clickedthermalcreation/edit/save/reopen
proof and browserauthoring/readout parity; separately water solid displacement/
bodyfeedback+mass/energytransport, smokeintersection/lightingfidelity, canonical
ignition/fuel/spread and authoritative network/exportproofs. Do notreopen all
milestones because this demo works. M18p99sim.block8.074>6, clockqualification and
cityconvergence remainopen. Nothingmerged.

Scratch `/home/next/reports/rime-thermal-courtyard-2026-10-18/` (~63MB). Root/shared
runner17GBfree,minimum15. OwnXvfb/demo processesstopped afterchecks. Olduntracked
notesbelongtoothers; neveradd-all/delete/reset. Actualglobalrules at
`/home/next/.claude-switch/profiles/default/CLAUDE.md`; projectCLAUDE,roadmaps,
brick-deliveryskill. Explicitstage, ownbranches, RESTdraftPR, no mergewithoutgates.
