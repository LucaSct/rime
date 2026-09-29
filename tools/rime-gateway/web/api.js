// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Thin fetch wrappers over `/api` (contract.md). One rule, enforced in one place: every non-2xx
// response's JSON `error` field is what a person sees (page-spec.md's "honest failure states"),
// never a bare status code or a console-only exception — so every caller in this page gets that
// behaviour by construction rather than by remembering to unwrap it each time.

const JSON_HEADERS = { "Content-Type": "application/json" };

/** Thrown by every wrapper below. `status` is 0 for a request that never reached the server (the
 * network itself failed) — real HTTP statuses start at 100, so 0 is an unambiguous "not the
 * server's answer" a caller can branch on. */
export class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.name = "ApiError";
    this.status = status;
  }
}

async function call(path, init) {
  let response;
  try {
    response = await fetch(path, { credentials: "same-origin", ...init });
  } catch {
    throw new ApiError(0, "could not reach the server");
  }
  if (response.ok) {
    return response;
  }
  let message = response.statusText || `request failed (${response.status})`;
  try {
    const body = await response.clone().json();
    if (body && typeof body.error === "string") {
      message = body.error;
    }
  } catch {
    // Not a JSON body (a raw 413 from the HTTP layer, a proxy's own error page, …) — the status
    // text is what there is.
  }
  throw new ApiError(response.status, message);
}

async function callJson(path, init) {
  const response = await call(path, init);
  if (response.status === 204) {
    return null;
  }
  return response.json();
}

function postJson(path, body) {
  return callJson(path, { method: "POST", headers: JSON_HEADERS, body: JSON.stringify(body) });
}

// ── The session API ───────────────────────────────────────────────────────────────────────────

export function getCatalogue() {
  return callJson("/api/catalogue");
}

export async function createSession(game, surface) {
  const response = await call("/api/sessions", {
    method: "POST",
    headers: JSON_HEADERS,
    body: JSON.stringify({ game, surface }),
  });
  return response.json();
}

export function getSession(id) {
  return callJson(`/api/sessions/${id}`);
}

export async function deleteSession(id) {
  // `keepalive` lets the request outlive the page: `leave()` runs from `pagehide`, and without it
  // the browser cancels the fetch as the document unloads, leaving the slot held until the
  // gateway notices the transport is gone.
  await call(`/api/sessions/${id}`, { method: "DELETE", keepalive: true });
}

export async function getIceServers(id) {
  const body = await callJson(`/api/sessions/${id}/ice`);
  return body.ice_servers;
}

/** `sdp` is the browser's local offer, gathered in full first (contract.md: non-trickle — one
 * round trip is the whole exchange). Returns the answer's SDP text. */
export async function postOffer(id, sdp) {
  const response = await call(`/api/sessions/${id}/offer`, {
    method: "POST",
    headers: { "Content-Type": "application/sdp" },
    body: sdp,
  });
  return response.text();
}

// ── The account ceremonies (/api/auth/...) ────────────────────────────────────────────────────
//
// auth_api.rs's module doc names the two request shapes this mirrors: a small command is a flat
// JSON body (`{"invitation":"…"}`); a signed WebAuthn response is passed through untouched as the
// ENTIRE body of a URL that already names which ceremony it answers — so the `*Passkey` functions
// below take an already-serialized JSON string (from webauthn.js) rather than building one.

export function registerBegin(invitation) {
  return postJson("/api/auth/register", { invitation });
}

export function registerCode(tx, code) {
  return postJson(`/api/auth/register/${tx}/code`, { code });
}

export function registerOptions(tx) {
  return postJson(`/api/auth/register/${tx}/options`, {});
}

export function registerPasskey(tx, credentialJson) {
  return callJson(`/api/auth/register/${tx}/passkey`, {
    method: "POST",
    headers: JSON_HEADERS,
    body: credentialJson,
  });
}

export function loginOptions(email) {
  return postJson("/api/auth/login/options", { email });
}

export function loginFinish(challenge, credentialJson) {
  return callJson(`/api/auth/login/${challenge}`, {
    method: "POST",
    headers: JSON_HEADERS,
    body: credentialJson,
  });
}

export function recoverBegin(email) {
  return postJson("/api/auth/recover", { email });
}

export function recoverProofs(tx, secret, code) {
  return postJson(`/api/auth/recover/${tx}/proofs`, { secret, code });
}

export function recoverOptions(tx) {
  return postJson(`/api/auth/recover/${tx}/options`, {});
}

export function recoverPasskey(tx, credentialJson) {
  return callJson(`/api/auth/recover/${tx}/passkey`, {
    method: "POST",
    headers: JSON_HEADERS,
    body: credentialJson,
  });
}

export async function logout() {
  await postJson("/api/auth/logout", {});
}

// ── Signing in with your phone (/api/auth/pair/..., ADR-0055; the views are pair.js) ──────────

export function pairBegin() {
  return postJson("/api/auth/pair", {});
}

export function pairDescribe(id) {
  return postJson(`/api/auth/pair/${id}/describe`, {});
}

export function pairOptions(id, email) {
  return postJson(`/api/auth/pair/${id}/options`, { email });
}

/** Same shape as `loginFinish`: the challenge in the path, the signed assertion as the whole body. */
export function pairApprove(id, challenge, credentialJson) {
  return callJson(`/api/auth/pair/${id}/approve/${challenge}`, {
    method: "POST",
    headers: JSON_HEADERS,
    body: credentialJson,
  });
}

export function pairStatus(id) {
  return postJson(`/api/auth/pair/${id}/status`, {});
}

export function pairRedeem(id, code) {
  return postJson(`/api/auth/pair/${id}/redeem`, { code });
}

/** `decision` is "accept" (mint the session) or "refuse" (kill the pairing). */
export function pairFinish(id, decision) {
  return postJson(`/api/auth/pair/${id}/finish`, { decision });
}
