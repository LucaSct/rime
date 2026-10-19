// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Player mode inside the editor's Preview: walk, look and jump through the scene.
//!
//! The editor never *decides* that the mouse is captured. Clicking only **asks** the engine for
//! capture; keys and look are forwarded, and the cursor is hidden and locked, only after the engine
//! answers with a matching `captured` acknowledgement for the same session. If we inferred capture
//! from the click, a refused or stale request would leave the user steering a world that ignores
//! them (or, worse, a hidden cursor with nothing listening). The acknowledgement is the single
//! source of truth; everything else here is bookkeeping to *give capture back* promptly.
//!
//! Keyboard and mouse are sent as **levels with differential edges**: one `KeyDown` when a key
//! becomes held and one `KeyUp` when it stops, never per-frame repeats. Look is sent as an absolute
//! *virtual* pointer position that starts at (0, 0) on every capture and accumulates raw mouse
//! motion; the host turns consecutive positions back into relative motion. No desktop coordinates
//! ever cross the wire, so a warped or re-centred cursor cannot cause a view jump.

use std::sync::mpsc::Sender;

use eframe::egui;
use rime_protocol::{EditorMessage, PlayControl, PlayControlAction, PlayControlState};

use super::protocol_input::Input;
use super::session::Outbound;

// Backend state encodings (mirrors `PlayControlState`'s documented u8 fields).
const MODE_PLAYER: u8 = 1;
const PHASE_EDIT: u8 = 0;
const PHASE_PLAYING: u8 = 1;
const STATUS_ACTIVE: u8 = 1;

// USB HID usage codes, the same numbering the native controller reads.
const HID_A: u32 = 0x04;
const HID_D: u32 = 0x07;
const HID_S: u32 = 0x16;
const HID_W: u32 = 0x1A;
const HID_SPACE: u32 = 0x2C;
const HID_SHIFT: u32 = 0xE1;

/// A capture request unanswered for this many `prepare` calls is abandoned (and cancelled with a
/// Release). Counted in frames, not wall-clock time, so state never depends on a timer.
const PENDING_FRAME_LIMIT: u32 = 300;
/// One frame's raw mouse motion is clamped to this many units per axis, so a driver glitch cannot
/// spin the camera.
const MAX_STEP: f64 = 2000.0;
/// The virtual pointer is clamped well inside i32 so accumulation can never overflow.
const MAX_VIRTUAL: f64 = 1.0e9;

/// Everything `step` reads from egui for one frame, gathered separately so the state machine can
/// be driven deterministically by tests.
#[derive(Default, Clone)]
pub(super) struct FrameInput {
    /// The OS window has keyboard focus.
    pub focused: bool,
    /// Escape was pressed this frame (and consumed from egui).
    pub escape: bool,
    /// HID codes that should be held right now (already empty while a shortcut modifier is down).
    pub desired: Vec<u32>,
    /// Raw mouse motion this frame, if any source reported one.
    pub motion: Option<(f32, f32)>,
}

struct Pending {
    session: u64,
    frames: u32,
}

#[derive(Default)]
pub(super) struct PlayerControl {
    /// Session the pending request / live capture belongs to.
    session: Option<u64>,
    pending: Option<Pending>,
    captured: bool,
    /// HID codes we have sent a `KeyDown` for and not yet a `KeyUp`.
    held: Vec<u32>,
    virt: (f64, f64),
    last_sent: (i32, i32),
    /// The cursor is currently hidden and locked by us.
    cursor_locked: bool,
    /// Player mode is live and nothing blocks input (set each `prepare`).
    usable: bool,
    focused: bool,
    /// Session we already sent a corrective Release for, so a stale backend capture is answered once.
    stray_released: Option<u64>,
}

/// Does Player mode own the viewport? Only mode Player with a running or paused scene.
fn owns_viewport(state: &PlayControlState) -> bool {
    state.mode == MODE_PLAYER && state.phase != PHASE_EDIT
}

fn can_capture(state: &PlayControlState) -> bool {
    owns_viewport(state) && state.phase == PHASE_PLAYING && state.status == STATUS_ACTIVE
}

pub(super) fn send_control(
    out: &Sender<Outbound>,
    state: &PlayControlState,
    action: PlayControlAction,
) {
    send_action(out, state.session_id, action);
}

fn send_action(out: &Sender<Outbound>, session_id: u64, action: PlayControlAction) {
    let _ = out.send(Outbound::Editor {
        msg: EditorMessage::PlayControl,
        payload: PlayControl { action, session_id }.encode(),
    });
}

impl PlayerControl {
    /// Whether the engine has acknowledged capture and we are forwarding input.
    pub(super) fn captured(&self) -> bool {
        self.captured
    }

    /// Run once per frame before global shortcuts. Handles focus loss, blocking, Escape, keys and
    /// look even if the viewport panel is not drawn this frame.
    pub(super) fn prepare(
        &mut self,
        ctx: &egui::Context,
        state: Option<&PlayControlState>,
        connected: bool,
        blocked: bool,
        out: &Sender<Outbound>,
    ) {
        let (focused, held_modifier, motion) = ctx.input(|i| {
            let m = i.modifiers;
            let motion = i
                .pointer
                .motion()
                .or_else(|| Some(i.pointer.delta()).filter(|d| *d != egui::Vec2::ZERO))
                .map(|v| (v.x, v.y));
            (
                i.viewport().focused.unwrap_or(true),
                m.command || m.ctrl || m.alt,
                motion,
            )
        });
        // Escape is consumed only while we hold (or are about to hold) the mouse, so the gizmo
        // shortcut elsewhere keeps working in plain Edit mode.
        let escape = (self.captured || self.pending.is_some())
            && ctx.input_mut(|i| i.consume_key(i.modifiers, egui::Key::Escape));
        let mut desired = Vec::new();
        if self.captured && !held_modifier {
            ctx.input(|i| {
                for (key, code) in [
                    (egui::Key::W, HID_W),
                    (egui::Key::A, HID_A),
                    (egui::Key::S, HID_S),
                    (egui::Key::D, HID_D),
                    (egui::Key::Space, HID_SPACE),
                ] {
                    if i.key_down(key) {
                        desired.push(code);
                    }
                }
                if i.modifiers.shift {
                    desired.push(HID_SHIFT);
                }
            });
        }
        let frame = FrameInput {
            focused,
            escape: escape || held_modifier,
            desired,
            motion,
        };
        self.step(&frame, state, connected, blocked, out);
        self.sync_cursor(ctx);
        if self.captured || self.pending.is_some() {
            ctx.request_repaint();
        }
    }

    /// Draw the controls hint and request capture on a click inside the image. Returns true while
    /// Player mode owns the viewport (even when the mouse is not captured), so the caller skips
    /// the legacy fly camera, picking and gizmo.
    pub(super) fn viewport(
        &mut self,
        ui: &mut egui::Ui,
        response: &egui::Response,
        rect: egui::Rect,
        state: Option<&PlayControlState>,
        out: &Sender<Outbound>,
    ) -> bool {
        let Some(state) = state.filter(|s| owns_viewport(s)) else {
            return false;
        };
        let pressed = ui.input(|i| {
            i.pointer
                .primary_pressed()
                .then(|| i.pointer.interact_pos())
                .flatten()
        });
        // `response` covers exactly the fitted image, but the rect check is what keeps letterbox
        // bars from ever starting capture; it does not rely on the caller's hit-testing.
        if let Some(pos) = pressed.filter(|_| response.hovered()) {
            self.request_capture(state, pos, rect, out);
        }
        let text = if self.captured {
            "Esc to release the mouse"
        } else if state.phase != PHASE_PLAYING {
            "Paused"
        } else if state.status == STATUS_ACTIVE {
            "Click to play  ·  WASD move  ·  Space jump  ·  Esc releases the mouse"
        } else {
            "Player mode is unavailable"
        };
        // The city sky can be nearly white. Keep the capture instruction readable over
        // either sky or ground, and wrap it inside a narrow docked viewport.
        let color = egui::Color32::WHITE;
        let galley = ui.painter().layout(
            text.to_owned(),
            egui::FontId::proportional(13.0),
            color,
            (rect.width() - 28.0).max(1.0),
        );
        let backing = egui::Rect::from_min_size(
            rect.left_top() + egui::vec2(8.0, 8.0),
            galley.size() + egui::vec2(12.0, 8.0),
        );
        ui.painter()
            .rect_filled(backing, 3.0, egui::Color32::from_black_alpha(190));
        ui.painter()
            .galley(backing.min + egui::vec2(6.0, 4.0), galley, color);
        true
    }

    /// Give everything back now (Stop, new engine, mode change): key-ups, then Release, then a
    /// visible, ungrabbed cursor.
    pub(super) fn release(
        &mut self,
        ctx: &egui::Context,
        state: Option<&PlayControlState>,
        out: &Sender<Outbound>,
    ) {
        if self.session.is_none() {
            self.session = state.map(|s| s.session_id);
        }
        if self.captured || self.pending.is_some() {
            self.drop_all(out, true);
        }
        self.sync_cursor(ctx);
    }

    /// Ask the engine for capture if the press is inside the image and Player mode can accept it.
    /// Returns whether a request was sent. Never marks the mouse captured.
    pub(super) fn request_capture(
        &mut self,
        state: &PlayControlState,
        pos: egui::Pos2,
        rect: egui::Rect,
        out: &Sender<Outbound>,
    ) -> bool {
        // `state.captured` already true means a previous capture's flag has not cleared yet;
        // treating that as an acknowledgement would be inference, so wait for it to drop.
        if !self.usable
            || !self.focused
            || !can_capture(state)
            || state.captured
            || self.captured
            || self.pending.is_some()
            || !rect.contains(pos)
        {
            return false;
        }
        self.session = Some(state.session_id);
        self.pending = Some(Pending {
            session: state.session_id,
            frames: 0,
        });
        self.stray_released = None;
        send_action(out, state.session_id, PlayControlAction::Capture);
        true
    }

    /// The deterministic core of `prepare`.
    pub(super) fn step(
        &mut self,
        f: &FrameInput,
        state: Option<&PlayControlState>,
        connected: bool,
        blocked: bool,
        out: &Sender<Outbound>,
    ) {
        self.focused = f.focused;
        let owned = state.filter(|s| connected && !blocked && owns_viewport(s));
        self.usable = owned.is_some();

        if self.captured || self.pending.is_some() {
            let keep = owned.is_some_and(|s| {
                Some(s.session_id) == self.session
                    && s.phase == PHASE_PLAYING
                    && s.status == STATUS_ACTIVE
            }) && f.focused;
            if !keep || f.escape {
                self.drop_all(out, true);
                return;
            }
            let s = owned.expect("keep implies owned");
            // The engine dropped capture on its own: follow it, nothing to tell it back.
            if self.captured && !s.captured {
                self.drop_all(out, false);
                return;
            }
            if let Some(p) = self.pending.as_mut() {
                if s.captured && s.session_id == p.session {
                    self.pending = None;
                    self.captured = true;
                    // Every capture starts a fresh virtual origin; the ack frame's own motion
                    // belonged to the click and is deliberately not counted.
                    self.virt = (0.0, 0.0);
                    self.last_sent = (0, 0);
                    let _ = out.send(Outbound::Input(Input::PointerMove { x: 0, y: 0 }));
                    return;
                }
                p.frames += 1;
                if p.frames > PENDING_FRAME_LIMIT {
                    self.drop_all(out, true);
                }
                return;
            }
            self.forward(f, out);
            return;
        }

        // Not ours, yet the engine says captured: answer once with a Release so it cannot keep
        // steering from a capture we never asked for.
        if let Some(s) = owned {
            if s.captured && self.stray_released != Some(s.session_id) {
                self.stray_released = Some(s.session_id);
                send_action(out, s.session_id, PlayControlAction::Release);
            }
        }
    }

    fn forward(&mut self, f: &FrameInput, out: &Sender<Outbound>) {
        let mut want = f.desired.clone();
        want.sort_unstable();
        want.dedup();
        let ups: Vec<u32> = self
            .held
            .iter()
            .copied()
            .filter(|c| !want.contains(c))
            .collect();
        for code in ups {
            let _ = out.send(Outbound::Input(Input::KeyUp { code }));
        }
        for &code in &want {
            if !self.held.contains(&code) {
                let _ = out.send(Outbound::Input(Input::KeyDown { code }));
            }
        }
        self.held = want;

        if let Some((dx, dy)) = f.motion {
            for (axis, d) in [(0, dx), (1, dy)] {
                if !d.is_finite() {
                    continue;
                }
                let d = f64::from(d).clamp(-MAX_STEP, MAX_STEP);
                let v = if axis == 0 {
                    &mut self.virt.0
                } else {
                    &mut self.virt.1
                };
                *v = (*v + d).clamp(-MAX_VIRTUAL, MAX_VIRTUAL);
            }
            let pos = (self.virt.0.round() as i32, self.virt.1.round() as i32);
            if pos != self.last_sent {
                self.last_sent = pos;
                let _ = out.send(Outbound::Input(Input::PointerMove { x: pos.0, y: pos.1 }));
            }
        }
    }

    /// KeyUp for every key actually held, then (optionally) Release, then forget the capture.
    fn drop_all(&mut self, out: &Sender<Outbound>, send_release: bool) {
        for code in std::mem::take(&mut self.held) {
            let _ = out.send(Outbound::Input(Input::KeyUp { code }));
        }
        if let (true, Some(session)) = (send_release, self.session) {
            send_action(out, session, PlayControlAction::Release);
            self.stray_released = Some(session);
        }
        self.captured = false;
        self.pending = None;
        self.session = None;
    }

    fn sync_cursor(&mut self, ctx: &egui::Context) {
        if self.captured == self.cursor_locked {
            return;
        }
        self.cursor_locked = self.captured;
        ctx.send_viewport_cmd(egui::ViewportCommand::CursorGrab(if self.captured {
            // X11 and Windows support confinement where winit's locked mode may be unavailable.
            // Raw motion still drives look; confinement is not desktop-coordinate navigation.
            if cfg!(target_os = "windows")
                || (cfg!(target_os = "linux")
                    && (std::env::var_os("WAYLAND_DISPLAY").is_none()
                        || std::env::var("WINIT_UNIX_BACKEND").as_deref() == Ok("x11")))
            {
                egui::CursorGrab::Confined
            } else {
                egui::CursorGrab::Locked
            }
        } else {
            egui::CursorGrab::None
        }));
        ctx.send_viewport_cmd(egui::ViewportCommand::CursorVisible(!self.captured));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc::{channel, Receiver};

    const SESSION: u64 = 7;

    fn st(phase: u8, status: u8, captured: bool, session: u64) -> PlayControlState {
        PlayControlState {
            supported_modes: 3,
            mode: 1,
            phase,
            status,
            captured,
            session_id: session,
            tick_count: 0,
            reason: String::new(),
        }
    }

    fn playing(captured: bool) -> PlayControlState {
        st(PHASE_PLAYING, STATUS_ACTIVE, captured, SESSION)
    }

    fn frame() -> FrameInput {
        FrameInput {
            focused: true,
            ..Default::default()
        }
    }

    fn rect() -> egui::Rect {
        egui::Rect::from_min_size(egui::pos2(100.0, 50.0), egui::vec2(400.0, 300.0))
    }

    #[derive(Debug, PartialEq)]
    enum Ev {
        Down(u32),
        Up(u32),
        Move(i32, i32),
        Capture(u64),
        Release(u64),
        Other,
    }

    fn drain(rx: &Receiver<Outbound>) -> Vec<Ev> {
        let mut v = Vec::new();
        while let Ok(o) = rx.try_recv() {
            v.push(match o {
                Outbound::Input(Input::KeyDown { code }) => Ev::Down(code),
                Outbound::Input(Input::KeyUp { code }) => Ev::Up(code),
                Outbound::Input(Input::PointerMove { x, y }) => Ev::Move(x, y),
                Outbound::Editor {
                    msg: EditorMessage::PlayControl,
                    payload,
                } => {
                    if payload
                        == (PlayControl {
                            action: PlayControlAction::Capture,
                            session_id: SESSION,
                        })
                        .encode()
                    {
                        Ev::Capture(SESSION)
                    } else if payload
                        == (PlayControl {
                            action: PlayControlAction::Release,
                            session_id: SESSION,
                        })
                        .encode()
                    {
                        Ev::Release(SESSION)
                    } else {
                        Ev::Other
                    }
                }
                _ => Ev::Other,
            });
        }
        v
    }

    /// Click, deliver the engine's ack, and return a player that is truly captured.
    fn captured_player() -> (PlayerControl, Sender<Outbound>, Receiver<Outbound>) {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        assert!(p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        p.step(&frame(), Some(&playing(true)), true, false, &tx);
        assert!(p.captured());
        drain(&rx);
        (p, tx, rx)
    }

    #[test]
    fn prepare_releases_capture_for_shortcut_modifiers_and_shift_escape() {
        for modifiers in [
            egui::Modifiers::CTRL,
            egui::Modifiers::ALT,
            egui::Modifiers::COMMAND,
            egui::Modifiers::SHIFT,
        ] {
            let (mut p, tx, rx) = captured_player();
            p.step(
                &FrameInput {
                    desired: vec![HID_W],
                    ..frame()
                },
                Some(&playing(true)),
                true,
                false,
                &tx,
            );
            assert_eq!(drain(&rx), vec![Ev::Down(HID_W)]);
            let mut input = egui::RawInput {
                modifiers,
                ..Default::default()
            };
            // Shift is a movement modifier, so it releases only when Escape is pressed.
            // The actual egui consume_key path must accept that modified Escape.
            if modifiers == egui::Modifiers::SHIFT {
                input.events.push(egui::Event::Key {
                    key: egui::Key::Escape,
                    physical_key: None,
                    pressed: true,
                    repeat: false,
                    modifiers,
                });
            }
            let ctx = egui::Context::default();
            let _ = ctx.run(input, |ctx| {
                p.prepare(ctx, Some(&playing(true)), true, false, &tx);
            });
            assert!(!p.captured());
            assert_eq!(drain(&rx), vec![Ev::Up(HID_W), Ev::Release(SESSION)]);
        }
    }

    #[test]
    fn click_asks_but_forwards_nothing_until_the_engine_acknowledges() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        assert!(p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        assert_eq!(drain(&rx), vec![Ev::Capture(SESSION)]);
        // Keys and look arrive while the request is still unanswered.
        let held = FrameInput {
            desired: vec![HID_W],
            motion: Some((30.0, 5.0)),
            ..frame()
        };
        p.step(&held, Some(&playing(false)), true, false, &tx);
        assert!(!p.captured());
        assert_eq!(drain(&rx), vec![]);
        // An ack for another session is not ours.
        p.step(&held, Some(&st(1, 1, true, SESSION + 1)), true, false, &tx);
        assert!(!p.captured());
        drain(&rx);
    }

    #[test]
    fn ack_starts_a_zero_origin_then_keys_are_edges_not_repeats() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx);
        drain(&rx);
        p.step(&frame(), Some(&playing(true)), true, false, &tx);
        assert_eq!(drain(&rx), vec![Ev::Move(0, 0)]);

        let w = FrameInput {
            desired: vec![HID_W, HID_SHIFT],
            ..frame()
        };
        p.step(&w, Some(&playing(true)), true, false, &tx);
        assert_eq!(drain(&rx), vec![Ev::Down(HID_W), Ev::Down(HID_SHIFT)]);
        for _ in 0..3 {
            p.step(&w, Some(&playing(true)), true, false, &tx);
        }
        assert_eq!(drain(&rx), vec![], "a held key must not repeat");
        let d = FrameInput {
            desired: vec![HID_D],
            ..frame()
        };
        p.step(&d, Some(&playing(true)), true, false, &tx);
        assert_eq!(
            drain(&rx),
            vec![Ev::Up(HID_W), Ev::Up(HID_SHIFT), Ev::Down(HID_D)]
        );
    }

    #[test]
    fn look_accumulates_into_integer_virtual_coordinates_and_ignores_garbage() {
        let (mut p, tx, rx) = captured_player();
        let mv = |m| FrameInput {
            motion: Some(m),
            ..frame()
        };
        p.step(&mv((4.0, -2.0)), Some(&playing(true)), true, false, &tx);
        p.step(&mv((3.0, 0.0)), Some(&playing(true)), true, false, &tx);
        p.step(
            &mv((f32::NAN, f32::INFINITY)),
            Some(&playing(true)),
            true,
            false,
            &tx,
        );
        p.step(&mv((0.2, 0.0)), Some(&playing(true)), true, false, &tx);
        assert_eq!(drain(&rx), vec![Ev::Move(4, -2), Ev::Move(7, -2)]);
        p.step(&mv((1.0e9, 0.0)), Some(&playing(true)), true, false, &tx);
        assert_eq!(drain(&rx), vec![Ev::Move(2007, -2)], "one frame is bounded");
    }

    #[test]
    fn escape_releases_every_held_key_before_the_control_release() {
        let (mut p, tx, rx) = captured_player();
        let keys = FrameInput {
            desired: vec![HID_W, HID_SPACE],
            ..frame()
        };
        p.step(&keys, Some(&playing(true)), true, false, &tx);
        drain(&rx);
        let esc = FrameInput {
            escape: true,
            desired: vec![HID_W, HID_SPACE],
            ..frame()
        };
        p.step(&esc, Some(&playing(true)), true, false, &tx);
        assert!(!p.captured());
        assert_eq!(
            drain(&rx),
            vec![Ev::Up(HID_W), Ev::Up(HID_SPACE), Ev::Release(SESSION)]
        );
        // The backend flag is still true for a frame: that must not recapture.
        p.step(&keys, Some(&playing(true)), true, false, &tx);
        assert!(!p.captured());
        assert_eq!(
            drain(&rx),
            vec![],
            "stale backend capture is already answered"
        );
    }

    #[test]
    fn modifier_shortcut_empties_the_desired_set_and_lifts_keys() {
        let (mut p, tx, rx) = captured_player();
        let w = FrameInput {
            desired: vec![HID_W],
            ..frame()
        };
        p.step(&w, Some(&playing(true)), true, false, &tx);
        drain(&rx);
        // prepare() builds an empty set while Ctrl/Cmd/Alt is down.
        p.step(&frame(), Some(&playing(true)), true, false, &tx);
        assert_eq!(drain(&rx), vec![Ev::Up(HID_W)]);
        assert!(p.captured());
    }

    #[test]
    fn pause_new_session_refusal_disconnect_and_block_all_release() {
        type Case = (&'static str, Option<PlayControlState>, bool, bool);
        let cases: Vec<Case> = vec![
            ("pause", Some(st(2, 1, true, SESSION)), true, false),
            (
                "new session",
                Some(st(1, 1, true, SESSION + 1)),
                true,
                false,
            ),
            ("refused", Some(st(1, 2, false, SESSION)), true, false),
            ("stopped", Some(st(0, 1, false, SESSION)), true, false),
            ("disconnected", Some(playing(true)), false, false),
            ("blocked", Some(playing(true)), true, true),
            ("no state", None, true, false),
        ];
        for (name, state, connected, blocked) in cases {
            let (mut p, tx, rx) = captured_player();
            let w = FrameInput {
                desired: vec![HID_W],
                ..frame()
            };
            p.step(&w, Some(&playing(true)), true, false, &tx);
            drain(&rx);
            p.step(&w, state.as_ref(), connected, blocked, &tx);
            assert!(!p.captured(), "{name}");
            assert_eq!(
                drain(&rx),
                vec![Ev::Up(HID_W), Ev::Release(SESSION)],
                "{name}"
            );
        }
    }

    #[test]
    fn focus_loss_releases_in_prepare_without_any_viewport_callback() {
        let (tx, rx) = channel();
        let ctx = egui::Context::default();
        let mut p = PlayerControl::default();
        let run =
            |ctx: &egui::Context, focused: bool, p: &mut PlayerControl, s: &PlayControlState| {
                let mut raw = egui::RawInput::default();
                raw.viewports
                    .entry(egui::ViewportId::ROOT)
                    .or_default()
                    .focused = Some(focused);
                let _ = ctx.run(raw, |ctx| p.prepare(ctx, Some(s), true, false, &tx));
            };
        run(&ctx, true, &mut p, &playing(false));
        assert!(p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        run(&ctx, true, &mut p, &playing(true));
        assert!(p.captured());
        drain(&rx);
        run(&ctx, false, &mut p, &playing(true));
        assert!(!p.captured());
        assert_eq!(drain(&rx), vec![Ev::Release(SESSION)]);
    }

    #[test]
    fn no_recapture_on_resume_without_a_fresh_click() {
        let (mut p, tx, rx) = captured_player();
        p.step(&frame(), Some(&st(2, 1, true, SESSION)), true, false, &tx);
        assert!(!p.captured());
        drain(&rx);
        // Resumed; the backend (stale or not) still reports captured.
        for captured in [true, false, true] {
            p.step(&frame(), Some(&playing(captured)), true, false, &tx);
            assert!(!p.captured());
        }
        assert!(!drain(&rx).iter().any(|e| matches!(e, Ev::Capture(_))));
        // A click does the asking, and only an ack captures.
        assert!(p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        assert!(!p.captured());
    }

    #[test]
    fn backend_refusal_while_pending_cancels_the_request() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx);
        drain(&rx);
        p.step(&frame(), Some(&st(1, 2, false, SESSION)), true, false, &tx);
        // Even a late ack for the cancelled request cannot capture.
        p.step(&frame(), Some(&playing(true)), true, false, &tx);
        assert!(!p.captured());
    }

    #[test]
    fn unanswered_request_times_out_by_frame_count() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx);
        drain(&rx);
        for _ in 0..=PENDING_FRAME_LIMIT {
            p.step(&frame(), Some(&playing(false)), true, false, &tx);
        }
        assert_eq!(drain(&rx), vec![Ev::Release(SESSION)]);
    }

    #[test]
    fn letterbox_unfocused_blocked_and_wrong_phase_clicks_never_request() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        let outside = [egui::pos2(99.0, 100.0), egui::pos2(200.0, 351.0)];
        for pos in outside {
            assert!(!p.request_capture(&playing(false), pos, rect(), &tx));
        }
        assert!(!p.request_capture(
            &st(2, 1, false, SESSION),
            egui::pos2(200.0, 100.0),
            rect(),
            &tx
        ));
        assert!(!p.request_capture(
            &st(1, 2, false, SESSION),
            egui::pos2(200.0, 100.0),
            rect(),
            &tx
        ));
        // The backend flag has not cleared yet: that is not permission.
        assert!(!p.request_capture(&playing(true), egui::pos2(200.0, 100.0), rect(), &tx));
        p.step(
            &FrameInput::default(),
            Some(&playing(false)),
            true,
            false,
            &tx,
        );
        assert!(!p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        p.step(&frame(), Some(&playing(false)), true, true, &tx);
        assert!(!p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        assert_eq!(drain(&rx), vec![]);
    }

    #[test]
    fn a_backend_capture_we_never_asked_for_is_released_once() {
        let (tx, rx) = channel();
        let mut p = PlayerControl::default();
        for _ in 0..3 {
            p.step(&frame(), Some(&playing(true)), true, false, &tx);
        }
        assert!(!p.captured());
        assert_eq!(drain(&rx), vec![Ev::Release(SESSION)]);
    }

    #[test]
    fn each_capture_restarts_the_virtual_origin() {
        let (mut p, tx, rx) = captured_player();
        let mv = FrameInput {
            motion: Some((50.0, 50.0)),
            ..frame()
        };
        p.step(&mv, Some(&playing(true)), true, false, &tx);
        p.step(&frame(), Some(&st(2, 1, false, SESSION)), true, false, &tx);
        drain(&rx);
        p.step(&frame(), Some(&playing(false)), true, false, &tx);
        assert!(p.request_capture(&playing(false), egui::pos2(200.0, 100.0), rect(), &tx));
        p.step(&mv, Some(&playing(true)), true, false, &tx);
        assert_eq!(
            drain(&rx),
            vec![Ev::Capture(SESSION), Ev::Move(0, 0)],
            "the ack frame's motion is not counted"
        );
    }

    #[test]
    fn explicit_release_gives_back_pending_and_captured_state() {
        let (mut p, tx, rx) = captured_player();
        let ctx = egui::Context::default();
        p.release(&ctx, Some(&playing(true)), &tx);
        assert!(!p.captured());
        assert_eq!(drain(&rx), vec![Ev::Release(SESSION)]);
    }

    #[test]
    fn viewport_reports_ownership_only_for_player_mode_outside_edit() {
        let ctx = egui::Context::default();
        let (tx, _rx) = channel();
        let mut p = PlayerControl::default();
        let cases = [
            (st(1, 1, false, 1), true),
            (st(2, 1, false, 1), true),
            (st(0, 1, false, 1), false),
            (
                PlayControlState {
                    mode: 0,
                    ..st(1, 1, false, 1)
                },
                false,
            ),
        ];
        for (state, expect) in cases {
            let mut got = None;
            let _ = ctx.run(egui::RawInput::default(), |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    let r = ui.allocate_response(egui::vec2(50.0, 50.0), egui::Sense::click());
                    got = Some(p.viewport(ui, &r, r.rect, Some(&state), &tx));
                });
            });
            assert_eq!(got, Some(expect));
        }
    }
}
