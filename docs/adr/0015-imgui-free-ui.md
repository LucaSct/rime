# ADR-0015: A from-scratch, imgui-free immediate-mode UI

- Status: Accepted
- Date: 2026-06-28

## Context

The ICEM viewer (Rime's flagship application, see the ROADMAP appendix) needs an on-screen control
panel: pick/toggle parts of an assembly, drive the cross-section plane, choose a field + colormap, and
scrub animations. Every windowed app and, later, Rime's own **editor** (M9, in Rust) needs the same
ingredients — text, buttons, sliders, checkboxes, panels, hit-testing. The obvious shortcut is to vendor
**Dear ImGui**. We deliberately do not.

Two reasons. First, a stated project decision (the viewer plan): the UI is **built from scratch on the
RHI** so it becomes reusable groundwork for the editor and so the engine ships no third-party UI runtime.
Second, the teaching goal — Rime is meant to be *read and learned from*, and "how a UI draws itself and
reacts to a mouse" is exactly the kind of thing a from-scratch engine should show plainly.

The constraint: the RHI is still minimal. It has **no alpha blending** (only a `color_write` mask), so a
classic translucent-font-atlas UI is not available as-is.

## Decision

**Build a small immediate-mode UI in the application layer, on the existing RHI — no new RHI feature, no
third-party.**

- **Immediate mode (no retained tree).** Each frame the app calls `begin()`, then
  `panel()/label()/button()/checkbox()/slider()`, then `end()`. Widgets *lay themselves out* (a vertical
  stack in a panel) and *react this frame*, appending coloured/textured quads to one vertex batch.
  Interaction uses the standard **hot/active-item** idiom (a press claims a widget via `active_`, so a
  slider keeps tracking the mouse even when the cursor leaves the track). No widget IDs to register, no
  state to retain between frames beyond "who is being dragged".
- **One pipeline for everything**, drawn as a single overlay pass *over* the 3-D scene
  (`color load_op = Load`, depth off). A vertex carries screen-pixel position, an atlas texcoord, and an
  RGBA colour. Solid widgets set `u < 0` (coverage 1); glyph quads sample the font atlas.
- **Bitmap font + alpha-test instead of blending.** A tiny built-in **5×7 bitmap font** is rasterised
  into one R-coverage atlas (`ui_font.hpp`, glyphs authored as readable string-art). Because the RHI has
  no blending, the shader **alpha-tests** (`discard` where coverage < 0.5): the panel stays opaque, text
  shows the panel through its glyph gaps, and letters are crisp. An **SDF** atlas for smooth scaling +
  real blending is a clean drop-in once the render graph (M5) brings a blend state.
- **Streamed geometry.** The per-frame vertex batch goes into one host-visible vertex buffer via
  `write_buffer` (a fixed, generous capacity; the overlay is tiny).

This lives in the viewer sample for now (`ui.hpp`, `ui_font.hpp`, `ui_render.hpp`,
`shaders/ui.{vert,frag}`); it graduates into an `engine/ui` module — and informs the Rust editor's
immediate-mode layer — when more than one app needs it.

## Consequences

- **+** No third-party UI runtime; the engine stays self-contained and legible. The immediate-mode core
  and the hot/active idiom are reusable by the editor.
- **+** Needs nothing new from the RHI — it rides the depth/push-constant/sampled-texture/`color_write`
  features already there (ADRs 0011/0012/0010), so it did not block on a blend-state brick.
- **−** Alpha-test text is aliased (no sub-pixel coverage) and the bitmap font does not scale smoothly;
  both are fixed by an SDF atlas + blending later. No text shaping/Unicode (ASCII, upper-cased) — enough
  for an engineering control panel, not for localized UI.
- **−** Immediate mode re-emits all UI geometry every frame; trivial at this scale, revisited if the
  editor's UI grows large.

## Proof

`tests/rhi/ui_offscreen_test` checks both halves: the **interaction** logic GPU-free (a button fires on a
click inside it, a checkbox toggles its bound bool, a slider drag sets its value), and the **render**
off-screen (a panel fills its rect, bright text/widget pixels appear — the font atlas works — and an
on-checkbox shows its green tick). End to end, `icem_viewer --assembly <dir>` drives part visibility and
the explode slider from the panel. See `docs/math/ui-text-layout.md`.
