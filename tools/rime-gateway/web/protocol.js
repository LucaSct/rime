// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The wire protocol the browser speaks over the "input" DataChannel — the exact byte layouts
// `tools/rime-protocol` and `engine/stream` speak, re-derived here field-by-field rather than
// shared, because JS has no way to depend on the Rust crate (the same reasoning `rime-protocol`'s
// own module docs give for mirroring the C++ side instead of sharing a schema). What keeps the
// three honest is the hand-derived byte vectors in dev/protocol_test.mjs, checked against this
// file's output.
//
// The contract (contract.md) requires every DataChannel message to be exactly ONE envelope:
// `[type:u16 LE][length:u32 LE][payload]`, byte-identical to `rime_protocol::connection::Connection`
// (tools/rime-protocol/src/connection.rs, `send`/`recv`). A WebRTC DataChannel already delivers
// messages whole (no TCP-style stream to split), so there is no reassembly to do here — one
// `message` event is one envelope, decoded in one call.

/** Wire type codes — mirrors `rime_protocol::MessageType::to_code` (tools/rime-protocol/src/lib.rs). */
export const MessageType = Object.freeze({
  Frame: 0x0001,
  StreamConfig: 0x0002,
  Input: 0x0101,
  Capabilities: 0x0102,
  KeyframeRequest: 0x0103,
  Bye: 0xffff,
});

/** `InputEvent::Kind` wire codes — mirrors `rime_protocol::input::InputKind::to_code`. */
export const InputKind = Object.freeze({
  KeyDown: 0,
  KeyUp: 1,
  PointerMove: 2,
  PointerDown: 3,
  PointerUp: 4,
  PointerScroll: 5,
});

/** `stream::Codec` wire codes — mirrors `rime_protocol::frame::Codec::to_code`. Only `Av1` is sent
 * by this page today; the others exist so a `Capabilities` list can name them later (LZ4, the
 * LAN-only extra from ADR-0052, is not offered until that brick lands). */
export const Codec = Object.freeze({
  Raw: 0,
  Lz4: 1,
  Jpeg: 2,
  Av1: 3,
});

/**
 * Frame one message: `[type:u16 LE][length:u32 LE][payload]`. Returns an `ArrayBuffer` ready for
 * `RTCDataChannel.send`.
 */
export function encodeEnvelope(type, payload) {
  const body = payload instanceof Uint8Array ? payload : new Uint8Array(payload ?? 0);
  const out = new Uint8Array(6 + body.length);
  const view = new DataView(out.buffer);
  view.setUint16(0, type, true);
  view.setUint32(2, body.length, true);
  out.set(body, 6);
  return out.buffer;
}

/**
 * Parse one envelope. `data` is the whole DataChannel message (an `ArrayBuffer`), not a stream —
 * a length mismatch is a protocol error from a peer that cannot be trusted, so this throws rather
 * than silently truncating or padding.
 */
export function decodeEnvelope(data) {
  const bytes = new Uint8Array(data);
  if (bytes.length < 6) {
    throw new Error(`envelope shorter than the 6-byte header (${bytes.length} bytes)`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const type = view.getUint16(0, true);
  const length = view.getUint32(2, true);
  const payload = bytes.subarray(6);
  if (payload.length !== length) {
    throw new Error(`envelope declared ${length} payload bytes, message carried ${payload.length}`);
  }
  return { type, payload };
}

// ── InputEvent — 37 bytes, mirrors tools/rime-protocol/src/input.rs `InputEvent::encode` ────────
//
// [kind:u8][code:u32][x:i32][y:i32][scroll_x:f32][scroll_y:f32][mods:u32][client_us:u64][seq:u32]
//
// `client_us` is a 64-bit microsecond timestamp — outside JS's safe-integer range for a plain
// Number over long uptimes, so it is carried as a BigInt end to end (DataView's 64-bit accessors
// take/return BigInt natively; there is no float64 rounding anywhere on this path).

const INPUT_EVENT_BYTES = 37;

/** Encode one `InputEvent`. `event.clientUs` is a BigInt (microseconds); everything else is a
 * plain number. Field names match the Rust struct with camelCase substituted for snake_case. */
export function encodeInputEvent(event) {
  const buf = new ArrayBuffer(INPUT_EVENT_BYTES);
  const view = new DataView(buf);
  let offset = 0;
  view.setUint8(offset, event.kind);
  offset += 1;
  view.setUint32(offset, event.code >>> 0, true);
  offset += 4;
  view.setInt32(offset, event.x | 0, true);
  offset += 4;
  view.setInt32(offset, event.y | 0, true);
  offset += 4;
  view.setFloat32(offset, event.scrollX, true);
  offset += 4;
  view.setFloat32(offset, event.scrollY, true);
  offset += 4;
  view.setUint32(offset, event.mods >>> 0, true);
  offset += 4;
  view.setBigUint64(offset, BigInt(event.clientUs), true);
  offset += 8;
  view.setUint32(offset, event.seq >>> 0, true);
  offset += 4;
  return new Uint8Array(buf);
}

/** Decode an `InputEvent` payload. Not needed by the page's own send-only path, but kept
 * symmetric with `encodeInputEvent` — round-tripping through both is how dev/protocol_test.mjs
 * checks the encoder against hand-derived bytes without also hand-deriving a second vector. */
export function decodeInputEvent(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length !== INPUT_EVENT_BYTES) {
    throw new Error(`InputEvent payload must be ${INPUT_EVENT_BYTES} bytes, got ${bytes.length}`);
  }
  let offset = 0;
  const kind = view.getUint8(offset);
  offset += 1;
  const code = view.getUint32(offset, true);
  offset += 4;
  const x = view.getInt32(offset, true);
  offset += 4;
  const y = view.getInt32(offset, true);
  offset += 4;
  const scrollX = view.getFloat32(offset, true);
  offset += 4;
  const scrollY = view.getFloat32(offset, true);
  offset += 4;
  const mods = view.getUint32(offset, true);
  offset += 4;
  const clientUs = view.getBigUint64(offset, true);
  offset += 8;
  const seq = view.getUint32(offset, true);
  offset += 4;
  return { kind, code, x, y, scrollX, scrollY, mods, clientUs, seq };
}

// ── StreamConfig — mirrors engine/stream/src/protocol.cpp `StreamConfigMessage::encode` ────────
//
// [codec:u8][fmt:u8][w:u32 LE][h:u32 LE][codec_config...]
//
// The AV1 path does not need it to stand up a decoder (the sequence header rides the video track),
// but m18 Track H's input path needs its GEOMETRY: `InputEvent.x/y` are stream-frame pixels, and
// this message is the authoritative statement of what the frame's pixel space IS. The variable
// tail is skipped — this page has no use for a sequence header it is not decoding.

/** Decode a `StreamConfig` payload's fixed head. Throws on a truncated message, for the same
 * reason `decodeEnvelope` does: a peer that cannot frame 10 bytes is not one to guess for. */
export function decodeStreamConfig(bytes) {
  if (bytes.length < 10) {
    throw new Error(`StreamConfig payload must be at least 10 bytes, got ${bytes.length}`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return {
    codec: view.getUint8(0),
    format: view.getUint8(1),
    width: view.getUint32(2, true),
    height: view.getUint32(6, true),
  };
}

// ── Capabilities — mirrors engine/stream/src/protocol.cpp `CapabilitiesMessage::encode` ─────────
//
// [count:u8][decoder:u8 * count] — a one-byte count (capped at 255) followed by that many codec
// bytes, preference order first. The gateway skips codecs it does not recognise rather than
// rejecting the message (forward compatibility, protocol.hpp), so this page only ever needs to
// encode, never decode, its own capability list.

/** Encode a `Capabilities` payload: the client's decoders, in preference order. This page always
 * sends `[Codec.Av1]` — the only decode path implemented (ADR-0052); LZ4 is a later, LAN-only
 * addition the gateway will not offer until that brick lands regardless of what is advertised. */
export function encodeCapabilities(decoders) {
  if (decoders.length > 255) {
    throw new Error("a Capabilities list is capped at 255 decoders");
  }
  const out = new Uint8Array(1 + decoders.length);
  out[0] = decoders.length;
  out.set(decoders, 1);
  return out;
}
