# ADR-0055 — Signing a browser in with your phone

- **Status:** accepted
- **Date:** 2026-09-29
- **Decided by:** Luca (the flow, marked); `gpt-6-astra` consulted on the security requirements
- **Amends:** [ADR-0048](0048-authenticating-the-hosted-front-end.md) decision 1 ("the passkey is the
  only authenticator") by adding a second way for a *browser* to get a session — never a second
  way to authenticate an *account*.

## Context

ADR-0048 made a passkey assertion the whole of login. That assumes the browser in front of the user
can produce one. **Firefox on Linux cannot**: it has no platform authenticator and does not implement
the hybrid (QR-and-Bluetooth) transport, so a passkey that lives on a phone is unreachable from it.
The owner has no roaming security key. The account's passkey is on the phone, and the desktop — the
machine that actually runs the render session — has no way in.

## Decision: the phone approves; the desktop gets a session, not a credential (Luca, 2026-09-29)

The desktop shows a QR code → the phone scans it → the user signs in **on the phone** with the
passkey → the phone shows a code → the user types that code into the desktop → the desktop is told
which account approved it and confirms → the desktop receives one ordinary session.

**Why a session and not a credential.** Enrolling the desktop (registering a new passkey there) is
impossible — it has no authenticator; that is the whole problem. Minting some other long-lived
secret for it (a "device key") would be a second authenticator with none of a passkey's properties,
which is exactly what ADR-0048 decision 1 exists to refuse. A session is what a passkey login
produces anyway: bounded (7 days), revocable, and nothing that can authenticate anywhere else. The
**authenticator is still the passkey** — the assertion is made on the phone, user-verified, every
time. What this ADR adds is a way to deliver the resulting session to a different screen.

## Requirements (consult, decided)

The flow is a small state machine in `rime-auth` (`flow/pair.rs`) served under `/api/auth/pair` by
the gateway (`auth_api/pair.rs`):

- A pairing is `{ 128-bit random id, desktop binding, created, expires = created + 5 min (never
  extended), state }`, in memory only (a restart invalidates pairings), at most **64 live**; the 65th
  is refused with 429 rather than grown.
- The desktop's pairing is bound to its `__Host-` ceremony cookie. Status, redeem and finish answer
  only that binding.
- The QR encodes `https://<rp-origin>/#pair=<id>` — the id in the **fragment**, so it reaches no
  server log. The origin comes from configuration, never from request headers.
- Loading the phone page reserves or approves nothing. It shows the desktop's user-agent summary
  (from a fixed vocabulary) and age, labelled **unverified**, and a warning to continue only if the
  user started this on their own screen just now.
- The phone's passkey assertion is bound to the phone's own ceremony cookie **and** to the pairing,
  with a challenge TTL of min(2 min, the pairing's remaining life). A **fresh, user-verified
  assertion is required even if the phone already has a session**. The account is read off the
  verified credential. Only the first valid approval wins. The credential counter is written back
  as a login does. The phone is **not** given a session.
- Approval mints an eight-digit code through `codes.rs` (purpose `pair`, bound to the pairing id),
  keeps only its HMAC, and returns the digits to the phone only. The desktop gets 5 attempts in
  total; then the pairing is dead.
- A correct code moves the pairing to *confirming* and returns the account's **email** to the
  desktop, which must accept before one ordinary session is minted, the ceremony cookie cleared, and
  the account mailed ("A browser was signed in with your phone at …"). Refusing kills the pairing.
- Unknown, expired, finished, wrong-state and not-this-browser are **one refusal** (404, one body),
  the capability rule ADR-0048's gate applies to render sessions.
- Every pair route requires `Origin` to equal the configured origin, answers with
  `Cache-Control: no-store` and `Referrer-Policy: no-referrer`, goes through the per-IP limiter, and
  logs no id, code, binding or challenge. Codes compare in constant time.

## Threat model

**Stops:**

- **Photographing the QR or the code alone.** Neither carries the desktop binding, and every desktop
  step refuses without it — indistinguishably from an id that does not exist.
- **Brute force.** Five guesses per pairing at an eight-digit code: 5 / 10⁸, and a new pairing costs a
  new QR, a new phone approval and a new passkey assertion.
- **A stolen phone session cookie.** Approval ignores sessions entirely; only an assertion started for
  this pairing verifies against it.
- **Account substitution.** An attacker who approves your QR with *their* account would otherwise sign
  you in to it; the desktop is shown the approving email and must accept.
- **Replay.** Challenges are single-use and pairing-bound (a login challenge cannot approve a pairing,
  and a pairing challenge cannot finish a login); the code is consumed; a finished pairing is gone,
  so a second `finish` is refused.
- **Two desktops racing.** Each pairing has its own binding, its own challenge binding and its own
  code MAC; nothing from one verifies against the other.

**Does NOT stop: a real-time relay phish.** An attacker shows the victim a page that relays the
attacker's own pairing QR; the victim scans it, approves with their passkey, and types the code into
the attacker's page. The attacker's desktop is then signed in to the victim's account. **This flow is
not phishing-resistant** the way a passkey used directly is — the passkey's origin binding protects
the *phone's* assertion, but nothing binds the *code* to the screen the victim is looking at. The
phone screen's warning, and the announcement mail afterwards, are the only mitigations. That is the
price of letting a browser with no authenticator in at all, and it is accepted knowingly.

## Consequences

- ADR-0048's rule is unchanged in substance: mail still logs nobody in, and the passkey is still the
  only thing that authenticates an account. A browser may now *receive* a session another device's
  passkey earned.
- `qrcode` (MIT OR Apache-2.0, `svg` feature only) joins the gateway behind the `auth` feature, so
  exported game bundles never compile it (ADR-0046 §2).
- **Deferred:** per-account session listing and revocation — the store can revoke a session only by
  its token today (`rime-auth/src/lib.rs`, `revoke_session`), so a user who discovers an unwanted
  phone-approved sign-in cannot yet end that one session from another device (recovery, which
  revokes everything, is the blunt tool). Also deferred: tagging sessions as phone-authorized, so
  that listing can say which ones were.
