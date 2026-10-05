# Architecture Decision Records (ADRs)

An **ADR** captures one significant technical decision: the context we were in, what we
decided, and the consequences we accepted. They explain *why the engine is the way it
is* to anyone who arrives later — including our future selves.

## Rules

- **Append-only.** Never rewrite history. If a decision changes, write a *new* ADR that
  supersedes the old one, and mark the old one `Superseded by ADR-XXXX`.
- **Numbered** sequentially: `NNNN-short-kebab-title.md`.
- **Small and honest.** Record the real trade-offs, including what we gave up.

## Format

```
# ADR-NNNN: Title
- Status: Proposed | Accepted | Superseded by ADR-XXXX
- Date: YYYY-MM-DD
## Context      — the forces at play, constraints, what we knew.
## Decision     — what we chose, stated plainly.
## Consequences — what follows (good and bad), what we now must live with.
## Alternatives considered — what else we weighed, and why we passed.
```

## Index

- [ADR-0001](0001-cpp-core-rust-tooling.md) — C++ core + Rust tooling
- [ADR-0002](0002-vulkan-first-rhi.md) — Vulkan-first behind a thin RHI
- [ADR-0003](0003-apache-2-license.md) — Apache-2.0 license
- [ADR-0004](0004-math-conventions.md) — math conventions (float, column-major, RH, Vulkan clip space)
- [ADR-0005](0005-rotation-representation.md) — rotations as unit quaternions (Hamilton, RH, active)
- [ADR-0006](0006-native-windowing.md) — native windowing & input (no GLFW/SDL)
- [ADR-0007](0007-vulkan-backend-bootstrapping.md) — Vulkan backend bootstrapping (volk + VMA, 1.3 baseline)
- [ADR-0008](0008-offline-shader-compilation.md) — offline GLSL→SPIR-V shader compilation
- [ADR-0009](0009-swapchain-and-presentation.md) — swapchain, presentation & frame pacing
- [ADR-0010](0010-textures-and-descriptors.md) — textures, samplers & the descriptor model
- [ADR-0011](0011-depth-attachment.md) — depth attachment & the depth test (pulled ahead of M5)
- [ADR-0012](0012-push-constants.md) — push constants for small per-draw data (MVP, …)
- [ADR-0013](0013-3d-textures.md) — 3-D (volume) textures (field colormaps; pulled ahead of M5)
- [ADR-0014](0014-stencil.md) — stencil state (for the cross-section cap)
- [ADR-0015](0015-imgui-free-ui.md) — a from-scratch, imgui-free immediate-mode UI
- [ADR-0016](0016-editor-is-a-client-of-the-engine.md) — the editor is a client of the engine (+ parallel-path rules)
- [ADR-0017](0017-streaming-codec.md) — the S0 streaming codec (JPEG on the wire, LZ4 for lossless)
- [ADR-0018](0018-ecs-storage-model.md) — ECS storage model (archetype/SoA chunked tables; generational-handle entities; change detection)
- [ADR-0019](0019-render-graph.md) — the render graph (frame-declared passes, virtual resources, graph-owned barriers)
- [ADR-0020](0020-descriptor-model-v2.md) — descriptor model v2 (declared binding layouts; transient per-draw sets from recycled pools)
- [ADR-0021](0021-compute-pipelines.md) — compute pipelines (one handle space, shared bindings, blunt post-dispatch barriers until the graph)
- [ADR-0022](0022-forward-pbr.md) — forward PBR (depth pre-pass → Cook–Torrance into HDR → tonemap; reusable graph passes)
- [ADR-0023](0023-app-fixed-tick-loop.md) — the application framework (a fixed simulation tick decoupled from the render frame; the M11 seam)
- [ADR-0024](0024-asset-model.md) — the asset model (content-hash identity, the RMA1 cooked container, Rust-cooks/C++-loads, schema hashes, deterministic cooks)
- [ADR-0025](0025-gpu-asset-bridge.md) — the GPU asset bridge (cooked mesh/texture → GPU resources on demand; no separate asset_gpu module)
- [ADR-0026](0026-physics-core.md) — the physics core (own rigid-body solver, no Jolt; determinism via a world hash)
- [ADR-0027](0027-convex-hull-shapes.md) — convex hull collision shapes
- [ADR-0028](0028-compound-shapes.md) — compound collision shapes (multi-shape bodies)
- [ADR-0029](0029-destruction-model.md) — the destruction model (cooked fracture pattern → static compound; damage = contact impulse; fracture = a body swap; the C2 event channel with world-space bounds)
- [ADR-0030](0030-streaming-v1.md) — streaming v1 (the S1 protocol; SVT-AV1 video on the wire, LZ4 local fast path)
- [ADR-0031](0031-editor-v1.md) — the editor v1 architecture (a Rust shell that is a client of a live engine)
- [ADR-0032](0032-lighting-v2.md) — lighting v2 (SDF-probe DDGI GI, cascaded + local shadows, clustered-forward many-lights, SSR; the destruction-coupling contracts C1–C6)
- [ADR-0033](0033-networking-v1.md) — networking v1 (server authority; destruction as event-replay + state as snapshots; own UDP transport + reliability layer)
- [ADR-0034](0034-svt-av1-4x-bump.md) — SVT-AV1 4.2.0 (a local Conan recipe, and low-delay's picture-in/picture-out contract)
- [ADR-0035](0035-vision-demo-m12.md) — the vision demo, M12 (a falsifiable "feels right"; the work ledger + a self-gating hardware perf run; the character controller and client-side prediction)
- [ADR-0036](0036-milestone-split-player-and-block.md) — splitting M12 into "The Player" (M12) and "The Block" (M13), at the seam ADR-0035 left for it
- [ADR-0037](0037-authoring-loop-m14.md) — M14 "The Authoring Loop" (open the shipped block in the editor, change it, save it, run the changed scene)
- [ADR-0038](0038-platform-proof-m15.md) — M15 "The Platform Proof" (a game that is not the block, with no engine edit to support it), and where M13's frame-rate debt goes
- [ADR-0039](0039-authored-surfaces-m16.md) — M16 "Authored Surfaces" (the material half of the asset bridge; the mesh→material edge, masked depth, BC formats, one asset runtime)
- [ADR-0040](0040-sky-and-atmosphere.md) — the sky (an analytic background now, a Hillaire-2020 LUT atmosphere later; the sky as a scene component, coupled to the world's sun)
- [ADR-0041](0041-the-visual-bar-m17.md) — M17 "The Visual Bar" (the budget is earned before the bar is spent; the frame becomes attributable first; virtualized geometry deferred to M18)
- [ADR-0042](0042-fluids-track-reopened.md) — Track FL reopens (water, fire, smoke: the particle + heightfield fork now, a unified particle-grid substrate gated for later; all three bricks strictly after M18)
- [ADR-0043](0043-virtualized-geometry-m18.md) — M18 virtualized geometry (rigid opaque mesh clusters, replacement DAG, GPU selection and a visibility path; terrain is M19)
- [ADR-0044](0044-visibility-id-and-indirect-abi.md) — the 64-bit `RG32Uint` visibility ID (version 3; version 2 retired) and how an indirect draw finds its cluster (`gl_DrawID` records, an identity index buffer, a constant draw count)
- [ADR-0045](0045-hosted-front-end-v1.md) — a hosted browser front end becomes committed scope (a separate Rust WebRTC gateway, both surfaces in the first deliverable; **the wire stays AV1** — measurement, not H.264, answers Ampere's missing AV1 encoder, so ADR-0030 is not amended)
- [ADR-0046](0046-exported-games-and-the-blender-boundary.md) — exported games embed the core and pick their mode before a device exists (one binary: play · browser · stream · host · dedicated), generated assets enter through the existing cook as *source*, and **no GPL code is linked** — so Blender stays a separate process or every exported game inherits the GPL
- [ADR-0047](0047-two-machines-and-the-starbase-tier.md) — two machines, two ratified budgets: the workstation (RTX 3060) keeps ADR-0035's bar unchanged and **never serves a hosted session**; starbase (GTX 1060, behind blackStar) is where hosting happens and its bar is ratified from the first pinned run, not invented here
- [ADR-0048](0048-authenticating-the-hosted-front-end.md) — authenticating the hosted front end: invite-gated registration, passkey login and mail-assisted recovery, in a `rime-auth` crate behind a Cargo feature that exported games do not enable (webauthn-rs is MPL-2.0 and ADR-0046 §2 would otherwise put that obligation in every shipped game)
- [ADR-0049](0049-webrtc-transport.md) — the WebRTC implementation is `str0m` (sans-IO, so the gateway keeps its blocking server and no async runtime goes viral), behind a seam that keeps it replaceable
- [ADR-0050](0050-clock-stability-not-clock-pinning.md) — amends ADR-0047 §2: a perf report's precondition is clock-**stability** measured under load during the run (graphics spread ≤2% of median), not clock-**pinning** — which Pascal cannot do, and which the old pre-run idle check could not detect anyway
- [ADR-0051](0051-rime-auth-is-not-built-on-windows.md) — `webauthn-rs` pulls `openssl-sys` unconditionally and the Windows runner has none, so the Windows leg builds the workspace `--exclude rime-auth`; hosting is Linux and an exported game never enables `auth`, and vendoring OpenSSL is the recorded fallback if a Windows regression there ever matters
- [ADR-0052](0052-browser-decode-path.md) — the browser decodes AV1 from a WebRTC video track (str0m's packetizer; PLI becomes the engine's existing `KeyframeRequest`); lossless LZ4 over the DataChannel with a WASM decoder is a LAN-only extra, switched on by an operator flag the gateway enforces, never inferred from the peer's address
- [ADR-0053](0053-media-ingress.md) — media ingress: UDP 50000–50002 forwarded router → blackStar (its first DNAT rule) → CT 122, one port per admission slot, a server-reflexive candidate at the public address learned from `rime.peekstar.eu`'s A record, replies policy-routed back through the edge; TURN over TLS on 443 (coturn behind a PROXY-v2-stripping adapter, per-session credentials) built before deployment, gated on first proving browsers send SNI for `turns:`
- [ADR-0054](0054-input-key-codes-hid.md) — `InputEvent.code` for a key event is a **USB HID usage ID** (Keyboard/Keypad page 0x07), rejecting both frozen `platform::Key` ordinals (the enum's values are documented as arbitrary) and `KeyboardEvent.code` strings (they break the fixed 37-byte payload); alongside it, pointer buttons are DOM `MouseEvent.button` order, `mods` is the `platform::KeyMods` bitmask, and `x`/`y` are stream-frame pixels — no protocol version bump, because no shipped consumer ever read `code`
- [ADR-0055](0055-signing-in-a-browser-with-your-phone.md) — signing a browser in with your phone: the desktop shows a QR, the phone approves with a fresh passkey assertion and shows a code, the desktop redeems it, confirms the approving account and receives one ordinary session — never a credential; not phishing-resistant against a real-time relay, and says so
- [ADR-0056](0056-m20.1-game-definition.md) — `GameDefinition` (m20.1): a game supplies `setup`/`fixed_tick`, an `InputMap`, optional presentation hooks, an autopilot and a state digest; the engine owns the mode, the loop, the device and input. `dedicated` never enters the device factory (counted, and no libvulkan mapped); `browser`/`stream`/`host` parse and refuse as not-yet
- [ADR-0057](0057-m18.5-page-streaming.md) — bounded virtual-geometry page streaming: a fixed-slot page pool behind a CPU policy with an explicit frame clock; a page is resident only once its upload's frame has **retired** (fence polls, in order), a slot is reused only once every frame that read it has retired, and the permanent coarse cut is uploaded blocking and never evicted — so a teleport falls back to a resident ancestor rather than a hole
- [ADR-0058](0058-m18.4-micro-raster.md) — the software micro-triangle rasterizer (M18.4): per-pixel 64-bit atomic min of (depth key, list index) in a storage buffer, merged into the hardware visibility target by the depth test; a bit-identical two-pass 32-bit fallback; the fill rule is top-left in the y-down framebuffer and the CPU oracle is corrected to it
- [ADR-0059](0059-m18.6-replacement-dag-cook.md) — the replacement-DAG cook (Nanite-style: lock group boundaries, QEM to 50%, regroup every level), written one parent per group and one page per simplification so the existing tree-walking selectors swap whole groups; payload v2 adds a monotone per-group LOD sphere, and the reader rejects non-monotone edges
- [ADR-0060 (m19.1)](0060-m19.1-heightfield.md) — the terrain heightfield: a cooked RMA1 kind with quantised u16 samples, scale/offset and the cell triangulation in the header (the cook copies 16-bit sources verbatim); a static-only, world-owned collision shape — one broadphase leaf per tile, a 2-D DDA ray walk with exact per-triangle roots, one-sided per-triangle contacts grouped into normal-coherent patches — with every fallback counted (number provisional)
- [ADR-0061 (m19.2)](0061-m19.2-terrain-convex-queries.md) — convex queries against terrain: per-triangle GJK/EPA/cast against each candidate cell triangle (a heightfield has no support function; a triangle does), one shared `cell_range` with the narrowphase, an explicit counted triangle budget — and depenetration deliberately NOT per-triangle EPA, because the shortest way out of a zero-thickness triangle is often deeper into the ground (measured: 0.4 m down beats 0.6 m up), so it reuses the contact build's "straight back up, never sideways" rule instead
- [ADR-0062 (m19.3)](0062-m19.3-terrain-render.md) — the terrain heightfield **draws from the integers physics collides with**: the cooked u16 samples uploaded verbatim as a new `R16_UNORM` RHI format, a vertex-pulling grid whose stage re-evaluates ADR-0060 §1's `offset + scale * q` (no vertex buffer, so no second height representation exists), ADR-0060's cell diagonal in the index buffer, and faceted `dFdx/dFdy` shading normals that match the faceted normals contacts report; proven structurally against a `rime::physics` raycast at 36 positions within a margin derived from the unorm round trip (measured 9.5 µm against a 1 mm margin), with a flipped cell diagonal rejected at 36/36 probes (number provisional)
- [ADR-0064 (m19.5)](0064-m19.5-terrain-brdf-metallic.md) — terrain shades with the **shared GGX BRDF** (`brdf.glsl`, also used by both forward shaders) and **metallic/roughness join the splat blend** in the same difference form as base colour, keeping the bit-identical anchor; the push block grows to 160 bytes behind a counted `max_push_constant_bytes` check; the ambient is a documented uniform-environment stand-in until the sky SH
- [ADR-0076 (m20.2)](0076-m20.2-content-root-and-bundle.md) — the content root and the bundle: content resolves from `--content`, then `content/` beside the executable, then the source tree **only for a binary still in its build directory** (a moved binary never reads the repository), failing with every path tried; `scripts/export-game.sh` writes executable + content + README and requires system-library-only dependencies with no RUNPATH and no link-time libvulkan; `scripts/export-proof.sh` matches the in-tree digest with `$HOME` and Vulkan hidden by bubblewrap (number provisional)
