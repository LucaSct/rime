// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The WebRTC session flow: create a render session, stand up a peer connection to it, and forward
// input while AV1 video arrives on a track. Unverifiable in this brick's own test harness — there
// is no real gateway, TURN server, or engine process in `dev/check.sh`'s headless run, only static
// DOM assertions (see page-report.md) — so this module is written straight from contract.md and
// checked by reading, not by a green test. The first real Chrome run against a real gateway is the
// interop proof ADR-0052 calls for, and it has not happened yet.

import * as api from "./api.js";
import {
  MessageType,
  InputKind,
  Codec,
  encodeEnvelope,
  decodeEnvelope,
  encodeInputEvent,
  encodeCapabilities,
} from "./protocol.js";
import { keyOrdinalOf } from "./keymap.js";

/** `KeyMods` bitmask — mirrors `engine/platform/include/rime/platform/keyboard.hpp`'s explicit
 * (and therefore, unlike `Key`, stable-by-declaration) values. */
const KeyMods = Object.freeze({ Shift: 1, Ctrl: 2, Alt: 4, Super: 8 });

function modsOf(event) {
  let mods = 0;
  if (event.shiftKey) mods |= KeyMods.Shift;
  if (event.ctrlKey) mods |= KeyMods.Ctrl;
  if (event.altKey) mods |= KeyMods.Alt;
  if (event.metaKey) mods |= KeyMods.Super;
  return mods;
}

/** How long without a decoded video frame before this page asks for a fresh keyframe
 * (page-spec.md: "Send KeyframeRequest if the video stalls (no requestVideoFrameCallback for
 * 2 s)"). */
const STALL_TIMEOUT_MS = 2000;

/**
 * Whether this browser can decode AV1 at all — checked before a session is even created, so a
 * browser that cannot show anything never spawns an engine process for nothing (ADR-0052: "not
 * yet, deliberately" for Safari and older mobile). `RTCRtpReceiver.getCapabilities` is itself
 * optional (Safari lacks it), so its absence already answers the question.
 */
export function av1DecodeSupported() {
  if (typeof RTCRtpReceiver === "undefined" || typeof RTCRtpReceiver.getCapabilities !== "function") {
    return false;
  }
  const caps = RTCRtpReceiver.getCapabilities("video");
  return !!caps && caps.codecs.some((codec) => codec.mimeType.toLowerCase() === "video/av1");
}

function preferAv1(transceiver) {
  if (typeof RTCRtpReceiver === "undefined" || typeof RTCRtpReceiver.getCapabilities !== "function") {
    return;
  }
  if (typeof transceiver.setCodecPreferences !== "function") {
    return;
  }
  const caps = RTCRtpReceiver.getCapabilities("video");
  if (!caps) return;
  const av1 = caps.codecs.filter((c) => c.mimeType.toLowerCase() === "video/av1");
  if (av1.length === 0) return;
  const rest = caps.codecs.filter((c) => c.mimeType.toLowerCase() !== "video/av1");
  transceiver.setCodecPreferences([...av1, ...rest]);
}

function waitForIceGatheringComplete(pc) {
  if (pc.iceGatheringState === "complete") {
    return Promise.resolve();
  }
  return new Promise((resolve) => {
    function onChange() {
      if (pc.iceGatheringState === "complete") {
        pc.removeEventListener("icegatheringstatechange", onChange);
        resolve();
      }
    }
    pc.addEventListener("icegatheringstatechange", onChange);
  });
}

/** One live session: the render session id, the peer connection, and the input channel. Owns
 * every listener it attaches and undoes them all in `leave()` — a session view that gets torn
 * down and rebuilt must not accumulate a second set of window-level key listeners. */
export class SessionHandle {
  constructor(sessionId, pc, channel, videoEl, onStatus) {
    this.sessionId = sessionId;
    this.pc = pc;
    this.channel = channel;
    this.videoEl = videoEl;
    this.onStatus = onStatus;
    this.seq = 0;
    this.stallTimer = null;
    this.lastFrameAt = 0;
    this.left = false;
    this._boundKeyDown = (e) => this.forwardKey(e, InputKind.KeyDown);
    this._boundKeyUp = (e) => this.forwardKey(e, InputKind.KeyUp);
    this._boundMouseDown = (e) => this.forwardPointerButton(e, InputKind.PointerDown);
    this._boundMouseUp = (e) => this.forwardPointerButton(e, InputKind.PointerUp);
    this._boundMouseMove = (e) => this.forwardPointerMove(e);
    this._boundWheel = (e) => this.forwardScroll(e);
  }

  // ── Setup ────────────────────────────────────────────────────────────────────────────────

  onChannelOpen() {
    // First message on the channel, always — the gateway cannot pick a codec (or refuse one it
    // does not support) until it knows what this client can decode.
    this.channel.send(encodeEnvelope(MessageType.Capabilities, encodeCapabilities([Codec.Av1])));
    this.onStatus("connected");
  }

  onMessage(event) {
    let decoded;
    try {
      decoded = decodeEnvelope(event.data);
    } catch (err) {
      console.error("rime: malformed message from the gateway", err);
      return;
    }
    switch (decoded.type) {
      case MessageType.StreamConfig:
        // Geometry/codec parameters for the DataChannel `Frame` path (contract.md: sent "ONLY for
        // a non-AV1 codec"). AV1 arrives on the video track and needs none of this, and the
        // LAN-only LZ4 path this would configure is not implemented yet (ADR-0052) — nothing to
        // do with it today.
        break;
      case MessageType.Bye:
        this.onStatus("the session ended");
        this.leave();
        break;
      default:
        // The editor band (0x0200..0x02FF) and anything newer than this build lands here; the
        // protocol's own forward-compatibility rule (lib.rs `MessageType::Other`) is to ignore
        // what you don't recognise, not to treat it as an error.
        break;
    }
  }

  startFrameWatch() {
    if (typeof this.videoEl.requestVideoFrameCallback !== "function") {
      // No stall signal available; the honest-failure text in app.js already told the person
      // which browsers this page fully supports.
      return;
    }
    this.lastFrameAt = performance.now();
    const onFrame = () => {
      this.lastFrameAt = performance.now();
      if (!this.left) {
        this.videoEl.requestVideoFrameCallback(onFrame);
      }
    };
    this.videoEl.requestVideoFrameCallback(onFrame);
    this.stallTimer = setInterval(() => {
      if (performance.now() - this.lastFrameAt > STALL_TIMEOUT_MS) {
        this.sendKeyframeRequest();
      }
    }, 500);
  }

  attachInputListeners() {
    const el = this.videoEl;
    el.tabIndex = 0;
    el.addEventListener("click", () => {
      if (document.pointerLockElement !== el) {
        el.requestPointerLock();
      }
    });
    // Keyboard/mouse are only forwarded while pointer-locked to this element, so a person typing
    // into the browser's own chrome (or a form on this very page) never leaks keystrokes to the
    // engine process.
    document.addEventListener("keydown", this._boundKeyDown);
    document.addEventListener("keyup", this._boundKeyUp);
    el.addEventListener("mousedown", this._boundMouseDown);
    el.addEventListener("mouseup", this._boundMouseUp);
    el.addEventListener("mousemove", this._boundMouseMove);
    el.addEventListener("wheel", this._boundWheel, { passive: true });
  }

  detachInputListeners() {
    document.removeEventListener("keydown", this._boundKeyDown);
    document.removeEventListener("keyup", this._boundKeyUp);
    const el = this.videoEl;
    el.removeEventListener("mousedown", this._boundMouseDown);
    el.removeEventListener("mouseup", this._boundMouseUp);
    el.removeEventListener("mousemove", this._boundMouseMove);
    el.removeEventListener("wheel", this._boundWheel);
  }

  // ── Input forwarding ────────────────────────────────────────────────────────────────────

  forwardKey(event, kind) {
    if (document.pointerLockElement !== this.videoEl) return;
    event.preventDefault();
    this.sendInput({
      kind,
      code: keyOrdinalOf(event),
      x: 0,
      y: 0,
      scrollX: 0,
      scrollY: 0,
      mods: modsOf(event),
    });
  }

  forwardPointerButton(event, kind) {
    if (document.pointerLockElement !== this.videoEl) return;
    this.sendInput({
      kind,
      code: event.button,
      x: 0,
      y: 0,
      scrollX: 0,
      scrollY: 0,
      mods: modsOf(event),
    });
  }

  forwardPointerMove(event) {
    if (document.pointerLockElement !== this.videoEl) return;
    // Pointer-locked motion is reported as a delta (`movementX`/`movementY`), not an absolute
    // position — `clientX`/`clientY` stay frozen once the pointer is locked, so there is no
    // absolute position to send. INFERRED: nothing in the contract says whether `x`/`y` on a
    // locked `PointerMove` means a delta or a position; a delta is what a locked pointer's own
    // browser event carries, so it is what this page forwards.
    this.sendInput({
      kind: InputKind.PointerMove,
      code: 0,
      x: event.movementX,
      y: event.movementY,
      scrollX: 0,
      scrollY: 0,
      mods: modsOf(event),
    });
  }

  forwardScroll(event) {
    if (document.pointerLockElement !== this.videoEl) return;
    this.sendInput({
      kind: InputKind.PointerScroll,
      code: 0,
      x: 0,
      y: 0,
      scrollX: event.deltaX,
      scrollY: event.deltaY,
      mods: modsOf(event),
    });
  }

  sendInput(fields) {
    if (this.channel.readyState !== "open") return;
    this.seq += 1;
    // `performance.now()` is milliseconds as a float; the wire wants integer microseconds, and
    // client_us is u64 — carried as BigInt past this point so a long session never loses
    // precision to float64 (see protocol.js's module doc).
    const clientUs = BigInt(Math.round(performance.now() * 1000));
    const event = { ...fields, clientUs, seq: this.seq };
    this.channel.send(encodeEnvelope(MessageType.Input, encodeInputEvent(event)));
  }

  sendKeyframeRequest() {
    if (this.channel.readyState !== "open") return;
    this.channel.send(encodeEnvelope(MessageType.KeyframeRequest, new Uint8Array(0)));
  }

  // ── Teardown ─────────────────────────────────────────────────────────────────────────────

  async leave() {
    if (this.left) return;
    this.left = true;
    this.detachInputListeners();
    if (this.stallTimer !== null) {
      clearInterval(this.stallTimer);
      this.stallTimer = null;
    }
    try {
      if (this.channel.readyState === "open") {
        this.channel.send(encodeEnvelope(MessageType.Bye, new Uint8Array(0)));
      }
    } catch (err) {
      console.error("rime: could not send Bye", err);
    }
    this.pc.close();
    try {
      await api.deleteSession(this.sessionId);
    } catch (err) {
      console.error("rime: could not delete the session", err);
    }
  }
}

/**
 * Stand up one session end to end, following contract.md's exact order: create the session, fetch
 * ICE servers, negotiate a `recvonly` video transceiver plus an `"input"` DataChannel, gather ICE
 * fully (non-trickle), then exchange SDP over `POST .../offer`. `onStatus(text)` is called with
 * every honest-failure and progress sentence page-spec.md asks for; this function never touches
 * the DOM directly, so `app.js` owns exactly where that text is shown.
 */
export async function startSession(game, surface, videoEl, onStatus) {
  onStatus("starting the session…");
  const created = await api.createSession(game, surface);
  const sessionId = created.session;

  onStatus("fetching media servers…");
  const iceServers = await api.getIceServers(sessionId);

  const pc = new RTCPeerConnection({ iceServers });
  const transceiver = pc.addTransceiver("video", { direction: "recvonly" });
  preferAv1(transceiver);
  pc.ontrack = (event) => {
    videoEl.srcObject = event.streams[0] ?? new MediaStream([event.track]);
  };

  const channel = pc.createDataChannel("input", { ordered: true });
  const handle = new SessionHandle(sessionId, pc, channel, videoEl, onStatus);
  channel.addEventListener("open", () => handle.onChannelOpen());
  channel.addEventListener("message", (event) => handle.onMessage(event));
  channel.addEventListener("close", () => onStatus("the input channel closed"));

  pc.oniceconnectionstatechange = () => {
    if (pc.iceConnectionState === "failed") {
      onStatus("could not reach the media server (UDP may be blocked)");
    }
  };

  onStatus("negotiating…");
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  // Non-trickle: the contract's one round trip IS the whole exchange, so every candidate has to
  // be in the offer before it is sent (ADR-0049's "signalling, ICE and SDP before anything
  // renders" left trickle out of scope, and there is no `/answer`-side channel to trickle onto).
  await waitForIceGatheringComplete(pc);

  let answerSdp;
  try {
    answerSdp = await api.postOffer(sessionId, pc.localDescription.sdp);
  } catch (err) {
    pc.close();
    await api.deleteSession(sessionId).catch(() => {});
    throw err;
  }
  await pc.setRemoteDescription({ type: "answer", sdp: answerSdp });

  handle.attachInputListeners();
  handle.startFrameWatch();
  return handle;
}
