# hello-game — the smallest complete game in this repository

Walk a character around an arena, touch five markers, push a crate out of the way, win.

```bash
build/<preset>/bin/hello_game              # the self-check: GPU-free, silent, exit 0
build/<preset>/bin/hello_game --verbose    # the same, with a report
build/<preset>/bin/hello_game --headless   # render it off-screen (lavapipe / CI)
build/<preset>/bin/hello_game --windowed   # play it: WASD to move, Esc to quit
```

## Why it exists

Two milestones point at this sample for two different reasons, and both of them want it small.

- [ADR-0038](../../docs/adr/0038-platform-proof-m15.md) §"what's missing" named it as **m15.7, the
  on-ramp** — the sample a newcomer reads first. Every other sample here demonstrates a *subsystem*:
  a triangle, a render graph, a physics scene. None of them shows the shape of a **game**. It was
  the first item on that ADR's cut list and it was duly cut, so this is a debt being paid rather
  than new scope.
- [ADR-0046](../../docs/adr/0046-exported-games-and-the-blender-boundary.md) §1 then made it a
  **prerequisite of M20**, the shipped game. That ADR records its own riskiest assumption: that a
  game's play loop can be lifted out of its sample `main` into an engine-owned `GameDefinition`
  **without rewriting the game**. The block's loop is ~3000 lines of sample code and is the wrong
  first subject for finding that out. This is the right one.

## The structure is the point

Everything the game **is** lives in `HelloGame`: its scene, its rules, its state, and three methods.

| method | what it does |
| --- | --- |
| `setup` | builds the world — floor, player, crate, five markers, a sun, a camera |
| `apply_intent` | receives the player's wish for the next tick |
| `fixed_tick` | advances one fixed step and applies the rules |

Everything about **running** it — argument parsing, the `app::Application`, the window, the
renderer, the self-check — lives in the host code beside it and touches the game only through those
three methods. Nothing in `HelloGame` knows whether it is being rendered; nothing in the host knows
the rules.

**That line is where M20's `GameDefinition` will be cut.** If lifting `HelloGame` into the engine
turns out to need the rules to know about a device, a window or a frame, then ADR-0046's assumption
is false — and we find out here, in 500 lines, instead of in the block.

## Three decisions worth reading the code for

**Input is an `Intent`, never a keystroke.** The game is handed a direction and a quit flag; mapping
keys to that intent is the host's job. This is not tidiness. It is what lets the same rules run from
a keyboard, from a scripted sequence, and — Track H — from a browser's DataChannel, with no branch
inside the game. The self-check depends on exactly that: it drives the game from a fixed script and
gets a bit-reproducible result. A game that read `platform::Event` directly could only be tested by
synthesising key events, which tests the key mapping and the rules together and cannot say which one
broke.

**The intent is stored, not acted on immediately.** Input arrives per *frame*; the simulation
advances per *tick*, and a frame may run zero ticks or eight. Applying a move the moment it arrives
would make the player's speed depend on the frame rate.

**The pickups are real trigger volumes.** `Collider::sensor` was reflected and honoured by nothing
until m15.6 made it fire `TriggerEvent`s. A sample that faked the overlap with a
`length(a - b) < r` test would quietly stop exercising the feature it looks like it uses. The player
is a **kinematic** capsule, so `PhysicsSync::push_in` drives the body from the transform the game
writes — which is also why the crate is *pushed* at walking speed instead of being fired across the
floor by a teleport-induced penetration.

## What the two tests prove

`hello_game_selftest` is GPU-free, so it gates on every CI OS and under both sanitizers — the rules,
the character controller and the trigger volumes are all CPU work. It asserts the scripted player
wins, **and** that the identical scenario on a different worker-thread count reaches the identical
result marker by marker and tick by tick (ADR-0026 determinism). A scalar score would also match a
run that collected the markers in another order, which is the failure a determinism proof is for.

`hello_game_headless` renders off-screen and checks the **pixels**: the scene is lit, and
green-dominant pixels are present — the markers are the only green-dominant thing in the scene, so
that is proof the trigger volumes are actually drawn. A render smoke test that only checks the exit
code passes on a black frame, and this engine has already lost time to a pass that was culled for
119 of 120 frames while every number still looked plausible.

Both checks were **falsified** before being trusted: clearing `Collider::sensor` makes the self-check
report 0 of 5, and greying the marker material makes the frame check report 0 marker pixels. The
first version of the frame check asked for "gold" pixels and the brown crate satisfied it — which is
the entire reason to run a falsification rather than to admire a green test.
