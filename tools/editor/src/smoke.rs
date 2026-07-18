// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `editor --smoke` — the headless editor<->engine end-to-end check (M9.3). It launches
//! `rime-engine --editor-host` over a fresh local socket, handshakes, pulls the component **schema**
//! and a full-world **snapshot**, and shuts the session down — asserting the engine exits cleanly.
//! Two modes prove the two halves of "the editor is a client of a live engine", no window needed:
//!
//!   * **channel** (default): push one **edit** back — the world/inspector path.
//!   * **`--frames N`**: the engine renders a scene and streams it; the smoke receives + LZ4-decodes
//!     N **viewport frames** to RGBA (the render → capture → encode → wire → decode path). Needs a
//!     GPU/lavapipe on the engine side.
//!
//! Just the real two processes speaking the real wire (`rime-protocol`).

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

    use rime_protocol::{
        decode_value, encode_value, AssetKind, AssetList, Connection, EditorMessage, FrameMessage,
        MessageType, PickRequest, PickResult, Schema, SetComponent, Snapshot, SnapshotComponent,
        SnapshotEntity, Value,
    };

    // A tiny cook manifest the smoke hands the engine (--assets) to prove the browse path end to end:
    // the engine parses this file and sends it back as an AssetList. Tab-separated
    // `source \t kind \t id-hex \t cooked` (assets::Manifest grammar), with the cooker's banner.
    const SMOKE_MANIFEST: &str = "# rime-manifest v1 (editor smoke)\n\
        meshes/barrel.gltf\tmesh\t00000000abcdef01\tbarrel.rmesh\n\
        materials/rust.mat\tmaterial\t0000000000000099\trust.rmat\n";

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

    /// The first (entity, component) whose type the schema describes and whose value has an editable
    /// scalar leaf — the field the smoke pokes to prove a reflection-typed edit round-trips.
    fn find_editable<'a>(
        schema: &Schema,
        snapshot: &'a Snapshot,
    ) -> Option<(&'a SnapshotEntity, &'a SnapshotComponent)> {
        for e in &snapshot.entities {
            for c in &e.components {
                if let Ok(v) = decode_value(schema, c.type_hash, &c.data) {
                    if read_first_scalar(&v).is_some() {
                        return Some((e, c));
                    }
                }
            }
        }
        None
    }

    /// The first scalar leaf's value as f64 (recursing into structs), for a before/after comparison.
    fn read_first_scalar(value: &Value) -> Option<f64> {
        match value {
            Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
            Value::F32(x) => Some(*x as f64),
            Value::F64(x) => Some(*x),
            Value::I32(x) => Some(*x as f64),
            Value::U32(x) => Some(*x as f64),
            Value::I64(x) => Some(*x as f64),
            Value::U64(x) => Some(*x as f64),
            Value::Struct(fields) => fields.iter().find_map(|(_, v)| read_first_scalar(v)),
        }
    }

    /// Change the first scalar leaf in place (toggle a bool, +1 a number), returning its new value as
    /// f64 — a deterministic, obviously-different edit the smoke can verify came back.
    fn bump_first_scalar(value: &mut Value) -> Option<f64> {
        match value {
            Value::Bool(b) => {
                *b = !*b;
                Some(if *b { 1.0 } else { 0.0 })
            }
            Value::F32(x) => {
                *x += 1.0;
                Some(*x as f64)
            }
            Value::F64(x) => {
                *x += 1.0;
                Some(*x)
            }
            Value::I32(x) => {
                *x += 1;
                Some(*x as f64)
            }
            Value::U32(x) => {
                *x += 1;
                Some(*x as f64)
            }
            Value::I64(x) => {
                *x += 1;
                Some(*x as f64)
            }
            Value::U64(x) => {
                *x += 1;
                Some(*x as f64)
            }
            Value::Struct(fields) => fields.iter_mut().find_map(|(_, v)| bump_first_scalar(v)),
        }
    }

    /// Read messages until the engine's snapshot reply arrives (channel mode carries nothing else, but
    /// skip anything unexpected rather than mis-handle it).
    fn recv_snapshot(conn: &mut Connection<UnixStream>) -> Result<Snapshot, String> {
        loop {
            let (ty, payload) = conn.recv().map_err(|e| format!("recv snapshot: {e}"))?;
            if ty == MessageType::Other(EditorMessage::Snapshot.to_code()) {
                return Snapshot::decode(&payload).map_err(|e| format!("decode snapshot: {e}"));
            }
        }
    }

    /// Read messages until a PickResult arrives (m9.6), skipping streamed frames — in viewport mode
    /// the engine keeps rendering while the pick pass runs, and the answer is a frame late by design.
    fn recv_pick(conn: &mut Connection<UnixStream>) -> Result<PickResult, String> {
        loop {
            let (ty, payload) = conn.recv().map_err(|e| format!("recv pick result: {e}"))?;
            if ty == MessageType::Other(EditorMessage::PickResult.to_code()) {
                return PickResult::decode(&payload).map_err(|e| format!("decode pick: {e}"));
            }
        }
    }

    pub fn run(args: &[String]) -> Result<String, String> {
        let engine = arg_value(args, "--engine")
            .or_else(|| std::env::var("RIME_ENGINE_BIN").ok())
            .ok_or("no engine binary — pass --engine <rime-engine> or set RIME_ENGINE_BIN")?;
        let scene = arg_value(args, "--scene");
        let frames: u32 = arg_value(args, "--frames")
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        let socket = unique_socket_path();

        // 1) Launch the engine host. --frames drives the streamed viewport (needs a GPU/lavapipe);
        //    otherwise it is the GPU-free editor channel over --scene (or a default world).
        let mut cmd = Command::new(&engine);
        cmd.arg("--editor-host").arg(&socket);
        if frames > 0 {
            cmd.arg("--viewport");
        } else if let Some(scene) = &scene {
            cmd.arg("--scene").arg(scene);
        }
        // In channel mode, also hand the engine a small manifest so the smoke can prove the browse
        // path (engine reads the file → AssetList over the live socket). The file must live until the
        // engine reads it at connect time; the smoke deletes it once the AssetList arrives.
        let manifest_path = if frames == 0 {
            let p = std::env::temp_dir().join(format!(
                "rime-editor-smoke-{}-{}.manifest",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map(|d| d.as_nanos())
                    .unwrap_or(0)
            ));
            std::fs::write(&p, SMOKE_MANIFEST).map_err(|e| format!("write manifest: {e}"))?;
            cmd.arg("--assets").arg(&p);
            Some(p)
        } else {
            None
        };
        let child = cmd.spawn().map_err(|e| format!("spawn '{engine}': {e}"))?;
        let guard = ChildGuard(Some(child));

        // 2) Connect (retry until it binds), then handshake. A read timeout keeps a wedged engine
        //    from hanging the smoke forever.
        let stream = connect_retry(&socket, Duration::from_secs(10))
            .map_err(|e| format!("connect to engine socket: {e}"))?;
        stream
            .set_read_timeout(Some(Duration::from_secs(15)))
            .map_err(|e| format!("set read timeout: {e}"))?;
        let mut conn = Connection::new(stream);
        conn.handshake().map_err(|e| format!("handshake: {e}"))?;

        // 3) Receive the schema, then the world snapshot (both modes).
        let schema = Schema::decode(&expect_editor(&mut conn, EditorMessage::Schema)?)
            .map_err(|e| format!("decode schema: {e}"))?;
        let snapshot = Snapshot::decode(&expect_editor(&mut conn, EditorMessage::Snapshot)?)
            .map_err(|e| format!("decode snapshot: {e}"))?;
        if schema.types.is_empty() {
            return Err("engine sent an empty schema".into());
        }

        let summary = if frames > 0 {
            // Streamed-viewport mode: receive + decode N frames — the render → capture → LZ4 → wire →
            // decode path end to end — then close.
            let (got, w, h, pixels) = receive_frames(&mut conn, frames)?;

            // Live pick (m9.6), self-locating so no scene coordinates are hardcoded: the brightest
            // pixel of a decoded frame is by construction on a lit, rendered surface (the cleared
            // background is black), so picking it MUST name a real entity — the click→PickRequest→
            // ID-pass→PickResult loop proven against the live renderer. A corner pixel of the same
            // frame is empty sky and must miss.
            let (bx, by) = brightest_pixel(&pixels, w);
            conn.send_editor(
                EditorMessage::PickRequest,
                &PickRequest { x: bx, y: by }.encode(),
            )
            .map_err(|e| format!("send pick: {e}"))?;
            let hit = recv_pick(&mut conn)?;
            if !hit.is_hit() {
                return Err(format!(
                    "picking the brightest rendered pixel ({bx},{by}) claimed empty space"
                ));
            }
            if !snapshot
                .entities
                .iter()
                .any(|e| (e.index, e.generation) == (hit.index, hit.generation))
            {
                return Err(format!(
                    "pick answered entity {}:{} which the snapshot does not contain",
                    hit.index, hit.generation
                ));
            }
            conn.send_editor(
                EditorMessage::PickRequest,
                &PickRequest { x: 0, y: 0 }.encode(),
            )
            .map_err(|e| format!("send pick: {e}"))?;
            let miss = recv_pick(&mut conn)?;
            if miss.is_hit() {
                return Err("picking the empty corner pixel claimed a hit".into());
            }

            conn.send_bye().map_err(|e| format!("send bye: {e}"))?;
            drain_until_closed(&mut conn);
            format!(
                "editor <-> engine viewport OK: {} entities; {got} frames decoded to {w}x{h} RGBA; \
                 pick at ({bx},{by}) hit entity {}:{}, corner missed; clean shutdown",
                snapshot.entities.len(),
                hit.index,
                hit.generation
            )
        } else {
            // Browse (m9.5): the engine parsed the --assets manifest and sends it after the snapshot.
            let assets = AssetList::decode(&expect_editor(&mut conn, EditorMessage::AssetList)?)
                .map_err(|e| format!("decode asset list: {e}"))?;
            if let Some(p) = &manifest_path {
                let _ = std::fs::remove_file(p);
            }
            if assets.assets.len() != 2 || !assets.assets.iter().any(|a| a.kind == AssetKind::Mesh)
            {
                return Err(format!(
                    "asset list did not round-trip the manifest: {:?}",
                    assets.assets
                ));
            }

            // Editor-channel mode: the m9.4 inspector path, headless. Decode a component to typed
            // fields *through the schema* (exactly what the inspector does), edit one scalar, send the
            // re-encoded bytes as a SetComponent, then request a fresh snapshot and confirm the field
            // actually changed on the live engine — the whole reflection-driven edit loop, proven.
            let (entity, component) = find_editable(&schema, &snapshot)
                .ok_or("snapshot has no schema-describable component with an editable field")?;
            let key = (entity.index, entity.generation);
            let hash = component.type_hash;

            let mut value =
                decode_value(&schema, hash, &component.data).map_err(|e| format!("decode: {e}"))?;
            let before = read_first_scalar(&value).ok_or("component has no scalar field")?;
            let after = bump_first_scalar(&mut value).ok_or("component has no scalar to edit")?;
            let edit = SetComponent {
                index: key.0,
                generation: key.1,
                type_hash: hash,
                blob: encode_value(&value),
            };
            conn.send_editor(EditorMessage::SetComponent, &edit.encode())
                .map_err(|e| format!("send edit: {e}"))?;

            // Ask for the world back and check the edited field holds the new value.
            conn.send_editor(EditorMessage::RequestSnapshot, &[])
                .map_err(|e| format!("send request-snapshot: {e}"))?;
            let snap2 = recv_snapshot(&mut conn)?;
            let comp2 = snap2
                .entities
                .iter()
                .find(|e| (e.index, e.generation) == key)
                .and_then(|e| e.components.iter().find(|c| c.type_hash == hash))
                .ok_or("edited component vanished from the refreshed snapshot")?;
            let got = read_first_scalar(
                &decode_value(&schema, hash, &comp2.data).map_err(|e| format!("re-decode: {e}"))?,
            )
            .ok_or("no scalar after edit")?;
            if (got - after).abs() > 1e-3 {
                return Err(format!(
                    "typed edit did not apply: field was {before}, set to {after}, engine reports {got}"
                ));
            }

            // The pick wire on the GPU-free host (m9.6): with no renderer there is no ID buffer,
            // so the honest answer to any pick is the "nothing" sentinel — but it must ANSWER
            // (a request without a reply would strand a client's click forever).
            conn.send_editor(
                EditorMessage::PickRequest,
                &PickRequest { x: 10, y: 10 }.encode(),
            )
            .map_err(|e| format!("send pick: {e}"))?;
            let pick = recv_pick(&mut conn)?;
            if pick.is_hit() {
                return Err(format!(
                    "the GPU-free host claimed a pick hit ({}:{})",
                    pick.index, pick.generation
                ));
            }

            conn.send_bye().map_err(|e| format!("send bye: {e}"))?;
            format!(
                "editor <-> engine OK: {} schema types, {} entities, {} assets browsed; typed field edit {before} -> {after} applied and confirmed via snapshot; pick answered honestly (no viewport => miss); clean shutdown",
                schema.types.len(),
                snapshot.entities.len(),
                assets.assets.len()
            )
        };

        let status = guard
            .take()
            .wait()
            .map_err(|e| format!("wait for engine: {e}"))?;
        if !status.success() {
            return Err(format!("engine exited with {status}"));
        }
        Ok(summary)
    }

    /// Receive and decode `n` viewport frames. Each must LZ4-decode to `width*height*4` RGBA bytes
    /// and carry some variation (a uniform image would mean nothing was rendered). Editor-channel
    /// messages arriving interleaved are ignored. Returns (frames, width, height, last frame's
    /// RGBA) — the pixels feed the self-locating pick that follows.
    fn receive_frames(
        conn: &mut Connection<UnixStream>,
        n: u32,
    ) -> Result<(u32, u32, u32, Vec<u8>), String> {
        let mut got = 0u32;
        let mut dims = (0u32, 0u32);
        let mut last = Vec::new();
        while got < n {
            let (ty, payload) = conn.recv().map_err(|e| format!("recv frame: {e}"))?;
            if ty != MessageType::Frame {
                continue; // an editor-channel message; not what this mode counts
            }
            let frame = FrameMessage::decode(&payload).map_err(|e| format!("decode frame: {e}"))?;
            let pixels = frame
                .decode_pixels()
                .map_err(|e| format!("decode pixels: {e}"))?;
            if pixels.len() != frame.desc.byte_size() {
                return Err("frame pixel size mismatch".into());
            }
            if !pixels.is_empty() && pixels.iter().all(|&b| b == pixels[0]) {
                return Err("frame decoded to a uniform image (nothing rendered?)".into());
            }
            dims = (frame.desc.width, frame.desc.height);
            last = pixels;
            got += 1;
        }
        Ok((got, dims.0, dims.1, last))
    }

    /// The (x, y) of the brightest pixel of an RGBA image — the most-lit spot of the rendered
    /// scene, guaranteed to sit ON some entity (the cleared background is black). Sum of R+G+B;
    /// ties keep the first, any of them serves.
    fn brightest_pixel(rgba: &[u8], width: u32) -> (i32, i32) {
        let mut best = (0usize, 0u32);
        for (i, px) in rgba.chunks_exact(4).enumerate() {
            let lum = px[0] as u32 + px[1] as u32 + px[2] as u32;
            if lum > best.1 {
                best = (i, lum);
            }
        }
        let w = width.max(1) as usize;
        ((best.0 % w) as i32, (best.0 / w) as i32)
    }

    /// Read and discard messages until the engine closes, so its frame-sender never blocks on a full
    /// socket buffer once we stop consuming (which would wedge the engine's clean shutdown).
    fn drain_until_closed(conn: &mut Connection<UnixStream>) {
        while conn.recv().is_ok() {}
    }
}
