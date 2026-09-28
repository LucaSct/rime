// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Bringing one hosted session up, and — the part that actually matters — taking it down again.
//!
//! The engine already knows how to be a session: `<host> --editor-host <socket>` binds a
//! `LocalListener` and serves the protocol (`engine/app/editor_host_app.cpp:9,491`). So the
//! gateway's job here is narrow and entirely outside `engine/`: spawn that process, wait for its
//! socket to exist, connect, handshake, and hand back a [`crate::Session`] with the surface policy
//! already applied.
//!
//! **Process per session, not in-process contexts** — `docs/design/hosted-rime.md` decided this, and
//! the reason is the teardown rather than the startup: one tenant's bad shader or malformed asset
//! must not take the others down, and there has to be a single unit to kill and (later) to bill.
//!
//! **Which is why kill-on-drop is the load-bearing property of this file, not spawning.** A gateway
//! that starts sessions and leaks the processes is a gateway that runs out of GPU and then out of
//! machine, and it fails slowly enough that the cause is long gone by the time anyone looks. So
//! [`SessionHandle`] kills its child in `Drop`, and there is a test that spawns a long sleep, drops
//! the handle, and asserts the pid is actually gone — because "we called kill()" and "the process
//! exited" are different claims.
//!
//! Not here yet, deliberately: HTTP, TLS, WebRTC, signalling, admission control, and the wall-clock
//! enforcement loop. [`SessionSpec::deadline`] carries the cap so the shape is right, but whoever
//! owns the event loop enforces it.

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

use crate::{Session, Surface};

/// What to launch, and what it is allowed to do once it is up.
#[derive(Debug, Clone)]
pub struct SessionSpec {
    /// The game or editor host binary.
    pub program: PathBuf,
    /// Arguments BEFORE `--editor-host <socket>`, which the supervisor appends itself so a caller
    /// cannot accidentally point a session at somebody else's socket.
    pub args: Vec<String>,
    /// Where the child will bind. One per session; the supervisor removes a stale file first.
    pub socket: PathBuf,
    /// Assigned by the gateway, never by the client. See [`Surface`].
    pub surface: Surface,
    /// How long to wait for the child to bind its socket.
    pub startup_timeout: Duration,
    /// Total session wall-clock cap. Carried here so the value has one home; enforced by the caller
    /// that owns the event loop.
    pub deadline: Duration,
}

/// Why a session failed to come up. Separate variants because they point at different culprits: a
/// missing binary is a deployment fault, a socket timeout is the game failing to start, and a
/// handshake failure is a protocol-version mismatch.
#[derive(Debug)]
pub enum SpawnError {
    /// The program could not be executed at all.
    Spawn(std::io::Error),
    /// The child never bound its socket within `startup_timeout`.
    SocketTimeout { waited: Duration },
    /// The child exited before binding its socket. Carries its status text, which is the only clue.
    ChildExited(String),
    /// The socket appeared but could not be connected to.
    Connect(std::io::Error),
    /// The protocol handshake failed — usually a version mismatch.
    Handshake(rime_protocol::Error),
}

impl std::fmt::Display for SpawnError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SpawnError::Spawn(e) => write!(f, "could not spawn the session process: {e}"),
            SpawnError::SocketTimeout { waited } => {
                write!(f, "session did not bind its socket within {waited:?}")
            }
            SpawnError::ChildExited(status) => {
                write!(
                    f,
                    "session process exited before binding its socket ({status})"
                )
            }
            SpawnError::Connect(e) => write!(f, "could not connect to the session socket: {e}"),
            SpawnError::Handshake(e) => write!(f, "session handshake failed: {e}"),
        }
    }
}

impl std::error::Error for SpawnError {}

/// A live session: the child process plus the framed, surface-filtered connection to it.
///
/// Dropping this **kills the child**. That is the point of the type, and it is why the connection
/// and the child live together rather than being handed back separately — a caller holding only the
/// connection could not reap the process, and a caller holding both could drop them in the order
/// that leaks.
pub struct SessionHandle {
    child: Child,
    socket_path: PathBuf,
    session: Session<UnixStream>,
}

impl SessionHandle {
    #[must_use]
    pub fn session(&mut self) -> &mut Session<UnixStream> {
        &mut self.session
    }

    /// The child's pid, for logging and for tests that need to assert it is gone.
    #[must_use]
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// Has the child already exited on its own? Non-blocking.
    pub fn exited(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(Some(_)))
    }

    /// Kill the child and wait for it, so the pid is genuinely reclaimed rather than left a zombie.
    /// Idempotent: a child that has already exited is simply reaped.
    pub fn terminate(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.socket_path);
    }
}

// Manual, because neither `Child` nor the connection is usefully printable — and a supervisor's log
// line wants the two things that identify a session anyway: which process and which socket.
impl std::fmt::Debug for SessionHandle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SessionHandle")
            .field("pid", &self.child.id())
            .field("socket", &self.socket_path)
            .field("surface", &self.session.surface())
            .finish()
    }
}

impl Drop for SessionHandle {
    fn drop(&mut self) {
        // `kill()` without `wait()` leaves a zombie, which still holds a pid and still counts
        // against the process table — so a gateway that only killed would look like it was reaping
        // and slowly stop being able to start sessions.
        self.terminate();
    }
}

/// Bring one session up: spawn, wait for the socket, connect, handshake.
///
/// `--editor-host <socket>` is appended by this function rather than taken from `spec.args`, so a
/// session cannot be pointed at a path the supervisor did not choose.
pub fn spawn_session(spec: &SessionSpec) -> Result<SessionHandle, SpawnError> {
    // A stale socket from a crashed predecessor would let `connect` succeed against nothing, and the
    // handshake would then fail with a confusing I/O error instead of a clear startup failure.
    let _ = std::fs::remove_file(&spec.socket);

    let mut command = Command::new(&spec.program);
    command.args(&spec.args);
    command.arg("--editor-host").arg(&spec.socket);
    let mut child = command.spawn().map_err(SpawnError::Spawn)?;

    match wait_for_socket(&spec.socket, spec.startup_timeout, &mut child) {
        Ok(()) => {}
        Err(e) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err(e);
        }
    }

    let stream = match UnixStream::connect(&spec.socket) {
        Ok(s) => s,
        Err(e) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err(SpawnError::Connect(e));
        }
    };

    let mut session = Session::new(stream, spec.surface);
    if let Err(e) = session.handshake() {
        let _ = child.kill();
        let _ = child.wait();
        return Err(SpawnError::Handshake(e));
    }

    Ok(SessionHandle {
        child,
        socket_path: spec.socket.clone(),
        session,
    })
}

/// Poll for the child's socket, and give up early if the child dies first.
///
/// Watching the child matters as much as watching the clock: a game that fails to load its content
/// exits in milliseconds, and waiting the full startup timeout for a process that is already gone
/// turns an instant, explainable failure into a slow mysterious one.
fn wait_for_socket(path: &Path, timeout: Duration, child: &mut Child) -> Result<(), SpawnError> {
    let started = Instant::now();
    loop {
        if path.exists() {
            return Ok(());
        }
        if let Ok(Some(status)) = child.try_wait() {
            return Err(SpawnError::ChildExited(format!("{status}")));
        }
        if started.elapsed() >= timeout {
            return Err(SpawnError::SocketTimeout { waited: timeout });
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

/// Serve the protocol's 6-byte version header on an accepted stream — the server half of
/// [`rime_protocol::Connection::handshake`]. Exposed for tests and for a future stub session; the
/// engine does this itself.
pub fn serve_handshake<S: Read + Write>(stream: &mut S) -> std::io::Result<()> {
    let mut hello = [0u8; 6];
    hello[0..4].copy_from_slice(&rime_protocol::PROTOCOL_MAGIC.to_le_bytes());
    hello[4..6].copy_from_slice(&rime_protocol::PROTOCOL_VERSION.to_le_bytes());
    stream.write_all(&hello)?;
    stream.flush()?;
    let mut peer = [0u8; 6];
    stream.read_exact(&mut peer)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn scratch_socket() -> PathBuf {
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!("rime-gw-test-{}-{n}.sock", std::process::id()))
    }

    fn spec(program: &str, args: &[&str], socket: PathBuf, timeout_ms: u64) -> SessionSpec {
        SessionSpec {
            program: PathBuf::from(program),
            args: args.iter().map(|s| (*s).to_string()).collect(),
            socket,
            surface: Surface::Play,
            startup_timeout: Duration::from_millis(timeout_ms),
            deadline: Duration::from_secs(60),
        }
    }

    /// A stand-in for the engine, and deliberately one that binds its OWN socket the way the real
    /// host does (`editor_host_app.cpp:491`). An earlier version of this test bound the listener from
    /// the test thread before calling `spawn_session`, which the supervisor then correctly deleted as
    /// a stale socket — the test was wrong, not the code, and doing it this way exercises the unlink
    /// path for real instead of racing it.
    ///
    /// `python3 -c SCRIPT <args>` puts the script's arguments in `sys.argv[1:]`, and the supervisor
    /// appends `--editor-host <socket>` last, so `sys.argv[-1]` is the socket path.
    ///
    /// The version is spliced in from [`rime_protocol::PROTOCOL_VERSION`], not written as a literal:
    /// the literal `3` this stub used to carry broke the moment the wire moved to v4, which is the
    /// stale-peer refusal working exactly as designed, pointed at a test.
    fn stub_host() -> String {
        format!(
            "\
import socket, sys, time\n\
p = sys.argv[-1]\n\
s = socket.socket(socket.AF_UNIX)\n\
s.bind(p)\n\
s.listen(1)\n\
c, _ = s.accept()\n\
c.sendall(bytes.fromhex('31534d52') + ({}).to_bytes(2, 'little'))\n\
c.recv(6)\n\
time.sleep(30)\n",
            rime_protocol::PROTOCOL_VERSION
        )
    }

    /// `kill -0` rather than `/proc/<pid>`: procfs is Linux-only, and this test must mean the same
    /// thing on macOS, where the first version of it failed for that reason alone.
    fn pid_alive(pid: u32) -> bool {
        Command::new("kill")
            .args(["-0", &pid.to_string()])
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }

    #[test]
    fn a_dropped_handle_actually_reaps_the_child() {
        // THE test in this file. "We called kill()" and "the process exited" are different claims,
        // and a gateway that only makes the first one runs out of machine slowly enough that the
        // cause is long gone by the time anyone looks.
        let socket = scratch_socket();
        let mut handle = spawn_session(&spec(
            "python3",
            &["-c", &stub_host()],
            socket.clone(),
            5000,
        ))
        .expect("session should come up");
        let pid = handle.pid();
        assert!(pid_alive(pid), "child should be running before the drop");
        assert_eq!(handle.session().surface(), Surface::Play);

        drop(handle);

        // Reaped, not merely signalled: `terminate` waits, so the pid is gone rather than a zombie.
        let mut gone = false;
        for _ in 0..200 {
            if !pid_alive(pid) {
                gone = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(gone, "pid {pid} still present after the handle was dropped");
        // The supervisor also removes the socket, so a restart is not blocked by its predecessor.
        assert!(!socket.exists(), "the session socket outlived the session");
    }

    #[test]
    fn a_child_that_dies_before_binding_is_reported_as_such() {
        // A process that exits at once. `/bin/sh -c "exit 1"` is used rather than the obvious
        // `false`, which lives at /usr/bin/false on macOS and /bin/false on Linux — the first version
        // hardcoded the Linux path and failed on macOS with Spawn(NotFound), asserting the location of
        // a binary instead of the behaviour it was written for. This must be ChildExited and not
        // SocketTimeout: waiting the full
        // timeout for a process that is already gone turns an instant, explainable failure into a
        // slow mysterious one, and the two errors point at different culprits.
        let socket = scratch_socket();
        let started = Instant::now();
        let err = spawn_session(&spec("/bin/sh", &["-c", "exit 1"], socket, 5_000))
            .expect_err("a process that exits cannot serve a session");
        assert!(
            matches!(err, SpawnError::ChildExited(_)),
            "expected ChildExited, got {err:?}"
        );
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "should fail as soon as the child dies, not after the 5s timeout"
        );
    }

    #[test]
    fn a_child_that_never_binds_times_out() {
        let socket = scratch_socket();
        let err = spawn_session(&spec("/bin/sh", &["-c", "sleep 30"], socket, 150))
            .expect_err("no socket was ever bound");
        assert!(
            matches!(err, SpawnError::SocketTimeout { .. }),
            "expected SocketTimeout, got {err:?}"
        );
    }

    #[test]
    fn a_missing_program_is_a_spawn_error() {
        let socket = scratch_socket();
        let err = spawn_session(&spec("/nonexistent/rime-host", &[], socket, 100))
            .expect_err("there is no such binary");
        assert!(matches!(err, SpawnError::Spawn(_)), "got {err:?}");
    }

    #[test]
    fn a_stale_socket_file_does_not_block_a_new_session() {
        // A crashed predecessor leaves its socket behind. Left in place, `connect` would succeed
        // against nothing and the failure would surface as a confusing handshake I/O error instead
        // of a startup failure — so the supervisor removes it first.
        let socket = scratch_socket();
        std::fs::write(&socket, b"stale").expect("plant a stale file");
        assert!(socket.exists());
        let err = spawn_session(&spec("/bin/sh", &["-c", "exit 1"], socket.clone(), 300))
            .expect_err("the child still cannot serve");
        // Not SocketTimeout: the stale file must not have been mistaken for a bound socket.
        assert!(
            matches!(err, SpawnError::ChildExited(_)),
            "stale file was treated as a live socket: {err:?}"
        );
        let _ = std::fs::remove_file(&socket);
    }
}
