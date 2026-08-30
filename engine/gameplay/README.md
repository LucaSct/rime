# engine/gameplay — the character controller

`rime::gameplay` is the layer where physics queries become a *player*: a kinematic capsule
driven by **collide-and-slide** over the `PhysicsWorld` seam, and a **hitscan weapon** that shoots
through the same query interface. It was scoped in
[ADR-0035](../../docs/adr/0035-vision-demo-m12.md) §3 (m12.2/m12.3) and exists so that m12.3/m12.4
can network and *predict* a mover that is already proven deterministic — reconciliation gets
debugged against the network, never against the mover.

**`step_character` is a pure function.** `(state, input, config, world, dt) → state`, no writes to
the world it observes, no hidden statics — the replay tests hold it to bit-identity. `step_weapon`
is held to the same standard for the same reason: m12.4 replays the last N ticks after a correction,
and a weapon that remembered anything outside `WeaponState` would fire a different number of shots
on the replay than it did the first time. The module depends on `core`, `ecs` and the *interfaces*
of `physics` (`shape_cast`, `raycast`, `penetration`, `QueryFilter`); nothing here reaches under
`engine/physics/src`. The wire types stay out: `CharacterInput` is this module's own shape, and
`gameplay_net` (m12.3) converts.

The controller's design is argued **in the code, next to the numbers that forced it** —
`src/character.cpp` is the textbook chapter. The shortest possible summary of its commitments:

- **Skin is a contact offset, not an epsilon** — movement queries sweep a capsule *inflated* by
  `skin`; nothing subtracts a distance along a sweep and divides by a cosine nobody applied.
- **Blocking is the cast's call; standing is the ray's.** "What am I standing on" is answered by a
  raycast under the axis (analytic against boxes/hulls), never by a shape query's resting-contact
  normal, and never by an edge-contact normal — those read walkable at the crest of a wall.
- **Steps are climbed by asking the world, not the normal** — a three-cast ladder (lift, advance a
  radius, set the foot down) whose every acceptance is a measurement, ending with a confirm that
  the landed pose can actually be stood in.
- **The tick certifies what it hands back.** Depenetration recovery runs at the start *and* the end
  of the tick under one shared budget, so no observer — renderer, snapshot, trigger — ever sees a
  penetrated pose; `grounded` is returned only when a ray has vouched for it at the final pose.
- **Every give-up has a counter** (`StepStats`, `FireStats`) — guardrail 5's rule: a skip that
  cannot be seen still reads as passing, so nothing here skips silently.
- **The weapon is a query, not an object** — one raycast, resolved inside the tick that fired it,
  with no projectile entity to network. `RayHit::child` names the destructible part it struck, which
  is what lets hitscan address a part without this module knowing what a part is. The damage itself
  is the consumer's: `gameplay` never links `destruction`.

## Status

| Brick | Provides | State |
| --- | --- | --- |
| m12.2 | `step_character` collide-and-slide (slide loop, plane clipping, creases), step-up/step-down, ground snap, depenetration recovery, jump/gravity, reflected `CharacterState`/`CharacterConfig`; proofs: analytic slope/step/slide bounds, replay determinism, recovery convergence, and a 1,296-cell structural probe grid | landed |
| m12.3 | **Weapon v1** — `step_weapon` deterministic hitscan, `aim_direction`/`character_aim` (the one view basis), semi-auto vs. automatic, tick-counted cooldown, reflected `WeaponConfig`/`WeaponState`; proofs: the aim convention against its own definition, self-exclusion, the eye-height cover case with its negative control, the cooldown period at four values, and replay bit-identity | landed |
| m12.4 | prediction + reconciliation replay `step_character` from a corrected state — the purity this module was built for, cashed in ([`gameplay_net`](../gameplay_net/README.md)) | landed |
| m12.6 | the M12 milestone proof — [`samples/13-networked-player`](../../samples/13-networked-player). **M12 "The Player" closes here.** | landed |
| m13.3a | `FirstPersonView` — pointer motion in, an eye transform out; pure, no world, no entities. Its forward is asserted to *be* `step_character`'s forward across 37 angles | landed |
| m13.3c | **`input_map.hpp`** — `InputBindings`, `FrameIntent`, `map_frame_input()`, `fly_step()`, `FlyCamera`. The device→intent map, and the only thing in this module that touches `rime::platform` | landed |

### On the `platform` edge (m13.3c)

`input_map.hpp` is why this module links `rime::platform`, and that edge is **downward** — platform
sits two layers below gameplay in the cake ([ARCHITECTURE §2](../../docs/ARCHITECTURE.md)). It is not
the same kind of edge as the ones the build file deliberately refuses (`destruction`, `net`), which
are sideways to peer feature modules and would make a single-player build drag in the whole
networking stack. Platform is GPU-free and has a null backend, so every proof here still runs
headless on any CI machine.

The brick existed because both halves of the map were already built and connected to nothing:
`FirstPersonView` was referenced only by its own header, `.cpp` and test, and `platform::Input`
(M2.3) only by its own test. A window opened in m13.3a with no way to move in it. The lesson is in
`tests/app/windowed_input_test.cpp`, which asserts the *join* rather than either part.

One limitation is real and named: look is a **right-drag**, not free look, because
`platform::CursorMode::Locked` is declared in `mouse.hpp` and implemented by no backend — there is no
`Window::set_cursor_mode()` at all. Set `InputBindings::look_requires_drag = false` when pointer
capture becomes a platform brick.

## Named costs (v1 deferrals, each argued at its site in `src/character.cpp`)

- **A step under a low ceiling is not climbed** — an obstructed lift refuses outright rather than
  searching for the tallest lift that fits.
- **A climbable step with a wall less than a radius beyond it is refused** — the ladder's progress
  gate demands a full radius of measured clearance, because less than that is what "still inside
  the riser" looks like to the down-probe.
- **A step exactly at `step_height` met head-on wedges instead of climbing** — the advance leaves
  the axis skin-short of the riser plane, the foot-down ray sees the floor behind, and the
  foot-fit confirm (correctly) refuses the landing. Approached off-axis it is climbed in two
  ticks. The wedge is clean: the character stops, nothing penetrates.
- **The ground ray is infinitely thin** — a capsule straddling a gap narrower than itself is
  judged by what is under its axis, and a pose whose axis overhangs an edge does not count as
  grounded. No perching.
- **The weapon has no spread, no recoil and no ammo** — each is a game rule that would want a
  random source, and on a predicted path that has to be a seeded stream both peers agree on. That is
  its own design, not a rider on the brick that first proves a shot crosses the wire.
- **A shot is resolved with no lag compensation** — against the world as it stands on the tick the
  server consumed the command, so a moving target must be led. Ruled out of M12 by ADR-0035 §4: the
  block's targets are buildings, and buildings do not dodge.
- **Test geometry is capped at ~10 m half-extents** — GJK's overlap predicate measurably loses
  shallow overlaps against very large convex shapes (see `tests/gameplay/character_fixture.hpp`
  for the measured table). That is an upstream physics limitation, reported there, not a
  controller property.

## Tests

`tests/gameplay/` — analytic slope/step/slide cases (closed-form expectations, not golden values),
recovery convergence, replay determinism, `weapon_test.cpp` (the weapon in isolation: one shooter,
static boxes, no network anywhere — so `tests/gameplay_net` debugs the consume loop and never the
gun), and `character_grid_test.cpp`: 18 slopes x 12 headings x 6 step heights asserting only
structural invariants (no NaN, no tick ends overlapping, no falling through the world, no unearned
`grounded`, bounded speed, bounded give-up counters). A failing cell prints itself as a
reproduction.

`input_map_test.cpp` (m13.3c) covers the device→intent fold: that the frame is rolled so edges stay
edges, that opposed keys cancel, that a diagonal lands on the unit disc and not the unit square, and
that `fly_step`'s basis re-derives `character.cpp`'s own right/forward across 24 yaws. The *join* is
proved elsewhere on purpose — `tests/app/windowed_input_test.cpp` drives a real `Application` with
injected events, and `first_light --input-selftest` is a CTest on the binary, because only running it
can show a sample actually wires one up.
