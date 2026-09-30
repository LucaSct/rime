// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Views and wiring — the only module that touches the DOM directly. Everything else (api.js,
// webauthn.js, session.js) is DOM-free on purpose, so this file is the one place a reviewer has
// to look to see what a person actually sees. No framework, no virtual DOM: `h()` below is a
// twelve-line builder, not a rendering system, because a page this small does not need one and
// this codebase ships inside every exported game (ADR-0046 §2) — a dependency here is a
// dependency there.
//
// Every view is a full replacement of `#app`'s contents rather than a diffed update. That is a
// deliberate simplicity trade: at this page's size (five views, one active at a time) the cost is
// a handful of DOM nodes rebuilt per navigation, and the benefit is that there is no view state
// that can go stale relative to what is on screen.

import * as api from "./api.js";
import * as webauthn from "./webauthn.js";
import { startSession, av1DecodeSupported } from "./session.js";
import { pairButton, renderPhonePairing, takePairFragment } from "./pair.js";

const root = document.getElementById("app");

/** The session currently on screen, if any — so `pagehide` and the "Leave" button can tear down
 * the same thing. */
let activeSession = null;

function clear(el) {
  while (el.firstChild) {
    el.removeChild(el.firstChild);
  }
}

/** A minimal `h("tag", {attr: value, onClick: fn, text: "…"}, [children])` DOM builder. `text` sets
 * `textContent`; any `on*` key whose value is a function is wired as a listener; everything else
 * is an attribute. */
function h(tag, attrs = {}, children = []) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === "text") {
      el.textContent = value;
    } else if (key.startsWith("on") && typeof value === "function") {
      el.addEventListener(key.slice(2).toLowerCase(), value);
    } else {
      el.setAttribute(key, value);
    }
  }
  for (const child of [].concat(children)) {
    if (child == null) continue;
    el.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
  }
  return el;
}

/** Every failure this page shows is text a person can read (page-spec.md's "honest failure
 * states"): an `ApiError`'s message is already the gateway's own `error` field, and a
 * `navigator.credentials.*` rejection is a `DOMException` whose `.message` the browser itself
 * wrote in plain language ("The operation either timed out or was not allowed…", etc). */
function messageFor(err) {
  if (err instanceof api.ApiError) return err.message;
  if (err instanceof Error && err.message) return err.message;
  return "Something went wrong.";
}

/** Switch the page's layout to `view` ("auth", "catalogue", "session") and put `actions` in the top
 * bar's right-hand slot. The layout is CSS keyed off `body[data-view]` — a view never sets widths
 * itself — so the column is narrow for a form, wide for the grid and edge to edge for a stream. */
function setView(view, actions = []) {
  document.body.dataset.view = view;
  const slot = document.getElementById("topbar-actions");
  if (slot) {
    clear(slot);
    for (const action of actions) slot.appendChild(action);
  }
}

/** Clear `#app` and hand back a fresh card to build an auth-style view in. */
function authCard() {
  clear(root);
  setView("auth");
  const card = h("section", { class: "card" });
  root.appendChild(card);
  return card;
}

function backToSignIn() {
  return h("nav", {}, [
    h("a", {
      href: "#",
      text: "Back to sign in",
      onclick: (e) => {
        e.preventDefault();
        renderSignIn();
      },
    }),
  ]);
}

// ── Sign in ───────────────────────────────────────────────────────────────────────────────────

function renderSignIn() {
  const card = authCard();
  const status = h("p", { class: "status", role: "status" });
  const email = h("input", {
    type: "email",
    id: "signin-email",
    name: "email",
    required: "",
    autocomplete: "email webauthn",
  });
  const form = h(
    "form",
    {
      "aria-label": "Sign in",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          const started = await api.loginOptions(email.value);
          const publicKey = webauthn.decodeRequestOptions(started.options.publicKey);
          const credential = await navigator.credentials.get({ publicKey });
          const body = webauthn.encodeAssertionResponse(credential);
          await api.loginFinish(started.challenge, body);
          await renderCatalogueOrSignIn();
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    },
    [
      h("h1", { text: "Sign in" }),
      h("p", { class: "lede", text: "Play and edit Rime games in your browser." }),
      h("label", { for: "signin-email", text: "Email" }),
      email,
      h("button", { type: "submit", text: "Sign in with a passkey" }),
      status,
    ],
  );
  card.appendChild(form);
  card.appendChild(pairButton(pairUi()));
  card.appendChild(
    h("nav", {}, [
      h("a", {
        href: "#register",
        text: "Register with an invitation",
        onclick: (e) => {
          e.preventDefault();
          renderRegister();
        },
      }),
      h("a", {
        href: "#recover",
        text: "Recover your account",
        onclick: (e) => {
          e.preventDefault();
          renderRecover();
        },
      }),
    ]),
  );
}

// ── Register: invitation -> mailed code -> passkey (ADR-0048 decision 1's ordering) ─────────────

function renderRegister() {
  const card = authCard();
  const status = h("p", { class: "status", role: "status" });
  const step = h("div", { id: "register-step" });
  const invitation = h("input", { type: "text", id: "register-invitation", name: "invitation", required: "" });
  const form = h(
    "form",
    {
      "aria-label": "Register with an invitation",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          const begun = await api.registerBegin(invitation.value);
          renderRegisterCodeStep(begun.transaction, step, status);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    },
    [
      h("h1", { text: "Register" }),
      h("label", { for: "register-invitation", text: "Invitation code" }),
      invitation,
      h("button", { type: "submit", text: "Continue" }),
    ],
  );
  card.append(form, step, status, backToSignIn());
}

function renderRegisterCodeStep(tx, container, status) {
  clear(container);
  const code = h("input", {
    type: "text",
    id: "register-code",
    name: "code",
    inputmode: "numeric",
    maxlength: "8",
    required: "",
  });
  const form = h(
    "form",
    {
      "aria-label": "Enter the mailed code",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          await api.registerCode(tx, code.value);
          renderRegisterPasskeyStep(tx, container, status);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    },
    [
      h("p", { text: "Check your email for an eight-digit code." }),
      h("label", { for: "register-code", text: "Code" }),
      code,
      h("button", { type: "submit", text: "Confirm code" }),
    ],
  );
  container.appendChild(form);
}

function renderRegisterPasskeyStep(tx, container, status) {
  clear(container);
  container.appendChild(h("p", { text: "Code confirmed. Create a passkey to finish registering." }));
  container.appendChild(
    h("button", {
      type: "button",
      id: "register-create-passkey",
      text: "Create a passkey",
      onclick: async () => {
        status.textContent = "";
        try {
          const started = await api.registerOptions(tx);
          const publicKey = webauthn.decodeCreationOptions(started.options.publicKey);
          const credential = await navigator.credentials.create({ publicKey });
          const body = webauthn.encodeAttestationResponse(credential);
          const done = await api.registerPasskey(tx, body);
          renderRecoverySecret(done.recovery_secret, renderCatalogueOrSignIn);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    }),
  );
}

// ── Recover: both proofs, then a replacement passkey (ADR-0048: mail can never replace a passkey) ─

function renderRecover() {
  const card = authCard();
  const status = h("p", { class: "status", role: "status" });
  const step = h("div", { id: "recover-step" });
  const email = h("input", { type: "email", id: "recover-email", name: "email", required: "" });
  const form = h(
    "form",
    {
      "aria-label": "Recover your account",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          const begun = await api.recoverBegin(email.value);
          renderRecoverProofsStep(begun.transaction, step, status);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    },
    [
      h("h1", { text: "Recover your account" }),
      h("label", { for: "recover-email", text: "Email" }),
      email,
      h("button", { type: "submit", text: "Continue" }),
    ],
  );
  card.append(form, step, status, backToSignIn());
}

function renderRecoverProofsStep(tx, container, status) {
  clear(container);
  const secret = h("input", { type: "text", id: "recover-secret", name: "secret", required: "" });
  const code = h("input", { type: "text", id: "recover-code", name: "code", required: "" });
  const form = h(
    "form",
    {
      "aria-label": "Prove it is you",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          await api.recoverProofs(tx, secret.value, code.value);
          renderRecoverPasskeyStep(tx, container, status);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    },
    [
      h("p", { text: "Recovery needs both your recovery secret and a fresh mailed code." }),
      h("label", { for: "recover-secret", text: "Recovery secret" }),
      secret,
      h("label", { for: "recover-code", text: "Mailed code" }),
      code,
      h("button", { type: "submit", text: "Continue" }),
    ],
  );
  container.appendChild(form);
}

function renderRecoverPasskeyStep(tx, container, status) {
  clear(container);
  container.appendChild(
    h("p", {
      text: "Proofs accepted. Creating a new passkey replaces every existing one and signs out every device.",
    }),
  );
  container.appendChild(
    h("button", {
      type: "button",
      text: "Create a replacement passkey",
      onclick: async () => {
        status.textContent = "";
        try {
          const started = await api.recoverOptions(tx);
          const publicKey = webauthn.decodeCreationOptions(started.options.publicKey);
          const credential = await navigator.credentials.create({ publicKey });
          const body = webauthn.encodeAttestationResponse(credential);
          const done = await api.recoverPasskey(tx, body);
          renderRecoverySecret(done.recovery_secret, renderCatalogueOrSignIn);
        } catch (err) {
          status.textContent = messageFor(err);
        }
      },
    }),
  );
}

function renderRecoverySecret(secret, onContinue) {
  const card = authCard();
  card.appendChild(h("h1", { text: "Save your recovery secret" }));
  card.appendChild(
    h("p", {
      text:
        "This is shown once and never again. Store it somewhere safe: it is needed, together " +
        "with a mailed code, if every passkey is ever lost.",
    }),
  );
  card.appendChild(h("code", { id: "recovery-secret", text: secret }));
  card.appendChild(
    h("button", {
      type: "button",
      text: "I have saved it — continue",
      onclick: () => onContinue(),
    }),
  );
}

// ── Catalogue ─────────────────────────────────────────────────────────────────────────────────

function surfaceLabel(surface) {
  return surface === "edit" ? "Open in the editor" : "Play";
}

async function renderCatalogueOrSignIn() {
  clear(root);
  root.appendChild(h("p", { class: "status", role: "status", text: "Loading…" }));
  try {
    const catalogue = await api.getCatalogue();
    renderCatalogue(catalogue.games);
  } catch (err) {
    if (err instanceof api.ApiError && err.status === 401) {
      renderSignIn();
    } else {
      renderFatalError(messageFor(err));
    }
  }
}

function renderCatalogue(games) {
  clear(root);
  setView("catalogue", [
    h("button", {
      type: "button",
      class: "secondary small",
      text: "Sign out",
      onclick: async () => {
        try {
          await api.logout();
        } catch (err) {
          console.error("rime: sign-out failed", err);
        }
        renderCatalogueOrSignIn();
      },
    }),
  ]);
  root.appendChild(
    h("div", { class: "catalogue-head" }, [
      h("h1", { text: "Games" }),
      h("p", {
        class: "lede",
        text: "Each one runs on this server and streams to your browser — pick one to start.",
      }),
    ]),
  );
  const list = h("ul", { class: "catalogue" });
  for (const game of games) {
    const actions = h("div", { class: "game-actions" });
    // Play first and loud, the editor second and quiet: a card should have one obvious thing to
    // press, and for a visitor that is playing — whatever order the gateway happens to list them.
    const surfaces = [...game.surfaces].sort((a, b) => (a === "play" ? -1 : b === "play" ? 1 : 0));
    surfaces.forEach((surface, i) => {
      actions.appendChild(
        h("button", {
          type: "button",
          class: i === 0 ? "" : "secondary",
          text: surfaceLabel(surface),
          onclick: () => beginSession(game, surface),
        }),
      );
    });
    list.appendChild(
      h("li", {}, [
        h("div", { class: "game-cover", "aria-hidden": "true" }),
        h("div", { class: "game-body" }, [h("h2", { text: game.title }), actions]),
      ]),
    );
  }
  root.appendChild(list);
  if (games.length === 0) {
    root.appendChild(h("p", { class: "lede", text: "No games are available on this host yet." }));
  }
}

// ── Session ───────────────────────────────────────────────────────────────────────────────────

/** Listeners the session view attaches to `document` — kept so leaving can take them off again
 * (the view is rebuilt on every visit, and a stale listener would toggle a detached stage). */
let stageListeners = [];

function removeStageListeners() {
  for (const [type, fn] of stageListeners) document.removeEventListener(type, fn);
  stageListeners = [];
}

/** Fullscreen the stage, then take the pointer and — where the browser has it — the keyboard.
 *
 * Keyboard Lock (`navigator.keyboard.lock()`, Chromium only, and only while fullscreen) is what lets
 * a game receive keys the browser would otherwise keep for itself: Esc, Ctrl+W, Alt+Tab-adjacent
 * shortcuts. With it, a quick Esc goes to the game and **holding** Esc leaves fullscreen — the
 * browser shows that hint itself. Without it (Firefox), Esc releases the pointer as usual. */
async function enterFullscreen(stage, video) {
  try {
    if (!document.fullscreenElement) await stage.requestFullscreen({ navigationUI: "hide" });
    if (navigator.keyboard && typeof navigator.keyboard.lock === "function") {
      await navigator.keyboard.lock();
    }
  } catch (err) {
    console.warn("rime: fullscreen was refused", err);
  }
  if (document.pointerLockElement !== video) video.requestPointerLock();
}

async function beginSession(game, surface) {
  if (!av1DecodeSupported()) {
    // ADR-0052: AV1 on a WebRTC track is the only decode path this page offers today. A browser
    // that cannot decode it gets this sentence instead of a black video.
    renderFatalError(
      "This browser cannot decode AV1 video, so it cannot show a running session yet " +
        "(this is expected on Safari and some mobile browsers today).",
    );
    return;
  }
  clear(root);
  removeStageListeners();
  const video = h("video", { autoplay: "", playsinline: "", id: "session-video" });
  // Input only flows while the pointer is locked to the video (session.js explains why), so the
  // person has to be told to click — without this line the stream looks live but ignores them.
  const hint = h("div", { class: "stage-hint" }, [
    h("span", { text: "Click the picture to play · Esc gives the mouse back" }),
  ]);
  const status = h("p", { class: "status", role: "status", text: "Starting…" });
  const stage = h("section", { class: "stage", "aria-label": game.title });
  // Wired through the `onclick` PROPERTY (below, and in onFullscreen) rather than `h`'s listener,
  // so the label and the action it performs are swapped together in one place.
  const fullscreen = h("button", { type: "button", class: "secondary small", text: "Fullscreen" });
  fullscreen.onclick = () => enterFullscreen(stage, video);
  const leave = h("button", {
    type: "button",
    class: "secondary small",
    text: "Leave",
    onclick: async () => {
      const session = activeSession;
      activeSession = null;
      removeStageListeners();
      if (document.fullscreenElement) await document.exitFullscreen().catch(() => {});
      if (session) await session.leave();
      renderCatalogueOrSignIn();
    },
  });
  const controls = h("div", { class: "controls" }, [fullscreen, leave]);
  stage.append(video, hint, h("div", { class: "stage-bar" }, [status, controls]));
  root.appendChild(stage);
  setView("session", [
    h("span", {
      class: "topbar-title",
      text: `${game.title} · ${surface === "edit" ? "editor" : "play"}`,
    }),
  ]);

  const onLock = () => stage.classList.toggle("locked", document.pointerLockElement === video);
  const onFullscreen = () => {
    fullscreen.textContent = document.fullscreenElement === stage ? "Exit fullscreen" : "Fullscreen";
    fullscreen.onclick =
      document.fullscreenElement === stage
        ? () => document.exitFullscreen().catch(() => {})
        : () => enterFullscreen(stage, video);
    if (!document.fullscreenElement && navigator.keyboard && navigator.keyboard.unlock) {
      navigator.keyboard.unlock();
    }
  };
  stageListeners = [
    ["pointerlockchange", onLock],
    ["fullscreenchange", onFullscreen],
  ];
  for (const [type, fn] of stageListeners) document.addEventListener(type, fn);

  try {
    activeSession = await startSession(game.id, surface, video, (text) => {
      status.textContent = text;
    });
  } catch (err) {
    status.textContent = messageFor(err);
  }
}

function renderFatalError(text) {
  const card = authCard();
  card.appendChild(h("p", { class: "error", role: "alert", text }));
  card.appendChild(
    h("button", { type: "button", text: "Try again", onclick: () => renderCatalogueOrSignIn() }),
  );
}

// A session view that gets abandoned (a tab closed, a navigation) still owns an engine process
// and a render slot until told otherwise — `pagehide` fires reliably in that case (unlike
// `beforeunload`, which mobile Safari and bfcache both make unreliable), so it is where `Bye` and
// the `DELETE` go. Best-effort: nothing waits for either to land before the page is gone.
window.addEventListener("pagehide", () => {
  if (activeSession) {
    activeSession.leave();
  }
});

/** What pair.js needs from this module (see its header for why it is passed rather than imported). */
function pairUi() {
  return { root, h, clear, messageFor, back: renderSignIn, done: renderCatalogueOrSignIn };
}

// A phone that scanned a desktop's QR code lands on `/#pair=<id>` (ADR-0055).
const pairing = takePairFragment();
if (pairing) {
  renderPhonePairing(pairUi(), pairing);
} else {
  renderCatalogueOrSignIn();
}
