// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The engine session behind the docking shell: spawn `rime-engine --editor-host --viewport`, connect
//! over the local socket, and run the wire off the UI thread. A **receiver** thread decodes the
//! schema, snapshot, and streamed frames into shared state the UI reads each repaint; a **sender**
//! thread forwards viewport input. Full-duplex over one socket (the `rime-protocol` contract), the
//! same wire the headless smoke proves — so only what appears on screen is Mac-eyeballed.

use std::io::ErrorKind;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use rime_protocol::{
    AssetEntry, AssetList, Connection, EditorMessage, FrameMessage, InputEvent, InputKind,
    MessageType, PickResult, Schema, Snapshot, SnapshotComponent,
};

use super::protocol_input::Input;

/// Everything the UI sends to the engine over the one shared socket: high-frequency viewport input
/// and low-frequency editor commands, multiplexed onto a single channel so one sender thread drains
/// both in order.
pub enum Outbound {
    /// A viewport pointer event, forwarded as an `InputEvent`.
    Input(Input),
    /// An editor-channel command (a `Command::to_wire` result — set/add/remove/spawn/…).
    Editor {
        msg: EditorMessage,
        payload: Vec<u8>,
    },
}

/// The latest decoded viewport frame — RGBA, top row first, with the geometry and a sequence stamp.
pub struct ViewportFrame {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
    pub seq: u64,
}

/// State shared between the session threads (writers) and the UI (reader). Guarded by one mutex; each
/// side holds it only briefly.
#[derive(Default)]
pub struct SharedState {
    pub connected: bool,
    pub error: Option<String>,
    pub schema: Schema,
    pub snapshot: Snapshot,
    pub assets: Vec<AssetEntry>,
    pub frame: Option<ViewportFrame>,
    pub frames_received: u64,
    pub fps: f32,
    /// The newest un-consumed pick answer (m9.6). The UI `take()`s it each repaint and maps the
    /// entity handle to the outliner selection; if several results land between repaints the
    /// latest wins — each reflects a distinct click, and selection is last-click-wins anyway.
    pub last_pick: Option<PickResult>,
    last_frame_at: Option<Instant>,
}

impl SharedState {
    /// Optimistically patch the mirror after an edit, so the inspector shows the new value this frame
    /// without waiting for a snapshot round-trip. Nothing else mutates the world while editing (that
    /// starts at Play), so the mirror stays truthful; structural changes still resync via a snapshot
    /// request. Updates the component's bytes in place, or inserts it if the entity lacked it (the
    /// undo-of-remove case).
    pub fn apply_optimistic_set(&mut self, key: (u32, u32), type_hash: u64, blob: &[u8]) {
        let Some(entity) = self
            .snapshot
            .entities
            .iter_mut()
            .find(|e| (e.index, e.generation) == key)
        else {
            return;
        };
        if let Some(comp) = entity
            .components
            .iter_mut()
            .find(|c| c.type_hash == type_hash)
        {
            comp.data = blob.to_vec();
        } else {
            entity.components.push(SnapshotComponent {
                type_hash,
                data: blob.to_vec(),
            });
        }
    }
}

pub type Shared = Arc<Mutex<SharedState>>;

/// Owns the spawned engine + the receiver thread; on drop, kills the engine and joins. The sender
/// thread is detached — it exits when the UI drops the input channel.
pub struct EngineSession {
    child: Option<Child>,
    recv_handle: Option<JoinHandle<()>>,
}

impl EngineSession {
    /// Spawn `rime-engine --editor-host --viewport [--assets <manifest>]` and start driving the wire.
    /// Any failure lands in `shared.error` (the status bar shows it) rather than panicking the UI.
    pub fn spawn(
        engine: String,
        assets: Option<String>,
        shared: Shared,
        out_rx: Receiver<Outbound>,
    ) -> Self {
        let socket = unique_socket_path();
        let mut command = Command::new(&engine);
        command.arg("--editor-host").arg(&socket).arg("--viewport");
        if let Some(assets) = &assets {
            command.arg("--assets").arg(assets);
        }
        let child = match command.spawn() {
            Ok(child) => child,
            Err(e) => {
                shared.lock().unwrap().error = Some(format!("spawn '{engine}': {e}"));
                return Self {
                    child: None,
                    recv_handle: None,
                };
            }
        };
        let recv_handle = thread::spawn(move || run_session(&socket, &shared, out_rx));
        Self {
            child: Some(child),
            recv_handle: Some(recv_handle),
        }
    }
}

impl Drop for EngineSession {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        if let Some(handle) = self.recv_handle.take() {
            let _ = handle.join();
        }
    }
}

fn unique_socket_path() -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    std::env::temp_dir().join(format!(
        "rime-editor-gui-{}-{}.sock",
        std::process::id(),
        nanos
    ))
}

fn connect_retry(path: &Path, timeout: Duration) -> std::io::Result<UnixStream> {
    let start = Instant::now();
    loop {
        match UnixStream::connect(path) {
            Ok(stream) => return Ok(stream),
            Err(e) if matches!(e.kind(), ErrorKind::NotFound | ErrorKind::ConnectionRefused) => {
                if start.elapsed() >= timeout {
                    return Err(e);
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(e) => return Err(e),
        }
    }
}

// Connect, handshake, then split the socket into a read half (frames) and a write half (input) — the
// full-duplex use one sender + one receiver, which ProtocolConnection allows.
fn run_session(socket: &Path, shared: &Shared, out_rx: Receiver<Outbound>) {
    let stream = match connect_retry(socket, Duration::from_secs(10)) {
        Ok(stream) => stream,
        Err(e) => {
            shared.lock().unwrap().error = Some(format!("connect: {e}"));
            return;
        }
    };
    let mut conn = Connection::new(stream);
    if let Err(e) = conn.handshake() {
        shared.lock().unwrap().error = Some(format!("handshake: {e}"));
        return;
    }
    let read_stream = conn.into_inner();
    let write_stream = match read_stream.try_clone() {
        Ok(stream) => stream,
        Err(e) => {
            shared.lock().unwrap().error = Some(format!("split socket: {e}"));
            return;
        }
    };
    shared.lock().unwrap().connected = true;

    // Sender thread: viewport input + editor commands → the wire. Detached; it exits when the UI
    // drops the outbound channel.
    let _sender = thread::spawn(move || run_sender(Connection::new(write_stream), out_rx));

    let mut read_conn = Connection::new(read_stream);
    // Drive the wire until the engine disconnects (recv returns Err on close/EOF).
    while let Ok((ty, payload)) = read_conn.recv() {
        handle_message(shared, ty, &payload);
    }
    shared.lock().unwrap().connected = false;
}

fn handle_message(shared: &Shared, ty: MessageType, payload: &[u8]) {
    match ty {
        MessageType::Frame => {
            let Ok(frame) = FrameMessage::decode(payload) else {
                return;
            };
            let Ok(rgba) = frame.decode_pixels() else {
                return;
            };
            let mut s = shared.lock().unwrap();
            let now = Instant::now();
            if let Some(prev) = s.last_frame_at {
                let dt = now.duration_since(prev).as_secs_f32();
                if dt > 0.0 {
                    // Exponential moving average — a steady fps readout, not a jittery instant one.
                    s.fps = if s.fps > 0.0 {
                        0.9 * s.fps + 0.1 / dt
                    } else {
                        1.0 / dt
                    };
                }
            }
            s.last_frame_at = Some(now);
            s.frames_received += 1;
            let seq = s.frames_received;
            s.frame = Some(ViewportFrame {
                width: frame.desc.width,
                height: frame.desc.height,
                rgba,
                seq,
            });
        }
        MessageType::Other(code) if code == EditorMessage::Schema.to_code() => {
            if let Ok(schema) = Schema::decode(payload) {
                shared.lock().unwrap().schema = schema;
            }
        }
        MessageType::Other(code) if code == EditorMessage::Snapshot.to_code() => {
            if let Ok(snapshot) = Snapshot::decode(payload) {
                shared.lock().unwrap().snapshot = snapshot;
            }
        }
        MessageType::Other(code) if code == EditorMessage::AssetList.to_code() => {
            if let Ok(list) = AssetList::decode(payload) {
                shared.lock().unwrap().assets = list.assets;
            }
        }
        MessageType::Other(code) if code == EditorMessage::PickResult.to_code() => {
            if let Ok(pick) = PickResult::decode(payload) {
                shared.lock().unwrap().last_pick = Some(pick);
            }
        }
        _ => {} // an editor->engine type echoed, or something this build predates
    }
}

fn run_sender(mut conn: Connection<UnixStream>, out_rx: Receiver<Outbound>) {
    while let Ok(out) = out_rx.recv() {
        let sent = match out {
            Outbound::Input(input) => conn.send_input(&to_input_event(input)),
            Outbound::Editor { msg, payload } => conn.send_editor(msg, &payload),
        };
        if sent.is_err() {
            break; // engine gone
        }
    }
}

fn to_input_event(input: Input) -> InputEvent {
    let (kind, x, y, code) = match input {
        Input::PointerMove { x, y } => (InputKind::PointerMove, x, y, 0),
        Input::PointerDown { x, y, button } => (InputKind::PointerDown, x, y, button),
        Input::PointerUp { x, y, button } => (InputKind::PointerUp, x, y, button),
    };
    InputEvent {
        kind,
        code,
        x,
        y,
        scroll_x: 0.0,
        scroll_y: 0.0,
        mods: 0,
        client_us: 0,
        seq: 0,
    }
}
