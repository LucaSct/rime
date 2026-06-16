# ADR-0003: Apache-2.0 license

- Status: Accepted
- Date: 2026-06-16

## Context

Rime is open source and intends to become a community, while letting studios ship
*commercial, proprietary* games built on it with no friction (VISION principles #1, #5).
A game engine also sits squarely in patent-heavy territory (graphics, physics,
compression), so patent posture matters.

License families considered: permissive (MIT, Apache-2.0, BSD), weak copyleft (MPL-2.0),
strong copyleft (GPL/LGPL/AGPL).

## Decision

**License the engine under Apache License 2.0.** Each source file carries an SPDX header
(`SPDX-License-Identifier: Apache-2.0`). The full text is in [`/LICENSE`](../../LICENSE);
attributions live in [`/NOTICE`](../../NOTICE).

## Consequences

**Good**
- **Permissive:** anyone can use, modify, and ship commercial games on Rime — no
  royalties, no copyleft obligation on the games themselves.
- **Explicit patent grant + retaliation clause:** contributors grant patent rights, and
  the license discourages patent aggression — important for graphics/physics tech. This
  is the main reason we prefer it over MIT.
- **Industry-trusted and compatible:** widely understood by legal teams; the same
  family O3DE uses, easing shared ecosystem expectations.

**Costs we accept**
- Slightly more boilerplate than MIT (the SPDX header + maintaining `NOTICE`).
- Incoming dependencies must be license-compatible; we track third-party licenses in
  `third_party/` and avoid copyleft deps that would compromise the permissive promise.

## Alternatives considered

- **MIT.** Simplest and most familiar, but **no explicit patent grant** — a real gap for
  an engine. Apache-2.0 gives the same permissive freedom *plus* patent protection.
- **Dual MIT/Apache-2.0** (Rust-ecosystem norm). Maximum compatibility, but more
  boilerplate and dual-license bookkeeping; Apache-2.0 alone covers our needs. May
  revisit for Rust crates we publish separately.
- **MPL-2.0 / GPL family.** File-level or strong copyleft would deter commercial studios
  from shipping closed games on Rime — directly against the vision. Rejected.
