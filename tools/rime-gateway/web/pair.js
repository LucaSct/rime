// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Signing a browser in with your phone (ADR-0055) — both halves of it, the desktop's and the
// phone's, in one module so app.js only has to mount a button and route one URL fragment.
//
// The desktop never holds a credential. It shows a QR code naming a pairing; the phone signs in
// with its passkey FOR that pairing and is shown an eight-digit code; the person types the code into
// the desktop, is shown which account approved it, and only then does the desktop get a session.
//
// Why the id travels in the URL FRAGMENT (`/#pair=<id>`): a browser never sends the part after `#`
// to a server, so the capability never appears in a request line, a proxy log or a Referer. The
// phone strips it from the address bar the moment it has read it, so it is not left in history
// either.
//
// app.js passes its own tiny DOM helpers in as `ui` ({root, h, clear, messageFor, back, done})
// rather than this module importing them: app.js runs its first render at import time, and a
// module cycle through it would be an ordering puzzle for no gain.

import * as api from "./api.js";
import * as webauthn from "./webauthn.js";

// The desktop polls every four seconds, not two. The account routes share one per-caller budget
// (auth_api.rs: 20 requests a minute, refilling one every three seconds), and the phone approving
// the pairing is usually behind the same home router — the same caller. A two-second poll would
// drain that budget in about a minute and starve the phone's own approval; four seconds refills
// faster than it spends, so the poll can run for the pairing's whole five minutes.
const POLL_MS = 4000;

const PAIR_ID = /^[0-9a-f]{32}$/;

/** If the page was opened as `/#pair=<id>`, return the id and remove it from the address bar;
 * otherwise null. Reading it does nothing else — loading the page reserves and approves nothing. */
export function takePairFragment() {
  const prefix = "#pair=";
  const hash = window.location.hash;
  if (!hash.startsWith(prefix)) return null;
  history.replaceState(null, "", window.location.pathname + window.location.search);
  const id = hash.slice(prefix.length);
  return PAIR_ID.test(id) ? id : null;
}

/** The "Sign in with my phone" button the sign-in view shows next to the passkey form. */
export function pairButton(ui) {
  return ui.h("button", {
    type: "button",
    id: "pair-start",
    text: "Sign in with my phone",
    onclick: () => renderDesktopPairing(ui),
  });
}

// ── The desktop: QR + countdown -> code -> "sign in as <email>?" ──────────────────────────────

function renderDesktopPairing(ui) {
  const { root, h, clear, messageFor } = ui;
  clear(root);
  const status = h("p", { class: "status", role: "status", text: "Preparing a code for your phone…" });
  const qr = h("div", { id: "pair-qr", style: "max-width:280px;background:#fff;padding:8px" });
  const countdown = h("p", { id: "pair-countdown" });
  const step = h("div", { id: "pair-step" });
  let id = null;
  let pollTimer = null;
  let tickTimer = null;

  // Everything that keeps running in the background stops here, whichever way the view is left.
  function stop() {
    clearTimeout(pollTimer);
    clearInterval(tickTimer);
    pollTimer = null;
    tickTimer = null;
  }

  function expired() {
    stop();
    clear(qr);
    clear(step);
    countdown.textContent = "";
    status.textContent = "This code has expired or was already used. Start again to get a new one.";
    step.appendChild(h("button", { type: "button", text: "Start again", onclick: () => renderDesktopPairing(ui) }));
  }

  async function cancel() {
    stop();
    if (id) {
      try {
        await api.pairFinish(id, "refuse");
      } catch {
        // Already gone, or never started: either way there is nothing left to refuse.
      }
    }
    ui.back();
  }

  async function poll() {
    try {
      const answer = await api.pairStatus(id);
      if (answer.status === "approved") {
        stop();
        clear(qr);
        countdown.textContent = "";
        renderCodeStep();
        return;
      }
      if (answer.status === "expired") {
        expired();
        return;
      }
    } catch (err) {
      // The one refusal (404) means the pairing is gone — expired, killed, or never ours.
      if (err instanceof api.ApiError && err.status === 404) {
        expired();
        return;
      }
      status.textContent = messageFor(err);
    }
    pollTimer = setTimeout(poll, POLL_MS);
  }

  function renderCodeStep() {
    clear(step);
    status.textContent = "";
    const code = h("input", {
      type: "text",
      id: "pair-code",
      name: "code",
      inputmode: "numeric",
      autocomplete: "one-time-code",
      maxlength: "8",
      required: "",
    });
    step.appendChild(
      h(
        "form",
        {
          "aria-label": "Enter the code from your phone",
          onsubmit: async (e) => {
            e.preventDefault();
            status.textContent = "";
            try {
              const answer = await api.pairRedeem(id, code.value);
              renderConfirmStep(answer.email);
            } catch (err) {
              if (err instanceof api.ApiError && err.status === 404) {
                expired();
              } else {
                status.textContent = messageFor(err);
              }
            }
          },
        },
        [
          h("p", { text: "Your phone approved. Type the eight-digit code it is showing." }),
          h("label", { for: "pair-code", text: "Code" }),
          code,
          h("button", { type: "submit", text: "Continue" }),
        ],
      ),
    );
  }

  // Account substitution is defeated HERE: someone who approved this screen with their own account
  // would sign you in to it, so the page names the account and asks before any session exists.
  function renderConfirmStep(email) {
    clear(step);
    step.append(
      h("p", {}, ["You are signing in as ", h("strong", { text: email }), "."]),
      h("p", { text: "If that is not your account, do not continue." }),
      h("button", {
        type: "button",
        id: "pair-accept",
        text: `Sign in as ${email}`,
        onclick: async () => {
          status.textContent = "";
          try {
            await api.pairFinish(id, "accept");
            ui.done();
          } catch (err) {
            status.textContent = messageFor(err);
          }
        },
      }),
      h("button", { type: "button", text: "That is not me — cancel", onclick: cancel }),
    );
  }

  root.append(
    h("h1", { text: "Sign in with your phone" }),
    h("p", {
      text:
        "Scan this with the phone that holds your passkey. Your phone will ask you to sign in, " +
        "then show a code to type here.",
    }),
    qr,
    countdown,
    step,
    status,
    h("button", { type: "button", text: "Cancel", onclick: cancel }),
  );

  (async () => {
    try {
      const begun = await api.pairBegin();
      id = begun.id;
      // The gateway's own SVG, rendered from a URL it built from configuration — inserted as-is.
      qr.innerHTML = begun.qr_svg;
      status.textContent = "Waiting for your phone…";
      const deadline = Date.now() + begun.expires_in * 1000;
      const tick = () => {
        const left = Math.max(0, Math.round((deadline - Date.now()) / 1000));
        countdown.textContent = `This code expires in ${Math.floor(left / 60)}:${String(left % 60).padStart(2, "0")}.`;
        if (left === 0) expired();
      };
      tick();
      tickTimer = setInterval(tick, 1000);
      pollTimer = setTimeout(poll, POLL_MS);
    } catch (err) {
      status.textContent = messageFor(err);
    }
  })();
}

// ── The phone: warning -> email -> passkey -> the code, large ─────────────────────────────────

function ago(seconds) {
  if (seconds < 60) return "just now";
  const minutes = Math.round(seconds / 60);
  return minutes === 1 ? "about a minute ago" : `about ${minutes} minutes ago`;
}

/** The view a phone sees after scanning. It shows what the desktop claims about itself, labelled as
 * unverified, and a warning — ADR-0055's only mitigation against a relayed phish, so it is the
 * first thing on the screen rather than small print. */
export function renderPhonePairing(ui, id) {
  const { root, h, clear, messageFor } = ui;
  clear(root);
  const status = h("p", { class: "status", role: "status" });
  const about = h("p", { id: "pair-about", text: "Looking up the request…" });
  const step = h("div", { id: "pair-step" });
  const email = h("input", {
    type: "email",
    id: "pair-email",
    name: "email",
    required: "",
    autocomplete: "email webauthn",
  });

  const form = h(
    "form",
    {
      "aria-label": "Approve with your passkey",
      onsubmit: async (e) => {
        e.preventDefault();
        status.textContent = "";
        try {
          const started = await api.pairOptions(id, email.value);
          const publicKey = webauthn.decodeRequestOptions(started.options.publicKey);
          const credential = await navigator.credentials.get({ publicKey });
          const body = webauthn.encodeAssertionResponse(credential);
          const approved = await api.pairApprove(id, started.challenge, body);
          clear(step);
          step.append(
            h("p", { text: "Type this into the other screen:" }),
            h("p", {
              id: "pair-shown-code",
              style: "font-size:2.5rem;font-family:monospace;letter-spacing:0.2em",
              text: approved.code,
            }),
            h("p", { text: "It works once, only on the screen that showed the QR code, for a few minutes." }),
          );
        } catch (err) {
          if (err instanceof api.ApiError && err.status === 404) {
            status.textContent = "This sign-in request has expired or was already used.";
          } else {
            status.textContent = messageFor(err);
          }
        }
      },
    },
    [
      h("label", { for: "pair-email", text: "Your email" }),
      email,
      h("button", { type: "submit", text: "Approve with my passkey" }),
    ],
  );
  step.appendChild(form);

  root.append(
    h("h1", { text: "Authorize another browser to sign in to your account" }),
    h("p", {
      class: "error",
      role: "alert",
      text:
        "Only continue if you started this on your own screen just now. If somebody sent you " +
        "this link or asked you for the code, stop: continuing would sign THEM in to your account.",
    }),
    about,
    step,
    status,
    h("nav", {}, [
      h("a", {
        href: "#",
        text: "Not me — cancel",
        onclick: (e) => {
          e.preventDefault();
          ui.back();
        },
      }),
    ]),
  );

  (async () => {
    try {
      const view = await api.pairDescribe(id);
      about.textContent =
        `Requested ${ago(view.age_secs)} by: ${view.user_agent} ` +
        "(unverified — this is what the other browser says about itself).";
    } catch (err) {
      clear(step);
      about.textContent =
        err instanceof api.ApiError && err.status === 404
          ? "This sign-in request has expired or was already used."
          : messageFor(err);
    }
  })();
}
