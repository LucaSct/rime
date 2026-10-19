# ui-helper report — tools/editor/src/gui/player.rs (new, committed on base 7be38d2)

**Compile/test status:** the file cannot compile in this worktree (no PlayControl wire, no `Input::KeyDown/KeyUp`). Instead I compiled and ran it in a throwaway harness (`harness/`, stub `rime_protocol` + stub `Input`/`Outbound`, real egui 0.31.1, offline): **15/15 tests pass**. Real-crate compile, clippy, and `-fsyntax-only` clang are deferred to root. The stubs assume `PlayControl{action,session_id}.encode()`, `PlayControlState` fields as specified, and `PlayControlAction: Copy`-free use (I only move it).

## API
As specified: `PlayerControl` (Default), `prepare`, `viewport`, `captured`, `release`, `send_control`. All `pub(super)`.
Deviations/additions (all `pub(super)`, for tests/root):
- `request_capture(state,pos,rect,out)->bool` and `step(&FrameInput,state,connected,blocked,out)` (deterministic core of `prepare`), `FrameInput`.
- `release` uses the tracked session, falling back to `state.session_id`.

## Behaviour notes / judgment calls
- Capture is requested only on a primary press with `response.hovered()` AND `rect.contains(pos)`, window focused, `usable` (set by the last `prepare`), phase Playing, status Active, and **backend `captured` currently false** (a stale true is never treated as an ack).
- Ack = matching session + `captured==true` while pending. Ack frame's own motion is dropped; virtual origin sends `PointerMove{0,0}`.
- Every drop (Esc, focus loss, pause, stop, mode/session change, refusal, disconnect, block, 300-frame pending timeout) sends KeyUp per held key, then Release for the tracked session. Backend-initiated drop (`captured` went false) sends KeyUps only. A backend `captured=true` we never requested gets one corrective Release per session.
- Esc is consumed (`consume_key`) only while captured or pending.
- "Modifiers cause release": Ctrl/Cmd/Alt lifts movement keys (KeyUps) but keeps the capture; Shift is sent as 0xE1 (sprint). I read this as key release, not control release — change if you meant otherwise.
- Look: `pointer.motion()` preferred, `pointer.delta()` fallback; non-finite ignored, ±2000/frame/axis, virtual clamp ±1e9, sends only on integer change.
- Cursor: `CursorGrab::Locked` + `CursorVisible(false)` on capture, `None`/visible on release. Locked is unsupported on some platforms/X11 (winit may log an error); no Confined fallback.
- Hint text painted top-left of the image rect.

## Tests (15, in-file)
ack-before-forward; zero origin + key edges/no repeat; look accumulation/NaN/bound; Esc order (Up…, Release) and no recapture on stale flag; modifier lift; pause/new-session/refusal/stop/disconnect/blocked/no-state release; focus loss via real `egui::Context::run` + `prepare`; no recapture on resume; refusal while pending; pending timeout; letterbox/unfocused/blocked/wrong-phase/stale-flag clicks; stray backend capture; origin reset per capture; explicit `release`; `viewport` ownership.
## Not tested
`consume_key(Escape)` through egui and the real `viewport` click path (needs pointer events + layout); actual cursor lock on a window; real wire encoding.
