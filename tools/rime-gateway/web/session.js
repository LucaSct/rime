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
  decodeStreamConfig,
} from "./protocol.js";
import { hidUsageOf } from "./keymap.js";

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

// Gathering is capped: an unreachable TURN server can keep a browser gathering for tens of
// seconds before it gives up on that candidate. After the cap the offer goes out with whatever
// candidates exist — the gateway is ICE-lite and only needs one that works.
const ICE_GATHERING_CAP_MS = 5000;

function waitForIceGatheringComplete(pc) {
  if (pc.iceGatheringState === "complete") {
    return Promise.resolve();
  }
  return new Promise((resolve) => {
    const timer = setTimeout(done, ICE_GATHERING_CAP_MS);
    function onChange() {
      if (pc.iceGatheringState === "complete") done();
    }
    function done() {
      clearTimeout(timer);
      pc.removeEventListener("icegatheringstatechange", onChange);
      resolve();
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
    // The stream's own pixel space, as `StreamConfig` declared it. Null until one arrives; the
    // AV1 path learns the same numbers from the decoded track instead (see frameSize()).
    this.frameWidth = 0;
    this.frameHeight = 0;
    // The pointer, in STREAM-FRAME pixels — the coordinate space `InputEvent.x/y` is defined in
    // (protocol.hpp). Under pointer lock there is no browser-supplied position to send, so the
    // page integrates `movementX/Y` into this one instead; see forwardPointerMove.
    this.pointerX = 0;
    this.pointerY = 0;
    this._boundKeyDown = (e) => this.forwardKey(e, InputKind.KeyDown);
    this._boundKeyUp = (e) => this.forwardKey(e, InputKind.KeyUp);
    this._boundMouseDown = (e) => this.forwardPointerButton(e, InputKind.PointerDown);
    this._boundMouseUp = (e) => this.forwardPointerButton(e, InputKind.PointerUp);
    this._boundMouseMove = (e) => this.forwardPointerMove(e);
    this._boundWheel = (e) => this.forwardScroll(e);
    this._boundLockChange = () => this.onPointerLockChange();
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
        // The DataChannel `Frame` path this configures is not implemented (AV1 arrives on the
        // video track, and the LAN-only LZ4 path waits on its own brick, ADR-0052) — but the
        // GEOMETRY is used: it is the authoritative statement of the pixel space `InputEvent.x/y`
        // are expressed in, and the engine re-sends it whenever the stream's size changes.
        try {
          const cfg = decodeStreamConfig(decoded.payload);
          this.frameWidth = cfg.width;
          this.frameHeight = cfg.height;
        } catch (err) {
          console.error("rime: malformed StreamConfig", err);
        }
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
      // A hidden tab stops video frame callbacks, which is not a stall — asking for keyframes
      // there would make the encoder send its most expensive frame for nobody to see.
      if (document.hidden) {
        this.lastFrameAt = performance.now();
        return;
      }
      if (performance.now() - this.lastFrameAt > STALL_TIMEOUT_MS) {
        this.sendKeyframeRequest();
        // Restart the stall window: one request per STALL_TIMEOUT_MS at most. Without this, a
        // real stall would ask four times a second, and every answer is a full keyframe.
        this.lastFrameAt = performance.now();
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
    document.addEventListener("pointerlockchange", this._boundLockChange);
  }

  detachInputListeners() {
    document.removeEventListener("keydown", this._boundKeyDown);
    document.removeEventListener("keyup", this._boundKeyUp);
    const el = this.videoEl;
    el.removeEventListener("mousedown", this._boundMouseDown);
    el.removeEventListener("mouseup", this._boundMouseUp);
    el.removeEventListener("mousemove", this._boundMouseMove);
    el.removeEventListener("wheel", this._boundWheel);
    document.removeEventListener("pointerlockchange", this._boundLockChange);
  }

  /** Put the virtual cursor in the MIDDLE of the frame each time the pointer locks. Starting it at
   * (0, 0) would begin every session in the frame's top-left corner, which is a real position the
   * engine would believe — a pick or a UI hit-test on the first click would land in the corner
   * rather than where the person is looking. */
  onPointerLockChange() {
    if (document.pointerLockElement !== this.videoEl) return;
    const frame = this.frameSize();
    this.pointerX = frame.width / 2;
    this.pointerY = frame.height / 2;
  }

  // ── Pointer coordinates ─────────────────────────────────────────────────────────────────
  //
  // `InputEvent.x/y` are STREAM-FRAME pixels: the coordinate system of the image the server is
  // sending, not of this page's layout. That is the contract protocol.hpp states and the one the
  // native client (samples/04-remote-view) has always followed — it scales window pixels into
  // frame pixels before sending. This page previously sent raw `movementX/Y` deltas, which is a
  // different quantity in a different space; it is the side that was wrong.
  //
  // Doing the scaling HERE rather than on the server is deliberate: only the client knows how big
  // its video box is, and a server that had to be told would need a second message and a window
  // where the two disagree.

  /** The stream's pixel size. `StreamConfig` is authoritative when one has arrived; otherwise the
   * decoded track's own dimensions, which for the AV1 path are the same numbers by construction.
   * Zero (no frame decoded yet, no config) means "unknown" and the caller passes coordinates
   * through unscaled rather than dividing by zero. */
  frameSize() {
    if (this.frameWidth > 0 && this.frameHeight > 0) {
      return { width: this.frameWidth, height: this.frameHeight };
    }
    return { width: this.videoEl.videoWidth || 0, height: this.videoEl.videoHeight || 0 };
  }

  /** Where the video's PICTURE actually is inside the element's box, and how big it is there.
   * A `<video>` letterboxes (`object-fit: contain` is the default), so the element's box is not
   * the picture: on a 16:9 element showing a 4:3 stream there are bars down both sides, and
   * measuring against the box would put the cursor several degrees off near the edges. */
  displayedVideoRect() {
    const box = this.videoEl.getBoundingClientRect();
    const frame = this.frameSize();
    if (frame.width === 0 || frame.height === 0 || box.width === 0 || box.height === 0) {
      return { left: box.left, top: box.top, width: box.width, height: box.height };
    }
    const scale = Math.min(box.width / frame.width, box.height / frame.height);
    const width = frame.width * scale;
    const height = frame.height * scale;
    return {
      left: box.left + (box.width - width) / 2,
      top: box.top + (box.height - height) / 2,
      width,
      height,
    };
  }

  /** CSS pixels of motion -> stream-frame pixels of motion. */
  motionScale() {
    const rect = this.displayedVideoRect();
    const frame = this.frameSize();
    if (rect.width === 0 || rect.height === 0 || frame.width === 0 || frame.height === 0) {
      return { x: 1, y: 1 };
    }
    return { x: frame.width / rect.width, y: frame.height / rect.height };
  }

  // ── Input forwarding ────────────────────────────────────────────────────────────────────

  forwardKey(event, kind) {
    if (document.pointerLockElement !== this.videoEl) return;
    event.preventDefault();
    this.sendInput({
      kind,
      code: hidUsageOf(event),
      x: 0,
      y: 0,
      scrollX: 0,
      scrollY: 0,
      mods: modsOf(event),
    });
  }

  forwardPointerButton(event, kind) {
    if (document.pointerLockElement !== this.videoEl) return;
    // `event.button` goes on the wire in DOM order (0 left, 1 middle, 2 right) — see the
    // `InputEvent` comment in protocol.hpp; the engine swaps middle/right into its own
    // `MouseButton` order. A button carries the CURRENT pointer position rather than zeros, so a
    // click is a click somewhere.
    this.sendInput({
      kind,
      code: event.button,
      x: Math.round(this.pointerX),
      y: Math.round(this.pointerY),
      scrollX: 0,
      scrollY: 0,
      mods: modsOf(event),
    });
  }

  forwardPointerMove(event) {
    if (document.pointerLockElement !== this.videoEl) return;
    // Pointer-locked motion arrives as a delta (`movementX/Y`); `clientX/Y` freeze the moment the
    // pointer locks, so there is no browser-supplied position to scale. The page therefore keeps
    // the position itself: integrate the delta, converted from CSS pixels to frame pixels, and
    // send the running total — which is what the wire's absolute `x`/`y` mean.
    //
    // NOT CLAMPED TO THE FRAME, and that is the interesting choice. A clamped virtual cursor is
    // what a desktop with a real pointer has, and it is exactly why desktop first-person games
    // use raw deltas instead: the engine reconstructs its `MouseMove` delta as (this − previous),
    // so clamping at an edge would zero the delta and the camera would refuse to keep turning
    // mid-look. The units stay stream-frame pixels either way; only the range is unbounded, and a
    // consumer that wants a cursor rather than a look-delta clamps it itself.
    const scale = this.motionScale();
    this.pointerX += event.movementX * scale.x;
    this.pointerY += event.movementY * scale.y;
    this.sendInput({
      kind: InputKind.PointerMove,
      code: 0,
      x: Math.round(this.pointerX),
      y: Math.round(this.pointerY),
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
  // The spec's default `binaryType` is "blob" (Firefox follows it; Chrome defaults to
  // "arraybuffer"). A Blob cannot be read synchronously, so without this every message from the
  // gateway would decode as an empty envelope in Firefox and be dropped as malformed.
  channel.binaryType = "arraybuffer";
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
