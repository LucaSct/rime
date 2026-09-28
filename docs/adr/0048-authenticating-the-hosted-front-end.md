# ADR-0048 — Authenticating the hosted front end

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca (owner decisions marked); `gpt-6-astra` consulted on the design
- **Supersedes:** nothing. **Amends** [ADR-0045](0045-hosted-front-end-v1.md)'s last consequence
  ("v1 is LAN and un-authenticated") by naming what has to exist before that stops being true.

## Context

[ADR-0045](0045-hosted-front-end-v1.md) shipped the hosted front end deliberately
un-authenticated, on the reasoning that v1 is LAN-only and nothing is published. Track H then built
the surface policy, the session supervisor, admission control and a bounded HTTP session API. The
result is a service where **an HTTP POST spawns a process**, and the only thing standing between that
and the internet is the absence of a DNS record and a firewall rule.

The owner's instruction (2026-09-28) is that **publication is deferred until authentication exists**,
and named three elements: an auth code, a passkey, and mail. This ADR composes them.

It also inherits a constraint that is easy to forget:
[ADR-0046](0046-exported-games-and-the-blender-boundary.md) §2 requires **the same gateway binary to
ship inside every exported standalone game**. A stranger running a Rime game at home has no
`rime.peekstar.eu`, no Resend key, and no relying-party domain — and a single-player game must not
ask anybody to log in.

## Decision 1: invite-gated registration, passkey login, mail-assisted recovery (Luca)

The three elements compose as follows, and each has exactly one job:

| element | job | **not** its job |
|---|---|---|
| **auth code** | an invitation authorising *registration* — 128 random bits, bound to one email, single-use, 7-day expiry | logging in |
| **passkey** | authenticating an existing account, every time, with user verification | proving who someone is in the world |
| **mail** | delivering the invitation and a short verification code; announcing credential changes | logging in **on its own** |

**Mail can never replace a passkey.** A code that can log you in makes the passkey decorative and
reduces the account's security to the mailbox. Recovery with every passkey lost requires **both** a
recovery secret shown once at registration **and** a fresh mailed code, and it may only register a
replacement passkey — completing it revokes the old credentials, all sessions and the recovery secret.

Ceremonies: first contact → invitation + mailed code → passkey registration → passkey login
thereafter; a new device is enrolled from an existing authenticated one; a lost device is revoked from
another passkey, or via the two-proof recovery above.

**Invite-gated was the owner's choice** over open registration. Three render slots is a demo rather
than a service, and the gate is a hard cap on who can spawn a process on the hosting GPU while
containment is still immature.

## Decision 2: `rime-auth`, behind a Cargo feature that exported games do not enable (Luca)

Authentication lives in a new crate, `tools/rime-auth`, and `rime-gateway` depends on it
**optionally**. Hosted builds enable the `auth` feature; exported game bundles do not.

This is a licensing decision as much as an architectural one. Passkey verification means CBOR/COSE
parsing and ES256 signature checking, which is not a thing to hand-roll for a security boundary, and
the mature Rust implementation — `webauthn-rs` — is **MPL-2.0** (verified in its workspace manifest,
not assumed). MPL is *not* GPL: it is file-level copyleft, so linking it leaves Rime's own code
Apache-2.0 and only requires that its own source stay available. But ADR-0046 §2 would otherwise put
that obligation into **every game a Rime user ships**, and [ADR-0017](0017-streaming-codec.md) refused
GPL x264 on exactly the reasoning that an exported game must not inherit somebody else's terms.

The feature flag resolves both at once: the obligation lands on the hosted deployment, which we
operate and can carry notices for, and never on a user's exported game — which needs no accounts
anyway.

**Consequence:** two build configurations must stay green in CI. An untested configuration is the one
that will be broken the day it matters.

## Decision 3: an append-only store, not SQLite (engineering call)

The consult proposed SQLite via `rusqlite` with `bundled`. **We are not taking that**, and the
disagreement is worth recording because the reasoning generalises.

What the store must actually do: hold a handful of accounts, their credentials, their invitations and
their sessions; redeem an invitation exactly once; revoke things; survive power loss without
corruption. What it does **not** need: a query planner, joins, concurrent writers, or a schema
migration system — there is one process, one writer, and the largest table will have tens of rows.

An **append-only record log with an fsync on commit and a full in-memory index** provides all of the
required properties in a few hundred lines, and crash-safety comes out by construction: a torn final
record is discarded on replay, because a record is only valid once its length and checksum are both
readable. `bundled` SQLite would instead add a C build to three CI platforms, a second licence to
audit, and ~1 MB, in exchange for capabilities this data model does not use.

This is the same reasoning that made `rime-protocol` dependency-free and made the gateway's HTTP and
JSON hand-rolled — and, symmetrically, the same reasoning that made `str0m` the right answer for
WebRTC ([ADR-0049](0049-webrtc-transport.md)): hand-roll what is small and well-understood, take a
dependency for what is large and cryptographic.

**The escape hatch is named so it is not a surprise:** if the data model grows relations that want
joining, or a second writer appears, SQLite behind the same `rime-auth` API is the migration, and the
store's interface is designed so that swap touches one module.

Hashing and HMAC use RustCrypto (`sha2`, `hmac`, both MIT OR Apache-2.0) rather than the consult's
suggested vendored OpenSSL, for consistency with `str0m`'s `rust-crypto` backend — having just
refused vendored C in the transport, accepting it here would be incoherent.

## Decision 4: identity and the render session are different things (engineering call)

`SessionId` is already a capability — 128 random bits, "an id is a capability". It is a **render
session** id and it is **not** a user session. Conflating them is the mistake this decision exists to
prevent.

- Every admission reservation gains an immutable `owner_account_id`.
- Hosted access requires **both** an authenticated owner **and** the render id. A leaked session URL
  alone grants nothing.
- Authorization happens **before** reservation and spawn, not after.
- Ownership is checked on lookup, deletion, signalling **and every transport attachment** — the
  consult's stated "most likely mistake" is protecting creation and leaving signalling reachable with
  only a render id, and a test must show a second account failing on every route.
- Listings are filtered to the caller's own sessions.
- Host caps stay 3 total / 1 play, **plus** 1 render session per account, checked atomically.
- Mutations require an exact `Origin` match and a CSRF token.

## Decision 5: what authentication means in an exported game (engineering call)

The exported binary selects a runtime policy explicitly, per ADR-0046 §1's launch modes:

- **local** (play on this machine, or a browser on loopback): **no authentication at all**. A
  single-player game must never ask anyone to log in. The launcher issues a loopback-only access token
  so a stray process on the same box cannot drive the session.
- **self-hosted / dedicated**: the operator issues instance-local tokens with separate player and
  admin scopes. Passkeys are *optional* and, if used, are bound to that instance's own origin. No
  hosted credential and no Resend key is required or accepted.
- **hosted**: this ADR.

A passkey is bound to an origin, so hosted credentials cannot work on a stranger's instance and must
not be made to.

## Consequences

- ADR-0045's "v1 is LAN and un-authenticated" stands **until this is built**, and publication waits.
  The `check_bind` refusal of wildcard addresses stays regardless; authentication is not containment.
- **Authentication is not a substitute for sandboxing.** `docs/design/hosted-rime.md`'s containment
  plan is still required before an untrusted user, and this ADR does not discharge it.
- Three things are refused outright: public hosted access with an anonymous or fallback path;
  mail-only recovery that can replace a passkey; and treating authentication as sufficient containment.
- The relying-party id and origin are **fixed by configuration**, never derived from request headers.
  Development uses `localhost` with its own store, and credentials do not transfer between the two.
- `tools/rime-gateway/src/http.rs`'s claim that "TLS and the public hostname live in blackStar" is
  **wrong** and is corrected by this brick: blackStar peeks SNI and passes PROXY v2 through without
  decrypting, so each backend terminates its own TLS. Rime needs a TLS terminator in its own container.

## Bricks

1. **the store** — `rime-auth` behind the feature: records, the append-only log, invitation issue and
   single-use redemption, session tokens. *(landed)*
2. **the ceremonies** — `webauthn-rs` registration and login, mailed codes through the Resend relay,
   recovery. *(this brick)* — `ceremony.rs` (both WebAuthn ceremonies, JSON in and out so no library
   type reaches the gateway), `codes.rs` (HMAC'd eight-digit codes), `mail.rs` (plain SMTP to the
   estate's local relay, which already holds the Resend key — so this crate holds none), and `flow.rs`,
   which is where the ordering rule of decision 1 is actually enforced.
3. **the gate** — `owner_account_id` through admission, authorization before spawn, per-account caps,
   ownership on every route including signalling, and the second-account test.
