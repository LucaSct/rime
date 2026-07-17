# rime-protocol

A hand-rolled Rust implementation of the Rime **streaming/editor wire protocol** — the exact bytes
`engine/stream` (+ `engine/editorhost`) speaks. The editor is a *client of a live engine process*
(ADR-0016): it launches `rime-engine --editor-host`, connects over the s1.4 local socket, and trades
reflection-described component data on the editor channel (and, later, a streamed viewport). This
crate is that client's tongue — the M9.3 keystone the rest of the editor is built on.

## Why a hand-rolled mirror

The two toolchains share no serialization runtime, so the wire is implemented twice — once in C++
(the reference), once here — and kept honest by a **cross-language conformance test**. The C++ side
emits a golden byte vector for each message (`tests/stream/protocol_fixtures_test.cpp`); this crate's
`tests/conformance.rs` decodes each golden and must **re-encode the identical bytes**. Byte equality
on the round-trip is the proof the two agree; drift on either side turns a test red instead of
becoming a silent field mismatch at runtime. Regenerate the goldens after a deliberate wire change
with `RIME_WRITE_PROTOCOL_FIXTURES=1 ctest -R protocol_fixtures` (then commit `tests/fixtures/`).

## Scope (v1 — the editor channel)

| Piece | What |
|---|---|
| Handshake + envelope | `[magic:u32][version:u16]`, then `[type:u16][len:u32][payload]` — all little-endian |
| [`MessageType`] | the wire type codes (`Frame`/`Input`/`Bye`/…); unknown codes pass through as `Other` |
| [`InputEvent`] | the 37-byte input payload (keys, pointer, scroll, + the s1.3 latency stamps) |
| [`FrameMessage`] | the frame header; the still-encoded pixel `data` is handed back opaque |
| [`editor`] | `Schema` / `Snapshot` decode, `SetComponent` / spawn / despawn — the `0x02xx` band |
| [`Connection`] | handshake + framed send/recv over any `Read + Write` (a `UnixStream`, a test pipe) |

Dependency-free on purpose: the editor channel is all fixed-layout integers. The **LZ4 pixel decode**
for the streamed viewport (an external lz4 crate) lands with the viewport panel, not here.

## Use

```rust
use rime_protocol::{Connection, EditorMessage, MessageType, Schema, SetComponent};

let mut conn = Connection::new(unix_stream);
conn.handshake()?;
let (ty, payload) = conn.recv()?;                 // e.g. the engine's Schema
if ty == MessageType::Other(EditorMessage::Schema.to_code()) {
    let schema = Schema::decode(&payload)?;       // label inspectors, gate by type_hash
}
conn.send_editor(EditorMessage::SetComponent, &edit.encode())?;  // push an edit back
```
