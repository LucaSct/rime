// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The engine session behind the docking shell: spawn `rime-engine --editor-host --viewport`, connect
//! over the local socket, and run the wire off the UI thread. A **receiver** thread decodes the
//! schema, snapshot, and streamed frames into shared state the UI reads each repaint; a **sender**
//! thread forwards viewport input. Full-duplex over one socket (the `rime-protocol` contract), the
//! same wire the headless smoke proves — so only what appears on screen is Mac-eyeballed.

use std::collections::HashMap;
use std::io::ErrorKind;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use rime_protocol::{
    Connection, EditorMessage, FrameMessage, InputEvent, InputKind, MessageType, Schema, Snapshot,
};

use super::protocol_input::Input;

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
    pub frame: Option<ViewportFrame>,
    pub frames_received: u64,
    pub fps: f32,
    last_frame_at: Option<Instant>,
}

impl SharedState {
    /// `type_hash → source name` from the schema, so the inspector can label a snapshot's components.
    pub fn schema_names(&self) -> HashMap<u64, String> {
        self.schema
            .types
            .iter()
            .map(|t| (t.type_hash, t.name.clone()))
            .collect()
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
    /// Spawn `rime-engine --editor-host --viewport` and start driving the wire. Any failure lands in
    /// `shared.error` (the status bar shows it) rather than panicking the UI.
    pub fn spawn(engine: String, shared: Shared, input_rx: Receiver<Input>) -> Self {
        let socket = unique_socket_path();
        let child = match Command::new(&engine)
            .arg("--editor-host")
            .arg(&socket)
            .arg("--viewport")
            .spawn()
        {
            Ok(child) => child,
            Err(e) => {
                shared.lock().unwrap().error = Some(format!("spawn '{engine}': {e}"));
                return Self {
                    child: None,
                    recv_handle: None,
                };
            }
        };
        let recv_handle = thread::spawn(move || run_session(&socket, &shared, input_rx));
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
fn run_session(socket: &Path, shared: &Shared, input_rx: Receiver<Input>) {
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

    // Sender thread: viewport input → InputEvents. Detached; it exits when the UI drops input_rx.
    let _sender = thread::spawn(move || run_sender(Connection::new(write_stream), input_rx));

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
        _ => {} // an editor->engine type echoed, or something this build predates
    }
}

fn run_sender(mut conn: Connection<UnixStream>, input_rx: Receiver<Input>) {
    while let Ok(input) = input_rx.recv() {
        if conn.send_input(&to_input_event(input)).is_err() {
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
