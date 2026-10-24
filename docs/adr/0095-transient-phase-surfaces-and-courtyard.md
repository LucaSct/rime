# ADR-0095: Transient phase appearance and a saved courtyard demo

Status: proposed opt-in implementation, 2026-10-18; no M18 or shipment closure.

## Context

ADR-0094 exposes validated thermal values in the editor. The user asked to end
the session with a working new/existing city scene demonstrating the updated
features. Fixed-cell phase presentation can be verified now; moving-water mass
transport, ice geometry and combustion authority are separate unresolved models.

## Decision

Add a small reusable renderer seam: copied bounded `SurfaceAppearance` rows keyed
by full generation-safe entity handle. After submesh/material-set resolution,
replace RGB and roughness in the private per-frame material copy, leaving shared
registries and ECS untouched. Sixteen rows maximum; null/duplicate keys,
nonfinite/out-of-range factors or overflow refuse and clear the whole batch.
Empty input restores authored appearance. Unmatched/stale handles affect nothing.

A removable `fluids_render` adapter above numerical scene authoring and rendering
reflects one disabled-by-default `ThermalSurface` binding. It reads the current
committed preview field and emits a fixed pale/rough ice ↔ blue/smooth water
palette using liquid mass fraction. It caches no bindings/state, so graph reset,
invalidity, deletion and edits cannot mix old state with a new revision. Numerical
refusal retains committed appearance while the editor reports refusal separately.
No geometry, alpha or shader change and no fractional-volume interpretation.

The saved Frost Yard courtyard has three fixed samples: equal-mass cooling,
melting and unheated control, plus spatially separate cosmetic smoke/fire. A sample
host reconstructs built-in geometry and palette from persistent content roles.
The standalone composition and editor use the same numerical/presentation APIs.
Its standalone 36-second duration pauses before range refusal, after both latent
transitions; Pause/Resume/Restart use real Application input. A copied lens adapts
the overview to narrow windows; camera authoring remains unchanged. Captures use
actual framebuffer dimensions rather than assuming requested window dimensions.

## Consequences and verification

Astra independently proposed transient frame-owned PBR appearance and a removable
adapter. Adopted committed-state/lifecycle separation, fixed phase endpoints,
shared-material neighbor and stale-generation pixel controls. Simplified its
per-material-slot binding to a whole-entity opt-in diagnostic, and its arbitrary
PBR replacement to RGB/roughness only. Persist no appearance endpoints/schedule
API today: the documented fixed palette and sample's authored constant sources
are enough for this bounded result. Response cost $0.03271, locally verified;
consultation is not a code review. Root owns edits and complete self-review.

Tests check admission/copy ownership, accepted phase progression, numerical
refusal retention, binding changes, invalid/reset clearing and no authored edits.
Real rendered ice/water differ strongly; neighboring shared-material pixels and
empty/stale/invalid clearing match baseline exactly with temporal effects off.
A saved-scene selftest traverses both latent transitions and proves the insulated
control and unchanged authored data. Real-host editor tests Step/Pause/Stop/restart
and save/reopen. Headless/native/windowed captures and sanitizer limitations are
recorded with [verification](../verification/thermal-courtyard-2026-10-18/README.md).

No new default-on workload, physical water surface, ice thickness/collision,
network determinism, fuel/spread/damage or native export proof is claimed. The
city is a compact self-contained diorama, not an authoritative gameplay city.
M18/performance gates remain open; enabled-scene capacity is not qualified by a
functional screenshot. This draft gives the user a working day-end example.
