// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Conversions between the browser's WebAuthn API (which speaks `ArrayBuffer`) and the JSON the
// gateway's `webauthn-rs` sends and expects (which speaks base64url strings, via the
// `base64urlsafedata` crate — confirmed by reading its source: `URL_SAFE_NO_PAD`, i.e. base64url
// with no `=` padding). Nothing here talks to the network; `session.js`/`app.js` call these
// around `navigator.credentials.create`/`get`.
//
// Why base64url and not base64: the challenge and credential ids ride inside JSON strings and
// inside URLs (a session id is already a capability carried in a path), and base64url's alphabet
// has no `+`, `/` or `=` to percent-encode. The gateway's Rust types (`webauthn-rs-proto`) fix
// this shape; this module exists to speak it, not to choose it.

function base64urlToBytes(b64url) {
  const b64 = b64url.replace(/-/g, "+").replace(/_/g, "/");
  const padded = b64 + "===".slice((b64.length + 3) % 4);
  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function bytesToBase64url(bytes) {
  const view = bytes instanceof ArrayBuffer ? new Uint8Array(bytes) : bytes;
  let binary = "";
  for (let i = 0; i < view.length; i++) {
    binary += String.fromCharCode(view[i]);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

/**
 * Turn the gateway's `PublicKeyCredentialCreationOptions` (challenge/user.id/excludeCredentials
 * ids as base64url strings — `webauthn-rs-proto::attest::PublicKeyCredentialCreationOptions`)
 * into what `navigator.credentials.create({publicKey})` requires (those same fields as
 * `ArrayBuffer`). Everything else in the object (rp, pubKeyCredParams, authenticatorSelection, …)
 * is already JSON-shaped exactly like the Web Authentication API wants it — `camelCase`, because
 * `webauthn-rs-proto` serializes with `#[serde(rename_all = "camelCase")]`.
 */
export function decodeCreationOptions(publicKey) {
  const options = { ...publicKey };
  options.challenge = base64urlToBytes(publicKey.challenge);
  options.user = { ...publicKey.user, id: base64urlToBytes(publicKey.user.id) };
  if (publicKey.excludeCredentials) {
    options.excludeCredentials = publicKey.excludeCredentials.map((cred) => ({
      ...cred,
      id: base64urlToBytes(cred.id),
    }));
  }
  return options;
}

/**
 * The login twin of [`decodeCreationOptions`]: `PublicKeyCredentialRequestOptions`'s
 * challenge/allowCredentials ids, base64url to `ArrayBuffer`.
 */
export function decodeRequestOptions(publicKey) {
  const options = { ...publicKey };
  options.challenge = base64urlToBytes(publicKey.challenge);
  if (publicKey.allowCredentials) {
    options.allowCredentials = publicKey.allowCredentials.map((cred) => ({
      ...cred,
      id: base64urlToBytes(cred.id),
    }));
  }
  return options;
}

/**
 * The `PublicKeyCredential` from `navigator.credentials.create()`, as the JSON body
 * `POST /api/auth/register/<tx>/passkey` (and the recovery equivalent) expect — matching
 * `webauthn-rs-proto::attest::RegisterPublicKeyCredential` field-for-field: `id` is the
 * credential's own base64url `id` (the browser already produces this per the WebAuthn spec, so
 * it passes through unchanged), `rawId`/`response.attestationObject`/`response.clientDataJSON`
 * are re-encoded from the `ArrayBuffer`s the browser hands back, and `extensions` is omitted —
 * the Rust field is `#[serde(default)]`, so an absent key decodes as the all-`false` default
 * rather than requiring this page to mirror every extension the server might one day ask for.
 */
export function encodeAttestationResponse(credential) {
  const response = {
    attestationObject: bytesToBase64url(credential.response.attestationObject),
    clientDataJSON: bytesToBase64url(credential.response.clientDataJSON),
  };
  return JSON.stringify({
    id: credential.id,
    rawId: bytesToBase64url(credential.rawId),
    response,
    type: credential.type,
  });
}

/**
 * The `PublicKeyCredential` from `navigator.credentials.get()`, as the body
 * `POST /api/auth/login/<challenge>` (and recovery's passkey step) expect — matching
 * `webauthn-rs-proto::auth::PublicKeyCredential`/`AuthenticatorAssertionResponseRaw`. `userHandle`
 * is `Option<Base64UrlSafeData>` on the Rust side, so `null` when the authenticator did not
 * return one is a value the type accepts, not an omission to paper over.
 */
export function encodeAssertionResponse(credential) {
  const response = {
    authenticatorData: bytesToBase64url(credential.response.authenticatorData),
    clientDataJSON: bytesToBase64url(credential.response.clientDataJSON),
    signature: bytesToBase64url(credential.response.signature),
    userHandle: credential.response.userHandle ? bytesToBase64url(credential.response.userHandle) : null,
  };
  return JSON.stringify({
    id: credential.id,
    rawId: bytesToBase64url(credential.rawId),
    response,
    type: credential.type,
  });
}
