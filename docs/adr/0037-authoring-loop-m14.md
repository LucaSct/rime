# ADR-0037: M14 — "The Authoring Loop"

- Status: Proposed
- Date: 2026-08-30

## Context

M13 built content. `rime-blockgen` writes a 213-entity city block as a `.rscene`, `99-the-block`
loads it, and the engine draws it, breaks it, replicates it and sounds it. **The editor cannot open
it.**

```
[ERROR] viewport scene load failed: unknown component type
        'rime::blockkit::SlabRole' (hash 0x3f51d745b75a387c) — is it registered?
```

That single line is the whole context for this milestone, and it is worth being precise about what
it does and does not mean.

**The editor is not missing — it works.** `tools/editor` (2.9k lines of Rust) + `tools/rime-protocol`
(1.7k) + `engine/editorhost` implement the engine-as-server architecture ruled in July: on the
default scene the smoke run reports 7 entities, 12 frames decoded to 960×540 RGBA, a pick at (275,53)
hitting entity 0:0, the viewport-camera lens verified (`vp·inv = I`, err 1.5e-5), a gizmo translate
applied, and a clean shutdown. Engine-as-server, a streamed viewport, working picking, working
gizmos. **That is the hard part, and it is done.**

What is missing is narrower and more embarrassing: the editor can edit a scene the engine cannot
ship, and cannot edit the scene the engine does ship. There is no round trip.

**Three specific gaps.**

1. **The host registers a scene profile by hand, and it is the wrong one.**
   `engine/app/editor_host_main.cpp` registers transform + render + physics components. The block's
   scene carries `blockkit::SlabRole` and `destruction::Destructible`, so it fails to load.
2. **The GUI shell is not built by CI.** `tools/editor/Cargo.toml` puts the egui/eframe shell behind
   `--features gui`, off by default so the CI-critical `editor --smoke` path stays a light build.
   Nothing in the matrix builds it, so 1,215 lines of `gui.rs` and 1,043 of `gizmo.rs` can rot
   silently.
3. **The editor smoke uses a synthetic scene.** It passes on all seven PRs of the M13 stack while the
   newest content in the repo is unopenable. A green gate that cannot see the content is the
   [assert-the-handoff-not-the-value] pattern again, one layer up.

**The root cause is bigger than the editor, and M13 proved it independently.** There is no single
answer in this codebase to *"what components does a Rime world have?"*. Every consumer hand-assembles
a list, and the lists have diverged. `99-the-block`'s first draft registered the four modules the
block's scene needs and none of the four its session needs; the block stood up and drew perfectly
while the predictor never seeded, destruction replicated zero ops, the peers disagreed and the mixer
heard nothing — four symptoms, one missing list, and not one of them visible in any subsystem's own
tests. The editor's failure and that failure are the same defect wearing different clothes.

## Decision

**M14 is "The Authoring Loop", and its "done when" is a round trip:**

> open the shipped block in the editor, change it, save it, and run the changed scene in the game.

That is deliberately not "the editor gets better". It is the smallest claim that forces every gap
above to close, and it is the difference between an engine you demo and an engine you can make
something with.

### 1. The component profile is an explicit composition, owned by the top layer

A **profile** is a named function that registers a component set on a world. Not a global registry:
that would need module self-registration through a mutable singleton, which violates guardrail 4 and
would quietly break guardrail 2's promise that the engine still builds when a feature module is
removed.

Instead the profile lives in whichever module already depends on everything it names — the
composition point, at the top of the cake — and every consumer of a `.rscene` calls the same one.
Modules stay independent and un-self-aware; there is exactly one list, and it is a normal function
that a linker error will find if a module is dropped.

Consequences, and they are the point:

- the editor host, `99-the-block`, and any future tool register the same set by construction;
- a scene that loads in the game loads in the editor, because "loads" now means the same thing;
- the list is one screen, in one file, and a diff to it is reviewable.

### 2. Unknown components degrade, they do not fail the load

`editor_host_main.cpp:349` already documents the intent — "a component the host has not registered
still loads and is fully editable; those entities simply do not draw". The viewport scene load does
not honour it. **An unknown `type_hash` must cost that one component, not the scene**, and it must be
counted: a load that silently dropped 140 components is a different fact from a clean one, and only a
counter can tell them apart. Same rule the repo applies everywhere else — give every skip path a
number.

This is defence in depth, not an alternative to §1. The profile makes the common case correct; the
degradation makes the uncommon case survivable rather than fatal.

### 3. The editor's proof loads real content

`editor --smoke` moves from its synthetic scene to **the block**. It would have caught `SlabRole` on
the day m13.2c landed, and it is the gate that keeps §1 honest: a profile that drifts from the
content is exactly what this test exists to fail on.

### 4. The GUI shell is built by CI

One job builds `--features gui`. Not run — a windowing stack in CI is its own problem — but
**compiled**, so a shell that no longer builds is a red job rather than a discovery months later. The
cheapest possible insurance against the thing that has already half-happened.

## Bricks

Planned to the same shape as ADR-0035's: each one ends in a proof that runs.

- **m14.0** — this ADR + the ladder. The decision brick; no engine code.
- **m14.1** — **the profile**, and the editor opens the block. One registration function, every
  consumer switched to it, unknown components degrading-with-a-counter, and `editor --smoke` loading
  the real `block.rscene`. *Proof: the smoke opens 213 entities, reports 0 unregistered components,
  and picks one.* This is the brick that fixes what was actually hit.
- **m14.2** — **the shell in CI** (`--features gui` compiled) + the shell able to open a scene file
  from the command line rather than only the built-in default. *Proof: a CI job builds it; the smoke
  opens a named file.*
- **m14.3** — **save**. The editor writes a `.rscene` back through the protocol. *Proof: load → save
  round-trips byte-identically, and load → edit → save → load reproduces the edit — the m13.2c
  round-trip discipline, from the other end.*
- **m14.4** — **the round trip**, M14's "done when": edit the block in the editor, save it, and
  `the_block` runs the edited scene. *Proof: a scripted edit (move a building, retint a storey) that
  the demo's own counters see.*
- **m14.5** — the asset browser against real cooked assets, if the ladder still has room.

**Cut order if it runs long:** m14.5 → m14.2's shell-opens-a-file half. **Never cut:** m14.1 and
m14.3 — without either there is no round trip and the milestone has no "done when".

## Sequencing against M13

**M13 closes first.** `99-the-block` reports one failing claim that this ADR does not touch: *the
collapse does not stay local* — a 5 m charge on one building flattens the whole block, across a 12 m
gap and then across the street. That is a destruction defect, it blocks M13's "feels right" clause,
and it is the next brick regardless of what follows it. m13.p (the measured perf pass) follows,
because its scope should come from a demo that exists.

M14 starts after. The one thing worth doing *now* is nothing — the temptation to start the editor
work while M13 has an open defect is exactly how an integration bug gets expensive.

## Alternatives considered

**A global component registry with module self-registration.** Rejected on guardrails: it needs
mutable global state (guardrail 4) and it makes "the engine still builds without this module" a
runtime question instead of a link-time one (guardrail 2). The explicit profile costs one line per
module in one file and keeps both promises.

**Make the editor tolerant and stop there** — i.e. §2 without §1. Rejected: it turns a hard failure
into a silent one. The editor would open the block, drop every `SlabRole`, and save a file that has
quietly lost the thing that decides what the block looks like. Tolerance without a correct profile is
worse than the error message.

**Skip the editor; keep generating content from code.** Genuinely arguable — `rime-blockgen` works,
and C++/Rust generators have taken the project this far. Rejected because it does not scale past
content whose *shape* is known in advance, and because "can a human author content for this engine"
is a question VISION answers yes to. Worth revisiting only if m14.1 and m14.3 turn out much harder
than they look.

**Rebuild the editor shell on something other than egui.** Not considered seriously at this point:
the shell works, the architecture behind it is proven, and re-litigating the UI toolkit before the
round trip exists would be optimising the part that is not blocking anything.
