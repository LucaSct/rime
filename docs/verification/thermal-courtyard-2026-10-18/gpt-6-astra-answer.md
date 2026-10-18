### Proposal: transient appearance overlay, removable thermal adapter

Ship an **opt-in visual phase demonstration**, not a fluid simulation. Use a saved courtyard scene containing a few explicit thermal cells on shallow water/ice display meshes, controllable heaters/coolers, and a separate particle-based smoke/fire exhibit. Label the latter “visual effect; not coupled to thermal solver.”

### API and layering

Add a small reusable renderer input:
`FrameAppearanceOverrides`: a bounded list of `{generation-safe entity, material-slot, replacement PBR values}`.

Overrides are frame-owned, copied like existing transient lights/particles, and never write ECS components or MaterialRegistry. Resolve them **after normal submesh/material-set resolution**, producing private per-draw GPU uniform copies. Unspecified slots retain authored appearance. Empty input takes the existing path unchanged. Reject duplicate keys, invalid slots, nonfinite values, and capacity overflow deterministically; ignore stale entity generations without retargeting.

A removable `ThermalPhasePresentation` adapter reads only ThermalPreview’s **accepted snapshot**, validated bindings, and status. It emits overrides, plus an editor/standalone status indicator. Renderer knows nothing about enthalpy or phase.

For each bound surface, map validated liquid fraction to two explicit, artist-chosen PBR endpoints: rough opaque ice versus darker, smoother opaque water. Interpolate base color and roughness only; use lighting that makes the change unmistakable. Do not imply changing geometry, flow, collision, volume, or thickness. No transparency or shader changes today.

### Ownership and lifecycle

Persist the courtyard, solver configuration, presentation bindings/endpoints, initial state, and deterministic source schedule. Do not persist transient overrides as authored materials.

Keep the existing 16-cell limit, cap bindings independently, and rebuild adapter bindings on edits. Associate accepted snapshots and bindings with a configuration revision; never mix revisions.

- **Run:** publish only committed states.
- **Pause:** retain accepted appearance.
- **Failed step:** optionally retain last accepted appearance, with conspicuous “step failed—showing last accepted state.”
- **Stop, invalid graph, deleted binding, or disabled presentation:** clear relevant overrides immediately; authored appearance returns.
- **Load/rebuild:** clear first, validate, then publish.

Use the same bounded presentation driver in editor and standalone; save a camera and restartable freeze/melt schedule.

### Decisive verification

Independently verify:

- **Pixels:** fixed-camera ice and water checkpoints differ clearly; Stop matches authored baseline; neighboring shared-material objects remain unchanged.
- **Sensitivity:** increasing heater power advances melting; cooling reverses it; zero sources plus insulated uniform cells stays unchanged.
- **Negative:** rejected transactions never display tentative state; invalid graphs clear presentation; entity deletion/reuse cannot inherit overrides; bad slots and overflow stay bounded.
- **Regression/repeatability:** empty input matches original rendering; authored components/registry remain unchanged; reload and restart reproduce accepted states and checkpoint images within declared tolerances.
