# ADR-0098: Editor navigation and environment presentation V2

- Status: Accepted for the development editor; release gates remain open
- Date: 2026-10-19
- Extends: ADR-0090, ADR-0095, ADR-0096 and ADR-0097

## Context

The native editor moves the scene camera through ordinary component edits. The
city Preview callback also caches a fly camera and previously rewrote its old
pose every Play tick, undoing editor movement. The editor itself seeded its pose
only once, so Stop or inspector edits could leave navigation at an obsolete pose.
The streamed image was stretched across the dock, misrepresenting its lens.

Water detail previously switched normal maps at exactly zero liquid fraction.
Fire authored smoke and light without a visible flame, while smoke used a smooth
disc and constant grey. Thermal references required numeric editor identities.

## Decision

The city fly camera accepts an externally changed world pose before processing
runtime input. The native camera reconciles acknowledged versus pending poses,
rebases for explicit author edits and phase/camera changes, and requests a fresh
snapshot when navigation ends. It preserves navigation during Play. Existing
protocol messages remain sufficient; uncertain old poses are treated as pending
until an acknowledgment, rather than inventing an edit revision.

The 960×540 stream fits inside the panel with its original aspect. The same fitted
rectangle owns image painting, picking, gizmo rays and navigation. Letterbox bars
cannot begin a scene gesture; an existing drag still releases across them.

The phase-detail owner precomputes a single static frost normal plus 32 frames for
each of eight liquid levels, and nine tint maps. Slopes combine static multiscale
frost with periodic directional water detail. Map selection rounds liquid mass
fraction to eighths; near zero and fully frozen select the same static map.
All 266 immutable 128² mip chains use 23,243,080 bytes, excluding driver overhead.
Accepted thermal time remains the clock. No thermal equation, mass, geometry,
collision or GPU ABI changes. Partial resource creation retains factor fallback.

Effects expose at most three flame rows per admitted source (48 total), using
accepted simulation duration and source identity. A lazy Flame mode reuses the
32-byte particle and 96-byte push interfaces. Flames emit HDR before alpha smoke;
both test and preserve depth without writing it. Off/empty paths declare no pass.
Removal, disable, extinguish and Stop remove flame/light immediately; old smoke
keeps its lifetime. Smoke uses smooth procedural density, lifetime fades and
bounded approximate illumination from the existing fire rows. Original additive
dust/muzzle shading remains unchanged.

The inspector recognizes exact ThermalSource.cell and ThermalContact.first/second
fields with the existing reflected Entity representation. It offers ThermalCell
targets with readable labels, explicit null, dangling-reference display and exact
numeric fallback. The ordinary command, reference validation and undo/save paths
retain ownership; this adds no schema or wire encoding.

## Consequences

The editor can inspect running effects without camera ownership fighting. Both
client latency tests and a real GUI/host Play/Pause/Stop/save proof are required.
Rendering is still cosmetic: water has no displacement/transport, fire has no
combustion authority, and smoke has hard scene intersections and no motion vectors.
Phase quantization is a presentation approximation, not continuous phase physics.
The larger immutable atlas has a documented startup/memory cost. Adaptive stream
resolution, possession, prefabs and broader gameplay remain separate work. No
performance, collapse or multiplayer release gate closes through these changes.
