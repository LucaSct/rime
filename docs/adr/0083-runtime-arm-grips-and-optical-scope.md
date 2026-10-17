# ADR-0083: runtime arm grips and an independent optical scope

- Status: Accepted for `99-the-block --play`
- Date: 2026-10-15
- Extends ADR-0082; supersedes its static arm-pose decision only.

## Context

The first viewmodel brick baked one rough wrist pose into both rifles. A rigid group
cannot adjust elbows while the weapon moves relative to the shoulders. The owner
requested runtime arm IK and a modern sniper with a German-made optic, magnification
only inside the lens, and glass reflections.

## Decision

`PlayArmIk` is sample-owned. It reads existing cooked skeletons and weights, solves
analytic two-bone chains from joint heads, and constrains complete wrist frames after
sway, recoil and aiming. Shoulders stay camera-attached; poles choose elbow bend planes.
Unreachable targets clamp without stretching, exposing counts and residuals. Fingers
keep authored local poses. CPU linear blend skinning deforms positions, normals and
tangents every rendered frame.

The renderer accepts current vertices and previous submitted local positions for an
immutable mesh. Borrowed arrays are copied into retired graph scratch; indices stay
resident. Current vertices drive every geometry pass. A velocity-only twin carries
previous positions in its unused normal slot, selected through the remaining 16 bytes
of the 256-byte draw-uniform stride. Appearance, identity changes and cuts reset that
history. Rigid draws retain zero-flag behavior. This is not a general animation module
or GPU skeleton API.

The CC-BY Remington MSR by Kaan includes a Schmidt & Bender 5–25×56 scope. A physical
ocular replaces opaque source glass. A second `SceneRenderer` owns its camera and
history, rendering the same world and terrain through a narrow projection. Explicit
camera snapshots avoid changing the world's camera. After weapon shading and before
parent TAA, a lens pass samples resolved optical HDR and filtered sky reflections into
loaded parent HDR, respecting housing depth. A virtual focal plane, exit-pupil mask,
eye relief and optic-fixed reticle approximate optics. Fresnel weights reflection.
The parent projection stays unchanged.

## Consequences

Selecting the sniper costs a second 512×512 scene view, currently fixed at 5×. The
asset's 5–25×56 name does not imply variable zoom. The MSR retains its solid source
materials; it has no texture maps. Other weapons allocate no optical renderer.

Art and editable rigs remain outside git. Preparation exports three weapons and three
weighted arm rigs in uniform camera-local metres. Synthetic CI tests prove contacts,
skinning, deformation velocity, frame lifetime, lens boundaries, occlusion, eye response
and reflection sensitivity. Real-art smoke additionally exercises motion and ADS;
coverage of downloaded artwork remains local.

## 2026-10-15: closer aimed ocular and rendered magnification proof

The owner requested a larger aiming window. The sample moves the MSR aim pose
forward 40 mm, reducing nominal eye relief from 90 to 50 mm. Lens radius remains
26 mm; the second camera's FOV and the eye-box parameters use the shared relief.
The outer camera FOV stays unchanged. A 5,376-pose installed-rig sweep, including
bob and recoil extremes, has zero clamped targets, 47.063 mm minimum arm reach
margin and 23.810 mm housing clearance beyond the parent near plane.

The smoke now renders a real target through the normal and optical paths and
measures its pixel width after TAA: 11 versus 54 pixels (4.909× on the RTX 3060).
This supplements the projective 5× check and transmitted-input sensitivity control;
a camera focal-ratio assertion by itself cannot prove the displayed magnification.
