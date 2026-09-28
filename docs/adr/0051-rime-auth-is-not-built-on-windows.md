# ADR-0051 — `rime-auth` is not built on Windows

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca
- **Relates to:** [ADR-0048](0048-authenticating-the-hosted-front-end.md) decisions 2 and 3,
  [ADR-0047](0047-two-machines-and-the-starbase-tier.md) §3 (hosting is starbase, which is Linux),
  [ADR-0046](0046-exported-games-and-the-blender-boundary.md) §2 (an exported game never enables `auth`)

## Context

ADR-0048 decision 2 ratified `webauthn-rs` for passkey verification and put it behind a Cargo feature
so an exported game does not inherit its MPL-2.0 obligation. What that decision did not know is what
the dependency drags in.

**Measured 2026-09-28**, on the first CI run of the crate that uses it (PR #210, Windows leg red):

- `webauthn-rs` → `webauthn-rs-core` → **`openssl` and `openssl-sys`, declared unconditionally** in
  `webauthn-rs-core`'s manifest. They are not behind a feature, so no feature selection avoids them —
  verified by reading the manifest, not inferred from the error.
- The `windows-latest` runner has no OpenSSL for `openssl-sys` to find, so `cargo build` fails in that
  crate's build script, **before any of its own code is type-checked**. Reproduced locally by
  cross-checking with `--target x86_64-pc-windows-gnu`, which fails in the same build script.
- The same chain is what made the *test* authenticator (`webauthn-authenticator-rs`, `softpasskey` →
  `crypto`) unbuildable there; that one is now a `[target.'cfg(unix)'.dev-dependencies]` entry, but it
  was never the cause.

So the repository's "builds on three platforms" guardrail and ADR-0048's choice of library are in
direct conflict, and one of them has to give.

## Decision

**The Windows CI leg builds the workspace with `--exclude rime-auth`.** `scripts/build.ps1` passes
`--workspace --exclude rime-auth` to both `cargo build` and `cargo test`, with the reason in a comment
beside it. Nothing else depends on the crate unless `rime-gateway`'s `auth` feature is on, and that
feature is off by default, so the rest of the workspace is unaffected — confirmed by asking Cargo:
`cargo tree -p rime-gateway --target x86_64-pc-windows-gnu -i openssl-sys` matches no package.

## What this costs, stated plainly

- **A Windows-only regression in `rime-auth` would reach `main` unseen.** Nothing compiles it there.
  The exposure is bounded by the crate already being Unix-shaped: `fill_random` refuses to invent
  entropy off Linux, so every ceremony test is `#[cfg(unix)]` and would be skipped on Windows even if
  it did build.
- **A Windows contributor running `cargo build` in `tools/` still hits the failure**, because that is
  the unexcluded command. The script is the supported path and it excludes the crate; a bare `cargo
  build` is not covered.
- It narrows a guardrail rather than fixing the thing that makes it fail, which is why it is written
  down here rather than left in a manifest comment.

## Why not the alternatives

- **Vendor OpenSSL** (`openssl` with `features = ["vendored"]`, pulled in as a direct dependency so
  Cargo's feature unification reaches `webauthn-rs`'s copy). This works — it was implemented and the
  40 tests passed against it before being reverted — and it keeps all three platforms building the
  crate. It costs a full OpenSSL C build on every CI platform and every clean checkout, plus a second
  licence in the tree. That is precisely the cost ADR-0048 decision 3 declined to pay for bundled
  SQLite, and paying it here to cover a platform that does not host would be inconsistent.
  **This is the recorded fallback**: if a Windows regression in `rime-auth` ever matters, vendoring is
  the change to make, and it is one line.
- **Install OpenSSL on the runner** (`choco install openssl`, or point `OPENSSL_DIR` at a preinstalled
  copy). Cheapest to write and the most fragile: it makes the build depend on the contents of a runner
  image that drifts, and the failure mode is a red CI on a PR that changed nothing — the repository
  already has one standing instance of that shape in the floating `rust-toolchain`.
- **Replace `webauthn-rs`.** Reopens a ratified decision that `gpt-6-astra` was consulted on, for a
  security boundary where the mature implementation is the one we picked.

## Consequences

- `docs/ROADMAP.md`'s platform claim is unchanged for the engine. It is the **hosted front end** that
  is Linux-only, which ADR-0047 §3 already said about where hosting runs; this makes it true of the
  build as well as of the deployment.
- If the hosted gateway is ever wanted on Windows, this ADR is what to supersede, and vendoring is the
  known-working route.
