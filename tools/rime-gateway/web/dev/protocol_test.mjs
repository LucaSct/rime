// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Byte-exact checks for protocol.js against vectors derived BY HAND from the Rust layouts —
// tools/rime-protocol/src/input.rs's `InputEvent::encode` doc comment and
// engine/stream/src/protocol.cpp's `CapabilitiesMessage::encode` — not against protocol.js's own
// logic, which would only prove the encoder agrees with itself. Run with `node
// dev/protocol_test.mjs`; dev/check.sh runs it as one of its gates.

import assert from "node:assert/strict";
import {
  MessageType,
  InputKind,
  Codec,
  encodeEnvelope,
  decodeEnvelope,
  encodeInputEvent,
  decodeInputEvent,
  encodeCapabilities,
} from "../protocol.js";

function toHex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

let failures = 0;

function check(name, actual, expectedBytes) {
  const expectedHex = toHex(expectedBytes);
  const actualHex = toHex(actual);
  if (actualHex === expectedHex) {
    console.log(`ok - ${name}`);
  } else {
    failures += 1;
    console.error(`FAIL - ${name}\n  expected ${expectedHex}\n  actual   ${actualHex}`);
  }
}

// ── InputEvent — [kind:u8][code:u32][x:i32][y:i32][scroll_x:f32][scroll_y:f32][mods:u32]
//                [client_us:u64][seq:u32], all little-endian (input.rs).
//
// Field-by-field derivation, each value chosen so the hex is checkable by hand rather than
// requiring a decimal<->hex conversion this comment would then have to redo the encoder's own
// work to verify:
//
//   kind       = InputKind.PointerDown = 3                       -> 03
//   code       = 0x000000AB (an arbitrary button/key code)       -> AB 00 00 00 (LE)
//   x          = -1 (i32)  -> two's complement 0xFFFFFFFF         -> FF FF FF FF (LE)
//   y          = 0x00000140 = 320                                 -> 40 01 00 00 (LE)
//   scroll_x   = 1.0f32    -> IEEE-754 0x3F800000 (textbook value) -> 00 00 80 3F (LE)
//   scroll_y   = -1.5f32   -> sign=1 exp=01111111(127) mantissa=1000...0(23 bits)
//                             = 1_01111111_10000000000000000000000 = 0xBFC00000
//                             (equivalently: +1.5f32 is the well-known 0x3FC00000; negating a
//                             float only flips the sign bit, giving 0xBFC00000)              -> 00 00 C0 BF (LE)
//   mods       = KeyMods::Shift(1) | KeyMods::Alt(4) = 5           -> 05 00 00 00 (LE)
//   client_us  = 0x0102030405060708 (u64, chosen in hex directly so the LE bytes are its own
//                digits reversed, rather than round-tripping through decimal)                -> 08 07 06 05 04 03 02 01 (LE)
//   seq        = 42 = 0x0000002A                                                              -> 2A 00 00 00 (LE)
const inputEvent = {
  kind: InputKind.PointerDown,
  code: 0xab,
  x: -1,
  y: 0x140,
  scrollX: 1.0,
  scrollY: -1.5,
  mods: 0b0101, // Shift | Alt
  clientUs: 0x0102030405060708n,
  seq: 42,
};

const expectedInputEventBytes = [
  0x03,
  0xab, 0x00, 0x00, 0x00,
  0xff, 0xff, 0xff, 0xff,
  0x40, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x80, 0x3f,
  0x00, 0x00, 0xc0, 0xbf,
  0x05, 0x00, 0x00, 0x00,
  0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
  0x2a, 0x00, 0x00, 0x00,
];
assert.equal(expectedInputEventBytes.length, 37, "the hand-derived vector itself must be 37 bytes");

const encodedInput = encodeInputEvent(inputEvent);
check("InputEvent encodes to the hand-derived 37 bytes", encodedInput, expectedInputEventBytes);

const decodedInput = decodeInputEvent(encodedInput);
assert.equal(decodedInput.kind, inputEvent.kind);
assert.equal(decodedInput.code, inputEvent.code);
assert.equal(decodedInput.x, inputEvent.x);
assert.equal(decodedInput.y, inputEvent.y);
assert.equal(decodedInput.scrollX, inputEvent.scrollX);
assert.equal(decodedInput.scrollY, inputEvent.scrollY);
assert.equal(decodedInput.mods, inputEvent.mods);
assert.equal(decodedInput.clientUs, inputEvent.clientUs);
assert.equal(decodedInput.seq, inputEvent.seq);
console.log("ok - InputEvent round-trips through decodeInputEvent");

// ── Capabilities — [count:u8][decoder:u8 * count] (engine/stream/src/protocol.cpp,
// `CapabilitiesMessage::encode`): one decoder, Av1 (wire code 3, frame.rs `Codec::to_code`).
const expectedCapabilitiesBytes = [0x01, 0x03];
check("Capabilities([Av1]) encodes to [count=1, Av1=3]", encodeCapabilities([Codec.Av1]), expectedCapabilitiesBytes);

// ── Envelope — [type:u16 LE][length:u32 LE][payload] (rime-protocol connection.rs `send`/`recv`).
// Wrapping the Capabilities payload above: type = MessageType.Capabilities = 0x0102 (LE: 02 01 —
// chosen over Input's 0x0101 because its two bytes differ, which an envelope test built on a
// symmetric value like 0x0101 could not catch a byte-swap bug with), length = 2 (LE: 02 00 00 00).
const expectedEnvelopeBytes = [0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x01, 0x03];
const encodedEnvelope = new Uint8Array(
  encodeEnvelope(MessageType.Capabilities, encodeCapabilities([Codec.Av1])),
);
check("encodeEnvelope frames [type][length][payload]", encodedEnvelope, expectedEnvelopeBytes);

const decodedEnvelope = decodeEnvelope(encodedEnvelope.buffer);
assert.equal(decodedEnvelope.type, MessageType.Capabilities);
assert.equal(toHex(decodedEnvelope.payload), toHex(expectedCapabilitiesBytes));
console.log("ok - decodeEnvelope recovers type and payload");

// A truncated envelope (declared length longer than what arrived) must be refused, not silently
// truncated or padded — a peer that lies about a length is not a peer to trust the rest of.
assert.throws(() => decodeEnvelope(new Uint8Array([0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x01]).buffer));
console.log("ok - decodeEnvelope rejects a length that does not match the message");

if (failures > 0) {
  console.error(`${failures} vector(s) did not match`);
  process.exit(1);
}
