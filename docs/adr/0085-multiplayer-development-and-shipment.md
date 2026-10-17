# ADR-0085: multiplayer development and shipment are the product

- Status: Accepted product direction; implementation proceeds in verified bricks
- Date: 2026-10-15
- Decided by: Luca
- Extends ADR-0046, ADR-0056 and ADR-0076; revises ADR-0074's ordering

## Context

The owner explicitly chose multiplayer game development and shipment as Rime's
purpose: native platform binaries, including PS4 and Xbox as goals, dedicated
hosting, browser play, and optional browser development for players to change
their own game. Battlefield-class multiplayer/destruction and Arma-like physics
are capability ambitions for both development and shipped games.

Linux/Windows/macOS builds, authoritative replication and prediction, a native
streaming gateway, and a standalone Linux hello-game bundle already exist. They
do not establish production player capacity, console support, a native browser
runtime, or an exported listening multiplayer server. GameDefinition's browser,
stream and host modes still refuse as unimplemented.

## Decision

1. Development and shipment share the game simulation, physics, authority and
   content pipeline. Advanced runtime features must not depend on an editor.
2. Each supported target produces its own native binary/package. The export set
   includes compatible server artifacts; individual platform executables need
   only expose profiles supported on that target. Dedicated mode selects its
   profile before graphics initialization.
3. Browser play begins with the existing native streaming seam. Native WebAssembly
   is a separate, unimplemented target. Reuse requires an audit of authentication,
   session cleanup, resource limits, compatibility and deployment behavior.
4. Browser development is explicitly creator-enabled and server-enforced. Player
   edits form isolated forks/overlays, pass validation and resource limits, and
   become immutable versions before publication to shared multiplayer sessions.
   Untrusted native code never runs in a trusted production server.
5. Every delivery brick includes build/test automation, identifiable artifacts,
   negative controls and operational evidence appropriate to that brick. Capacity,
   compatibility and support claims require measured envelopes on named hardware.
6. PS4 and Xbox have separate blocked target gates until actual SDK/toolchain,
   hardware, runtime, package and platform requirements can be tested. Their names
   in the vision do not make an executable available.

The order is: current playable repair; auditable Linux packaging; exported
multiplayer with separate server/client processes; capacity and operations;
further platform exports and browser play; optional player authoring. Unfinished
performance/editor gates stay visible and block the capabilities they protect.
This supersedes ADR-0074's requirement to finish general editor work before M20.
The bounded next brick is specified in [the delivery plan](../design/multiplayer-shipping.md).

## Consultation and consequences

Astra independently recommended a small verified Linux packaging brick before
network launcher changes, so later multiplayer tests refer to identifiable
artifacts. We agree, while retaining the existing GameDefinition, export records
and content-root behavior. We decline to make a general package-manager framework
or require another approval for the per-target export-set interpretation: it
implements the owner's existing platform and hosting intent.

The consultation cost $0.16343. Its recommendations are advisory; implementation,
source review and measured verification remain the implementation owner's work.
Unsigned content hashes identify bytes and do not authenticate a publisher.
No existing sample or unit test establishes Battlefield/Arma feature parity.
