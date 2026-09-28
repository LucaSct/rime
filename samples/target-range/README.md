# target-range — Milestone 15's "done when"

> a small game that is **not the block** is authored through the editor and runs on the engine, with
> **no engine or editor source changed** to support it

Five crates stand downrange. A shooter fires, they fall, the score counts up. That is the entire
game, and it is deliberately less interesting than what it proves.

```bash
build/dev/bin/target_range --scene samples/target-range/target_range.rscene --verbose
build/dev/bin/target_range --scene samples/target-range/target_range.rscene --headless
build/dev/bin/target_range --emit-scene samples/target-range/target_range.rscene   # regenerate
```

## What is actually being proved

The load-bearing words are **no engine or editor source changed**. Every other sample here is a
demonstration the engine was built to support. This one is a game the engine did not know about, and
the proof is partly in what this milestone's diff does *not* contain — it touches `samples/` and
`docs/` and nothing else.

**The game's own component is the proof.** `targetrange::Target` is declared here, reflected here,
registered here, and written into `target_range.rscene` by name and by type hash. The engine's
serializer round-trips it without knowing what a target is, because
`worldkit::register_engine_components` names the engine's components and deliberately stops —
"a game calls this and then registers its own on top"
(`engine/worldkit/include/rime/worldkit/profile.hpp:45-48`).

The load is **strict**: no `allow_unknown_components`. Delete the registration and the load fails
outright —

```
FAILED to load target_range.rscene: unknown component type 'targetrange::Target'
(hash 0x65a0bf25e44efc48) — is it registered?
```

— which is the falsification that makes the claim mean something. A lenient load would have skipped
the game's own records and brought up an empty range that passed nothing.

**The targets come from the file, not from the code.** Nothing in `main.cpp` says "five". The win
condition is "every `Target` in the loaded world is down", so moving, adding or deleting a crate in
the editor changes the game with no rebuild. That is the difference between a game that *reads* a
scene and one that regenerates its own level and ignores the file it was handed — the distinction
M14's round trip exists to catch, and the reason this sample prints a authored digest.

**A shot is a real raycast.** `PhysicsWorld::raycast` into the real world, and the answer is believed
even when it is inconvenient. Shots can miss, and both miss paths are proven reachable rather than
asserted:

| edit to the scene | result |
| --- | --- |
| move one crate to `z -200`, past the 60 m shot range | `4 of 5 down, 334 shots, 330 world misses` |
| park a knocked-over crate in the line of fire | `1 of 4 down, 333 blocked` |

Both of those are edits to the **scene file**, which is the other thing they demonstrate: the game's
behaviour changed with no rebuild.

A hit detector that cannot miss proves nothing, which is why those two runs were made before the
green one was trusted.

## The three tests

| test | proves |
| --- | --- |
| `target_range_selftest` | GPU-free, so it gates on all three OSes and both sanitizers: the scene loads strictly, it declared targets at all, every target went down to a real raycast, and the whole run — score, hits, both miss counters and the authored digest — is identical on a different worker-thread count (ADR-0026) |
| `target_range_headless` | the same authored scene renders off-screen; skips honestly with no Vulkan device unless `RIME_REQUIRE_VULKAN` is set |
| `target_range_authoring_round_trip` | **the authoring loop, on a game that is not the block.** `editor --smoke` opens the scene through `target-range-host`, edits it and saves it; the game then runs the saved file and must still clear it, with a **different** authored digest |

That last one needs both clauses. A differing digest alone would be satisfied by an editor that
corrupted the scene; a passing self-check alone would be satisfied by an editor that changed nothing.
Together they say the editor made a real change and the result is still a playable range. It skips
cleanly when the Rust half is not built, because a `--cpp-only` build legitimately has no editor.

## The game brings its own editor host

Pointing the editor at the generic `rime-engine` and asking it to save this scene produces:

```
refusing to save: this build did not understand 5 component(s) in the scene it loaded,
and saving would delete them. Open it in a build that registers them.
```

**That refusal is the correct behaviour and it is where this brick got interesting.** `rime-engine`
registers the engine's components and the block's; it has never heard of `targetrange::Target`, so a
save through it would silently drop every crate's points, its `required` flag and its knock impulse.
The engine noticing and stopping is what you want from a tool that owns your content.

The answer is the seam m15.2 built for exactly this: `run_editor_host` is a **library**, so a game
that wants the editor to understand it builds its own host — `target-range-host` — and answers two
questions there: what its components **are** (the registrar) and what they **look like** (the
viewport preparer). The editor already takes `--engine <path>`, so pointing it at that binary is the
whole integration:

```bash
build/dev/bin/editor --engine build/dev/bin/target-range-host --scene samples/target-range/target_range.rscene
```

`host_main.cpp` is 90 lines, most of them comment. `the-block-host` is the same three lines for the
block. **That two unrelated games need the same three lines and no engine change is the platform
claim**, and it is the part of M15 that could not be proved by the block alone — one game with its own
host is a special case; two is a seam.

## What the proof found

A platform proof earns its keep by surfacing friction, so here is the friction.

**`derive_world_transforms` is missing from the public API.** A `.rscene` carries `LocalTransform`
only — `WorldTransform` is derived and deliberately unreflected — and
`ecs::propagate_transforms` only *recomputes* entities that already have both
(`engine/ecs/include/rime/ecs/transform.hpp:41`). So a freshly loaded scene has no world poses at
all, nothing binds to physics, and the range comes up with zero targets. **That is exactly how this
sample failed the first time it ran.**

The engine already contains the fix: `derive_world_transforms` in
`engine/app/editor_host_app.cpp:167`, whose own comment calls it "the same two-step 'load then
derive' every reflection-driven world reconstruction needs". It is file-local to that translation
unit and not exported, so **every game must write it again**. This sample writes it again, because
m15.8's diff may not touch `engine/` — promoting it into `rime::scene` is recorded as a follow-up
rather than smuggled into the proof.

**What is authored and what is not.** The scene is authored; the *look* is code. A `.rscene` names a
mesh by content id and this sample ships no cooked assets, so the crates' meshes and materials are
built in `run_rendered`. Cooked assets authored through the pipeline are M16's subject
([ADR-0039](../../docs/adr/0039-authored-surfaces-m16.md)), not M15's, and saying so beats implying
the crates' appearance came out of the editor too.

**The scene is generated, then editable.** `--emit-scene` writes the committed `.rscene`, the same
shape M14 established — `rime-blockgen` writes the block's scene, the editor changes it, the game
runs whatever file it is handed. Hand-writing the file is possible (the format is human-diffable and
says so) but would mean hand-computing every component's type hash.
