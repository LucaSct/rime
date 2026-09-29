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
  clear(root);
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
      h("label", { for: "signin-email", text: "Email" }),
      email,
      h("button", { type: "submit", text: "Sign in with a passkey" }),
      status,
    ],
  );
  root.appendChild(form);
  root.appendChild(
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
  clear(root);
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
  root.append(form, step, status, backToSignIn());
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
  clear(root);
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
  root.append(form, step, status, backToSignIn());
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
  clear(root);
  root.appendChild(h("h1", { text: "Save your recovery secret" }));
  root.appendChild(
    h("p", {
      text:
        "This is shown once and never again. Store it somewhere safe: it is needed, together " +
        "with a mailed code, if every passkey is ever lost.",
    }),
  );
  root.appendChild(h("code", { id: "recovery-secret", text: secret }));
  root.appendChild(
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
  root.appendChild(h("h1", { text: "Rime" }));
  root.appendChild(
    h("button", {
      type: "button",
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
  );
  const list = h("ul", { class: "catalogue" });
  for (const game of games) {
    const item = h("li", {}, [h("h2", { text: game.title })]);
    for (const surface of game.surfaces) {
      item.appendChild(
        h("button", {
          type: "button",
          text: surfaceLabel(surface),
          onclick: () => beginSession(game.id, surface),
        }),
      );
    }
    list.appendChild(item);
  }
  root.appendChild(list);
  if (games.length === 0) {
    root.appendChild(h("p", { text: "No games are available on this host yet." }));
  }
}

// ── Session ───────────────────────────────────────────────────────────────────────────────────

async function beginSession(gameId, surface) {
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
  const video = h("video", { autoplay: "", playsinline: "", id: "session-video" });
  const status = h("p", { class: "status", role: "status", text: "Starting…" });
  const leave = h("button", {
    type: "button",
    text: "Leave",
    onclick: async () => {
      const session = activeSession;
      activeSession = null;
      if (session) await session.leave();
      renderCatalogueOrSignIn();
    },
  });
  root.append(video, status, leave);
  try {
    activeSession = await startSession(gameId, surface, video, (text) => {
      status.textContent = text;
    });
  } catch (err) {
    status.textContent = messageFor(err);
  }
}

function renderFatalError(text) {
  clear(root);
  root.appendChild(h("p", { class: "error", role: "alert", text }));
  root.appendChild(
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

renderCatalogueOrSignIn();
