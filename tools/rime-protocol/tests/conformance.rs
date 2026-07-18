// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Cross-language protocol conformance — the Rust half (M9.3). Each fixture under `tests/fixtures/`
//! is a golden byte vector the **C++** engine emitted (tests/stream/protocol_fixtures_test.cpp).
//! This crate must (1) decode it into the right fields and (2) re-encode the identical bytes. Byte
//! equality on the round-trip is the real proof the two implementations of one wire agree; a drift
//! on either side turns this red. See tests/README (docs/design/graphics-streaming.md) for the wire.

use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::thread;

use rime_protocol::{
    decode_value, encode_value, Codec, ComponentRef, Connection, EditorMessage, FieldKind,
    FrameMessage, InputEvent, InputKind, MessageType, PixelFormat, Schema, SetComponent, Snapshot,
    Value, PROTOCOL_MAGIC, PROTOCOL_VERSION,
};

fn fixture(name: &str) -> Vec<u8> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()))
}

#[test]
fn handshake_bytes_match_the_golden() {
    let golden = fixture("handshake.bin");
    let mut expect = Vec::new();
    expect.extend_from_slice(&PROTOCOL_MAGIC.to_le_bytes());
    expect.extend_from_slice(&PROTOCOL_VERSION.to_le_bytes());
    assert_eq!(golden, expect, "handshake wire layout drifted from C++");
}

#[test]
fn input_event_decodes_and_re_encodes_byte_exact() {
    let golden = fixture("input_event.bin");
    let e = InputEvent::decode(&golden).expect("decode input");
    assert_eq!(e.kind, InputKind::PointerDown);
    assert_eq!(e.code, 1);
    assert_eq!(e.x, 100);
    assert_eq!(e.y, -50);
    assert_eq!(e.scroll_x.to_bits(), 0.5f32.to_bits());
    assert_eq!(e.scroll_y.to_bits(), (-0.25f32).to_bits());
    assert_eq!(e.mods, 3);
    assert_eq!(e.client_us, 123_456_789);
    assert_eq!(e.seq, 42);
    assert_eq!(e.encode(), golden);
}

#[test]
fn frame_message_decodes_and_re_encodes_byte_exact() {
    let golden = fixture("frame_message.bin");
    let f = FrameMessage::decode(&golden).expect("decode frame");
    assert_eq!(f.sequence, 7);
    assert_eq!(f.capture_us, 1000);
    assert_eq!(f.wire_us, 4000);
    assert_eq!(f.last_input_seq, 42);
    assert_eq!(f.last_input_client_us, 123_456_789);
    assert_eq!(f.codec, Codec::Lz4);
    assert_eq!(f.desc.width, 4);
    assert_eq!(f.desc.height, 2);
    assert_eq!(f.desc.format, PixelFormat::Rgba8Unorm);
    assert_eq!(f.data, vec![0xDE, 0xAD, 0xBE, 0xEF]);
    assert_eq!(f.encode(), golden);
}

#[test]
fn lz4_frame_decodes_to_the_pixels_cpp_compressed() {
    // frame_lz4.bin is a FrameMessage whose data the C++ engine LZ4-compressed from an 8x8 gradient;
    // frame_lz4_pixels.bin is that raw gradient. Rust must decompress the former back to the latter,
    // byte-for-byte — the cross-language proof for the editor viewport's lossless frame path.
    let frame = FrameMessage::decode(&fixture("frame_lz4.bin")).expect("decode frame");
    assert_eq!(frame.codec, Codec::Lz4);
    assert_eq!(frame.desc.width, 8);
    assert_eq!(frame.desc.height, 8);
    assert_eq!(frame.encode(), fixture("frame_lz4.bin")); // header + compressed data re-encode exact

    let pixels = frame.decode_pixels().expect("lz4 decode");
    assert_eq!(pixels.len(), frame.desc.byte_size());
    assert_eq!(
        pixels,
        fixture("frame_lz4_pixels.bin"),
        "cross-language LZ4 pixel mismatch"
    );
}

#[test]
fn schema_decodes_field_layout_and_re_encodes_byte_exact() {
    let golden = fixture("schema.bin");
    let schema = Schema::decode(&golden).expect("decode schema");

    // Camera is a component and carries its four reflected fields in order, kinds intact — this is
    // what the inspector is *generated* from (m9.4).
    let camera = schema
        .type_by_hash(
            schema
                .types
                .iter()
                .find(|t| t.name == "rime::render::Camera")
                .expect("camera type present")
                .type_hash,
        )
        .unwrap();
    assert!(camera.is_component);
    assert!(camera.type_hash != 0);
    assert_eq!(camera.fields.len(), 4);
    assert_eq!(camera.fields[0].name, "fov_y");
    assert_eq!(camera.fields[0].kind, FieldKind::F32);
    assert_eq!(camera.fields[3].name, "active");
    assert_eq!(camera.fields[3].kind, FieldKind::Bool); // the bool round-trips as a kind

    // MeshRef has a single u32 field.
    let mesh = schema
        .types
        .iter()
        .find(|t| t.name == "rime::render::MeshRef")
        .expect("meshref type present");
    assert_eq!(mesh.fields.len(), 1);
    assert_eq!(mesh.fields[0].kind, FieldKind::U32);

    assert_eq!(schema.encode(), golden);
}

#[test]
fn snapshot_components_decode_to_typed_values_and_re_encode_exact() {
    let schema = Schema::decode(&fixture("schema.bin")).expect("decode schema");
    let snap = Snapshot::decode(&fixture("snapshot.bin")).expect("decode snapshot");

    // Every snapshot component decodes to a typed tree against the schema, and re-encoding that tree
    // reproduces the exact wire bytes — the proof the Rust value codec matches the C++ packed layout
    // byte-for-byte (the whole basis for editing a component and sending it back).
    let mut decoded = 0;
    for e in &snap.entities {
        for c in &e.components {
            let value =
                decode_value(&schema, c.type_hash, &c.data).expect("decode component value");
            assert_eq!(
                encode_value(&value),
                c.data,
                "value round-trip drifted for type 0x{:016x}",
                c.type_hash
            );
            decoded += 1;
        }
    }
    assert!(decoded > 0, "snapshot had no components to decode");

    // Spot-check the Camera the C++ fixture spawned: Camera{0.9, 0.1, 500.0, true}.
    let camera_hash = schema
        .types
        .iter()
        .find(|t| t.name == "rime::render::Camera")
        .expect("camera type")
        .type_hash;
    let cam = snap
        .entities
        .iter()
        .flat_map(|e| &e.components)
        .find(|c| c.type_hash == camera_hash)
        .expect("camera component in snapshot");
    let Value::Struct(fields) = decode_value(&schema, camera_hash, &cam.data).unwrap() else {
        panic!("camera should decode to a struct");
    };
    assert_eq!(fields[0].0, "fov_y");
    assert_eq!(fields[0].1, Value::F32(0.9));
    assert_eq!(fields[3].0, "active");
    assert_eq!(fields[3].1, Value::Bool(true));
}

#[test]
fn component_ref_decodes_and_re_encodes_byte_exact() {
    let golden = fixture("component_ref.bin");
    let cr = ComponentRef::decode(&golden).expect("decode component ref");
    assert_eq!(cr.index, 3);
    assert_eq!(cr.generation, 1);
    assert!(cr.type_hash != 0); // the Camera hash
    assert_eq!(cr.encode(), golden);
}

#[test]
fn snapshot_decodes_structure_and_re_encodes_byte_exact() {
    let golden = fixture("snapshot.bin");
    let snap = Snapshot::decode(&golden).expect("decode snapshot");
    assert_eq!(snap.entities.len(), 2);
    // The mesh entity carries two components (MeshRef + Parent) — nested per-component blobs decode.
    assert!(snap.entities.iter().any(|e| e.components.len() == 2));
    assert!(snap
        .entities
        .iter()
        .all(|e| e.components.iter().all(|c| c.type_hash != 0)));
    assert_eq!(snap.encode(), golden);
}

#[test]
fn set_component_decodes_and_re_encodes_byte_exact() {
    let golden = fixture("set_component.bin");
    let sc = SetComponent::decode(&golden).expect("decode set-component");
    assert_eq!(sc.index, 3);
    assert_eq!(sc.generation, 1);
    assert!(sc.type_hash != 0);
    assert!(!sc.blob.is_empty()); // the reflected Camera bytes
    assert_eq!(sc.encode(), golden);
}

#[test]
fn message_type_and_editor_codes_are_stable() {
    assert_eq!(MessageType::Frame.to_code(), 0x0001);
    assert_eq!(MessageType::Input.to_code(), 0x0101);
    assert_eq!(MessageType::Bye.to_code(), 0xFFFF);
    // An editor-band code is preserved transparently as Other (the forward-compat rule).
    assert_eq!(MessageType::from_code(0x0210), MessageType::Other(0x0210));
    assert!(MessageType::is_editor(0x0210));
    assert!(!MessageType::is_editor(0x0001));
    assert_eq!(EditorMessage::SetComponent.to_code(), 0x0210);
    assert_eq!(
        EditorMessage::from_code(0x0200),
        Some(EditorMessage::Schema)
    );
    assert_eq!(EditorMessage::from_code(0x0001), None);
}

/// A `Connection` drives a real handshake + framed exchange end to end. Uses a loopback TCP socket
/// (portable across CI OSes) as the transport — the same `Connection<S: Read + Write>` the editor
/// runs over a `UnixStream`. Proves the framing, not just the payloads.
#[test]
fn connection_handshakes_and_exchanges_a_message() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");

    let server = thread::spawn(move || {
        let (sock, _) = listener.accept().expect("accept");
        let mut conn = Connection::new(sock);
        conn.handshake().expect("server handshake");
        // Send a Schema message, then expect a SetComponent edit back, then Bye.
        let schema = Schema { types: vec![] };
        conn.send_editor(EditorMessage::Schema, &schema.encode())
            .expect("send schema");
        let (ty, payload) = conn.recv().expect("recv edit");
        assert_eq!(
            ty,
            MessageType::Other(EditorMessage::SetComponent.to_code())
        );
        let sc = SetComponent::decode(&payload).expect("decode edit");
        assert_eq!(sc.index, 9);
        conn.send_bye().expect("send bye");
    });

    let sock = TcpStream::connect(addr).expect("connect");
    let mut conn = Connection::new(sock);
    conn.handshake().expect("client handshake");
    let (ty, payload) = conn.recv().expect("recv schema");
    assert_eq!(ty, MessageType::Other(EditorMessage::Schema.to_code()));
    assert!(Schema::decode(&payload).is_ok());
    let edit = SetComponent {
        index: 9,
        generation: 1,
        type_hash: 0xABCD,
        blob: vec![1, 2, 3],
    };
    conn.send_editor(EditorMessage::SetComponent, &edit.encode())
        .expect("send edit");
    let (ty, _) = conn.recv().expect("recv bye");
    assert_eq!(ty, MessageType::Bye);

    server.join().expect("server thread");
}
