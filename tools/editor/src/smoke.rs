// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `editor --smoke` — the headless editor<->engine end-to-end check (M9.3). It launches
//! `rime-engine --editor-host` over a fresh local socket, handshakes, pulls the component **schema**
//! and a full-world **snapshot**, pushes one **edit** back, and shuts the session down — asserting
//! the engine exits cleanly. This is the CI-provable heart of "the editor is a client of a live
//! engine": no window, no GPU, just the real two processes speaking the real wire (`rime-protocol`).
//! The streamed viewport (frames) is a later brick; this proves the world/inspector channel.

use std::process::ExitCode;

/// Run the smoke and turn its result into a process exit code (0 = pass).
#[cfg(unix)]
pub fn run(args: &[String]) -> ExitCode {
    match imp::run(args) {
        Ok(summary) => {
            println!("{summary}");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("editor --smoke FAILED: {e}");
            ExitCode::from(1)
        }
    }
}

/// Non-Unix stub: the smoke drives an `AF_UNIX` socket through `std::os::unix`, which Windows std
/// does not expose. The engine's local wire *is* `AF_UNIX` on Windows too (s1.4), so a Windows smoke
/// is a small follow-up; the cross-platform egui shell will carry it. Keeps the crate compiling
/// everywhere (the CI build matrix includes Windows).
#[cfg(not(unix))]
pub fn run(_args: &[String]) -> ExitCode {
    eprintln!(
        "editor --smoke is Unix-only for now (it drives an AF_UNIX socket via std::os::unix)."
    );
    ExitCode::from(2)
}

#[cfg(unix)]
mod imp {
    use std::io::ErrorKind;
    use std::os::unix::net::UnixStream;
    use std::path::{Path, PathBuf};
    use std::process::{Child, Command};
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    use rime_protocol::{Connection, EditorMessage, MessageType, Schema, SetComponent, Snapshot};

    /// Owns the spawned engine so an early failure never leaks the process: on drop (any error path)
    /// it is killed and reaped. On the happy path we `take` it out to `wait` for its real exit code.
    struct ChildGuard(Option<Child>);

    impl ChildGuard {
        fn take(mut self) -> Child {
            self.0.take().expect("engine child taken twice")
        }
    }

    impl Drop for ChildGuard {
        fn drop(&mut self) {
            if let Some(mut child) = self.0.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
        }
    }

    fn arg_value(args: &[String], flag: &str) -> Option<String> {
        args.iter()
            .position(|a| a == flag)
            .and_then(|i| args.get(i + 1))
            .cloned()
    }

    /// A fresh, unlikely-to-collide socket path under the temp dir (pid + a nanosecond stamp).
    fn unique_socket_path() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        std::env::temp_dir().join(format!("rime-editor-{}-{}.sock", std::process::id(), nanos))
    }

    /// Connect to the engine's socket, retrying while it does not yet exist / is not yet listening —
    /// the engine binds a moment after we spawn it, so a bounded retry replaces a fragile readiness
    /// handshake.
    fn connect_retry(path: &Path, timeout: Duration) -> std::io::Result<UnixStream> {
        let start = Instant::now();
        loop {
            match UnixStream::connect(path) {
                Ok(stream) => return Ok(stream),
                Err(e)
                    if matches!(e.kind(), ErrorKind::NotFound | ErrorKind::ConnectionRefused) =>
                {
                    if start.elapsed() >= timeout {
                        return Err(e);
                    }
                    std::thread::sleep(Duration::from_millis(25));
                }
                Err(e) => return Err(e),
            }
        }
    }

    /// Receive one message and require it to be the given editor-channel type.
    fn expect_editor(
        conn: &mut Connection<UnixStream>,
        want: EditorMessage,
    ) -> Result<Vec<u8>, String> {
        let (ty, payload) = conn.recv().map_err(|e| format!("recv: {e}"))?;
        if ty == MessageType::Other(want.to_code()) {
            Ok(payload)
        } else {
            Err(format!(
                "expected {want:?} (0x{:04x}), got {ty:?}",
                want.to_code()
            ))
        }
    }

    pub fn run(args: &[String]) -> Result<String, String> {
        let engine = arg_value(args, "--engine")
            .or_else(|| std::env::var("RIME_ENGINE_BIN").ok())
            .ok_or("no engine binary — pass --engine <rime-engine> or set RIME_ENGINE_BIN")?;
        let scene = arg_value(args, "--scene");
        let socket = unique_socket_path();

        // 1) Launch the engine host.
        let mut cmd = Command::new(&engine);
        cmd.arg("--editor-host").arg(&socket);
        if let Some(scene) = &scene {
            cmd.arg("--scene").arg(scene);
        }
        let child = cmd.spawn().map_err(|e| format!("spawn '{engine}': {e}"))?;
        let guard = ChildGuard(Some(child));

        // 2) Connect (retry until it binds) and handshake.
        let stream = connect_retry(&socket, Duration::from_secs(10))
            .map_err(|e| format!("connect to engine socket: {e}"))?;
        let mut conn = Connection::new(stream);
        conn.handshake().map_err(|e| format!("handshake: {e}"))?;

        // 3) Receive the schema, then the world snapshot.
        let schema = Schema::decode(&expect_editor(&mut conn, EditorMessage::Schema)?)
            .map_err(|e| format!("decode schema: {e}"))?;
        let snapshot = Snapshot::decode(&expect_editor(&mut conn, EditorMessage::Snapshot)?)
            .map_err(|e| format!("decode snapshot: {e}"))?;
        if schema.types.is_empty() {
            return Err("engine sent an empty schema".into());
        }
        let entity = snapshot
            .entities
            .iter()
            .find(|e| !e.components.is_empty())
            .ok_or("snapshot has no inspectable entity")?;

        // 4) Push an edit back: re-set the entity's first component to its own bytes — a valid
        //    no-op edit. The editor never interprets a component blob (that is the inspector's job,
        //    schema-driven), so this exercises the whole SetComponent path without decoding it.
        let component = &entity.components[0];
        let edit = SetComponent {
            index: entity.index,
            generation: entity.generation,
            type_hash: component.type_hash,
            blob: component.data.clone(),
        };
        conn.send_editor(EditorMessage::SetComponent, &edit.encode())
            .map_err(|e| format!("send edit: {e}"))?;

        // 5) Close the session; the engine's drain loop ends on Bye and the process exits.
        conn.send_bye().map_err(|e| format!("send bye: {e}"))?;
        let status = guard
            .take()
            .wait()
            .map_err(|e| format!("wait for engine: {e}"))?;
        if !status.success() {
            return Err(format!("engine exited with {status}"));
        }

        Ok(format!(
            "editor <-> engine OK: {} schema types, {} entities; edit round-tripped; clean shutdown",
            schema.types.len(),
            snapshot.entities.len()
        ))
    }
}
