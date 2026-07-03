# ADR-0016: The editor is a client of the engine (and the parallel-path rules)

- Status: Accepted
- Date: 2026-07-03

## Context

Rime's editor (M9, Rust — [ADR-0001](0001-cpp-core-rust-tooling.md)) needs a live 3-D viewport. Two
shapes were possible:

1. **Embed** a renderer inside the Rust editor process — the editor owns a window and calls the
   engine as a library; or
2. **Client / server** — the *engine* runs as a process, renders the viewport, and delivers it to a
   thin editor **shell** over a protocol; input and edits flow back.

We choose (2), the **FrostEd-style** architecture (Frostbite's editor is a client of a running
game). The Rust shell owns docking, inspectors, and the asset browser; the engine renders the
viewport and streams it to the shell; edits return as reflection-described component data.

Why: it keeps the ADR-0001 seam honest (tools never link C++ internals — they talk over a documented
boundary), the editor always shows *exactly* what the engine renders (no second renderer to keep in
sync), and — the decisive reason — **one architecture serves three consumers** at rising transport
sophistication:

| Consumer | Transport / codec | When |
| --- | --- | --- |
| Dev streaming (headless server → dev machine) | TCP over LAN/VPN, cheap compression | S0, now |
| Editor viewport | local socket / shared memory, near-zero latency | M9 |
| Remote play — a shippable game feature | QUIC/WebRTC, hardware codecs | S1–S3 |

That convergence is what justifies continued investment in the Frostlens-derived viewport toolkit
(orbit camera, immediate-mode UI, off-screen capture): it is the **in-engine half of the editor**.

## Decision

**The editor is a client of a live engine process.** The engine renders the viewport and delivers it
over a **versioned protocol** (Track S; transport & codec choices in the future ADR-0017); the Rust
shell presents it and sends input + edits back. Component inspection/editing rides `core`'s
reflection, extended at M4 so components register **once** and are serialisable now,
editor-inspectable later.

### Parallel-path rules

The ICEM viewer (`samples/03-icem-viewer`) doubles as a proving ground for editor-viewport tech, so
some engine work happens "off the milestone." To keep that from pulling focus, parallel-path bricks
obey:

1. **Mainline-first.** The active milestone's next brick outranks any parallel-path brick; never two
   consecutive parallel bricks while the milestone has unstarted work.
2. **Scope test.** A parallel-path brick must serve the **editor-viewport / streaming stack**
   (camera, UI widgets/text, picking, gizmos, capture, transport). New CFD/domain views are ICEM
   scope — allowed in `samples/`, lowest priority, and engine changes only via the proven additive
   pattern (additive API + an ADR + an off-screen pixel proof).
3. **Graduation triggers.** `camera.hpp` → `engine/render` at M5; the immediate-mode UI
   (`ui*.hpp`) → `engine/ui` at its second consumer; `render_view` / turntable capture → shared
   infrastructure with S0's frame tap; the STL-loader *pattern* → `engine/assets` at M6.
4. **Dogfood.** The viewer adopts landed milestones (M4: parts as ECS entities; M5: its frame —
   mesh + stencil cap + iso/DVR + UI overlay — expressed as a render graph).

### ICEM ownership

The viewer is conceptually **ICEM's** (a separate computational-engineering project). ICEM should
*consume* Rime as a framework, not grow modules inside this repo. Its `samples/` placement is fine
for now; the domain code migrates to a downstream ICEM repo once Rime ships an SDK / package story
(~M6, when the FFI/C-ABI crate and `rime-cli cook` make install/export real). The two repos share
only *files* (meshes, field binaries), never code.

## Consequences

- **+** One protocol, proven cheaply by S0 (dev streaming) long before M9 — the editor becomes
  assembly, not invention.
- **+** The reflection registry gets three consumers (serialise now, inspect at M9, replicate at
  M11) — one place to get right (see the M4 storage ADR and the Net track).
- **+** Tools never link engine internals (ADR-0001 upheld structurally, not by convention).
- **−** A viewport protocol is now a long-lived, **versioned** interface from S0 — it must be
  versioned from day one and resist gold-plating v0 (a stated Track S risk).
- **−** Viewport latency becomes a real budget (capture + encode + transport). S0 measures
  glass-to-glass and records it; hardware codecs and async readback (S1) bring it down.

## Related

- [ADR-0001](0001-cpp-core-rust-tooling.md) — the C++ core / Rust tooling boundary; this refines
  *how* the editor talks to the engine.
- **Track S** (graphics streaming) and the future **ADR-0017** (transport & codec) — the protocol
  this rides.
- [ROADMAP.md](../ROADMAP.md) — M9 (editor as client), Track S (S0 now, S1+ later).
