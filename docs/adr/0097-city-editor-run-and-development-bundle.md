# ADR-0097: City editor launch and development bundle

- Status: Accepted for the development demo; release gates remain open
- Date: 2026-10-19

## Context

The city sample already owns predicted player input, weapons, local client/server
simulation and fracture rendering. The editor host draws intact authored geometry
and previews scene systems. Reimplementing weapons inside that host would fork the
game. The existing verified export profile covers hello-game, not the city assets.

## Decision

A saved city uses the same block assembly and city expansion as the generated game.
The generator refuses overwrites. Strict runtime load registers the city's lamp
and barrier patterns when the saved map references them, without expanding it again.
The game-specific editor host previews those same procedural pattern volumes.

The GUI accepts an explicit game executable and repeated literal argv entries. Run
game freezes edits while saving, waits for a successful acknowledgement for the
requested document, then launches the runtime with its absolute scene path. One
save is in flight; stale acknowledgements are drained before handling new actions.
A failed/disconnected save cannot launch. Save As adopts its path only on success.
The separate game has its own input, visible launch/status errors and output log;
closing the editor leaves it running. Preview remains a distinct simulation path.

Scene authors own placements, roles and asset identities. Palette and sample-host
preparers mark renderer registry references with the existing DerivedComponents
mechanism. These references are rebuilt on reopen; saving them made process-local
IDs survive into gameplay and left intact preview meshes behind fracture leaves.
Legacy directly authored references on untagged entities retain existing behavior.

The Linux x86_64 city development exporter reuses the binary provenance, payload
integrity, ABI and authentic notice checks under an explicit city profile. It adds
all prepared weapon/arms content and art attribution. A copied executable resolves
content beside itself; an explicit invalid root refuses without checkout fallback.
No-argument copied binaries start Play. Export never replaces an existing folder.

## Consequences

The editor and shipped game share authored map content and game code. Preview does
not possess the full player. Runtime camera/spawn policy remains sample-owned;
editor navigation currently changes the saved scene camera, not that spawn policy.
The artifact is a working development folder, not a production installer or proof
of clean second-machine compatibility. Performance and collapse gates stay open.

## Alternatives considered

Baking preview MeshRef/MaterialRef values into the map was rejected: registry IDs
are not content identities. A second weapon runtime in the editor was rejected.
Protocol save request IDs remain useful for concurrent clients; this GUI has one
serialized owner and no concurrent save requests, so a wire change is unnecessary.
