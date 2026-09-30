# hello-game — the smallest complete game in this repository

Walk a character around an arena, touch five markers, push a crate out of the way, win.

```bash
build/<preset>/bin/hello_game              # the self-check: GPU-free, silent, exit 0
build/<preset>/bin/hello_game --verbose    # the same, with a report
build/<preset>/bin/hello_game --headless   # render it off-screen (lavapipe / CI)
build/<preset>/bin/hello_game --windowed   # play it: WASD to move, Esc to quit
build/<preset>/bin/hello_game --dedicated-proof --verbose   # dedicated == play, no Vulkan loaded

# The shipped-game CLI (m20.1) — the engine's, not this sample's:
build/<preset>/bin/hello_game play                               # a window, keyboard input
build/<preset>/bin/hello_game dedicated --ticks 600 --autopilot  # headless, no GPU, prints a digest
build/<preset>/bin/hello_game browser --port 8443                # "not implemented yet", exit 3
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

Since **m20.1** the game and its host are two files, and the line between them is a type:

| file | what it is |
| --- | --- |
| `game.hpp` / `game.cpp` | everything the game **is** — a `rime::app::Game` (`setup`, `fixed_tick`, `autopilot`, `state_digest`, `present_setup`, `present_frame`) and the `GameDefinition` that hands it to the engine |
| `main.cpp` | this sample's self-checks — nothing a shipped game would need |

The loop, the device, the window, the key mapping and the launch mode are the **engine's**
(`rime/app/launch.hpp`, [ADR-0056](../../docs/adr/0056-m20.1-game-definition.md)). A game exported
under M20 has a one-line `main`: `return rime::app::run_game(argc, argv, definition());` — which is
what `hello_game <mode> …` runs.

ADR-0046's riskiest assumption was that this lift could be done **without rewriting the game**. It
could: the rules did not change. What moved was the physics world (into the game — the engine does
not own physics), the key mapping (into the engine, as an `InputMap`), and the render-only setup
(out of `setup` into `present_setup`, so the simulated world is the same entity for entity whether
or not anything draws it).

## Three decisions worth reading the code for

**Input is an `Intent`, never a keystroke.** The engine hands the game an `ActionState` (axes and
buttons it declared in its `InputMap`); the game reads it as a direction and a quit flag. This is not tidiness. It is what lets the same rules run from
a keyboard, from a scripted sequence, and — Track H — from a browser's DataChannel, with no branch
inside the game. The self-check depends on exactly that: it drives the game from a fixed script and
gets a bit-reproducible result. A game that read `platform::Event` directly could only be tested by
synthesising key events, which tests the key mapping and the rules together and cannot say which one
broke.

**Input applies per tick, not per frame.** Input arrives per *frame*; the simulation advances per
*tick*, and a frame may run zero ticks or eight. Since m20.1 the engine maps each frame's events
once and hands every tick an `ActionState`, with key presses latched until a tick consumes them — so
the player's speed never depends on the frame rate and a quick tap is never lost.

**The pickups are real trigger volumes.** `Collider::sensor` was reflected and honoured by nothing
until m15.6 made it fire `TriggerEvent`s. A sample that faked the overlap with a
`length(a - b) < r` test would quietly stop exercising the feature it looks like it uses. The player
is a **kinematic** capsule, so `PhysicsSync::push_in` drives the body from the transform the game
writes — which is also why the crate is *pushed* at walking speed instead of being fired across the
floor by a teleport-induced penetration.

## What the tests prove

`hello_game_selftest` is GPU-free, so it gates on every CI OS and under both sanitizers — the rules,
the character controller and the trigger volumes are all CPU work. It asserts the autopilot
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

`hello_game_dedicated_proof` (m20.1) runs the game through the engine's `dedicated` mode and three
other ways — the pre-port hand-rolled loop, `play` on a machine with no GPU, and (where a device
exists) `play` really rendering — and requires **one state digest**, both mid-game (120 ticks) and at
the win. `dedicated` must enter the device factory **zero** times and call no presentation hook, and
on Linux `/proc/self/maps` must show no `libvulkan` after every GPU-free leg. Falsified both ways:
forcing the runner to create a device in `dedicated` fails the count and the `libvulkan` check, and
nudging the crate by 1 cm in `present_setup` fails the rendering leg's digest.
