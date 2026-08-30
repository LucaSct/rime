# `99-the-block` — Milestone 13's "done when"

> a destructible urban block (M8 + M10 + M11 + M12) runs at a playable frame rate and *feels* right

The vision demo. Every piece of that sentence had been built and proved on its own; this is the
first process that runs them together — a cooked destructible city block, the M10 lighting stack, a
replicated server, a predicted client, a first-person camera, a window and a mixer, in one frame.

```bash
build/dev/bin/the_block --headless          # the CI-gated proof
build/dev/bin/the_block --play              # walk around it and shoot
build/dev/bin/the_block --headless --idle   # the control: nobody touches the block
build/dev/bin/the_block --headless --ticks 1100   # run the script past where CI stops caring
```

`--cooked <dir>` points at the nine `.rdest` files; the build cooks them into the target's own
directory and bakes that path in, so the two commands above just work after a build.

## Status: 23 claims pass — M13's "done when" is met

It did not start that way. m13.5 shipped this demo with 20 claims passing and one **reported as a
known defect**: *the collapse does not stay local*. A charge on one building flattened the whole
block, across the 12 m gap to its neighbours and then across the street. `--idle` — the same block
with a player who never moves or shoots — reproduced it, so it was not the player, the rifle, or the
charge's size.

**The cause was damage tuning, not physics.** `damage_threshold` is cooked per pattern and the
fracturer's 5.0 default was set for M8's small test wall. A building slab part is two orders of
magnitude heavier: measured here, contact impulses reached **669 kg·m/s** against a threshold of 5
and a part health of 1.0, so **729 of 799 contact ops were instant kills** and the cascade could not
damp. m13.6 put `--damage-threshold` / `--damage-scale` on `rime fracture` — they had never been
reachable from the CLI at all — and gave each cook tuning matched to its part mass.

Now the same charge levels the hero building, seriously damages the two buildings beside it, and
leaves the far side of the street untouched. The `locality` line prints the per-building survivors
and `damage` prints the op population that made the defect legible.

## What the gate covers

| | |
|---|---|
| scale | 2,016 parts (floor 1,500) · 36 local lights (floor 32) |
| player | the avatar replicates, and the **predictor seeds** — the camera rides the predicted pose, the wiring m13.3a named and deferred to here |
| destruction | shots reach the block · parts fall · peak live debris ≥ 400 · the visual budget stays above the live one |
| networking | ops replicate · every composition check matches · nothing unresolved · both peers agree on `shared_state_hash`, **within a bounded settle** |
| audio | destruction drives the mixer · nothing clips |
| render | the cull considers every part and rejects **1,983 of 2,051** · every leaf finds its mesh · the frame comes back lit |
| collapse | the far side of the street is untouched · most of the block is still standing |

"A playable frame rate" is the one clause CI cannot judge — lavapipe is a CPU rasteriser, so a
millisecond there is a statement about the runner's mood. Counts stay in `--headless` where CI can
fail on them forever; the clock belongs in a `--perf` mode (m13.p), on hardware whose fingerprint
goes into the report. That is the 11-lit-rooms split, ADR-0035 §2b.

## Five things that only broke when composed

Each was invisible in isolation, and each is commented at its site:

1. **Register every module's components, not just the scene's.** The first draft registered the four
   the block's `.rscene` needs and none of the four the session needs. The block stood up and drew
   perfectly; the predictor never seeded (no `RigidBodyHandle` meant no body to predict against),
   destruction replicated zero ops, the peers disagreed, and the mixer heard nothing. Four symptoms,
   one cause.
2. **Never make a structural change inside `world.query()`.** `ServerReplicator::replicate` moves the
   entity to another archetype and reallocates the chunk vector the query is walking; the loop aborts
   on the next chunk index. Collect, then act.
3. **A server replicates its destructibles; a client must drop its own.** Damage ops are addressed by
   NetId, so the entities need one. If the client also loads them from the scene it ends up with 280
   instances — 140 taking damage and 140 standing there forever, which draws as a building that never
   falls behind the one that does.
4. **`apply_palette` runs when destructibles arrive, not once at load.** It derives the look from the
   reflected `SlabRole`; a client's destructibles arrive tick by tick and miss a single call at load.
   2,016 leaves existed and followed the sim perfectly while the renderer considered 35 entities — a
   leaf with no material is not a draw.
5. **The cross-peer witness is `destruction_net::shared_state_hash`.** Not
   `DestructionWorld::state_hash()`, which folds physics body ids and local instance indices: two
   peers in perfect agreement will still disagree on it.

## Two claims that were nearly true by accident

The m13.2d lesson — a proof pinned to an accident — in two new places.

**The render claims are made on a frame drawn while the block is standing**, chosen by the condition
*"the client holds every part"* rather than by a tick number. At tick 0 the leaves do not exist yet;
a few ticks later replication has still delivered only part of the block over a lossy 80 ms link. A
fixed tick measured 35 props and passed "the cull did work" while drawing no block at all.

**The tape is tight because the tail is expensive.** Every tick after the collapse simulates ~1,500
debris bodies, so 1,100 ticks took 226 s in Debug on a real GPU — under ASan on lavapipe that is a
timeout, not a test. It is 430 ticks at 37 s now, with every claim still reachable; `--ticks N` runs
it longer by hand.

## Shape

One `Session` owns a `ScriptedNetwork`, a server peer and a client peer, at 80 ms RTT and 5% loss —
in-process and deterministic, because the claim is about composition at scale and a real socket would
add a nondeterministic variable that says nothing about it. The server owns the block; the client
loads the level, is sent the destructibles, predicts its own player and draws. **What is on screen is
what a client holds**, which is the only honest thing to show in a networked demo.

One `Demo` serves all three modes. What differs is where the input comes from and who owns the loop —
never what is standing there.

Deliberately **not** re-proved here: prediction against a prediction-off control
(`13-networked-player`), two clients agreeing on a broken wall (`12-networked-destruction`), and GI
mechanism (`11-lit-rooms`, `tests/render/gi_thesis_test.cpp`).
