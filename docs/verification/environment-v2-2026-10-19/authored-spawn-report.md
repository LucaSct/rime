# Authored city player spawn — report

Worktree `rime-wt/authored-city-spawn`, from 8f80481. One focused commit; not pushed.

## What changed
- `samples/99-the-block/player_spawn.hpp` (new, header-only): reflected `PlayerSpawn{bool enabled=false}`; pure `resolve_player_spawn(World&)` -> `SpawnResolution{None|Valid|Refused, copied placement, enabled_count, reason}` (:105); shared `register_block_components` (:184), engine + blockkit + PlayerSpawn.
- `main.cpp`: `register_all` :231 and `block_host_main.cpp` :209 both call the shared helper. `Peer::spawn` resolved in `stand_up` :553, after derive and before ground/session. Refused -> stderr + `return false`, so `start()` and `--scene-info` fail early. The server prints "authored player spawn: capsule centre (...) yaw pitch" at :561, once. Uses: `spawn_avatar` :669, recovery :1139, initial `play_world->prepare` :1991, `run_play` look :5689 (the handshake `idle` input now reads the view, which is the same constants when no marker).
- `tests/gameplay/player_spawn_test.cpp` + CMake (adds the source and links scene/worldkit/blockkit/ecs).

## Contradicts the spec (plainly)
- `blockkit::derive_world_transforms` is flat-only and ignores `Parent`. For parent support the resolver composes the `LocalTransform` chain itself (`parent * local`, same rule as `propagate_transforms`). It does not depend on `WorldTransform`, and I did not change the shared derive.
- A look straight up or down (horizontal forward < 1e-3) is refused, because yaw is undefined. Un-normalised rotations are normalised; only length < 1e-3 counts as degenerate. These are my choices.

## Verified
- Measured: 7 cases / 54 assertions pass. They cover none/disabled, one marker with roll and scale ignored and the look direction cross-checked against `look_direction`, a 3-level parented pose with archetype churn and marker despawn, duplicate refusal in two storage orders, 9 refusal kinds (no transform, NaN, inf, NaN quat, zero quat, vertical, dangling parent, parent without LocalTransform, cycle), scene save/load round trip, and the shared registry (schema hash).
  Built outside CMake: the test plus a doctest main, linked against the main checkout's existing release `lib/*.a` in /tmp. Those libs may predate this HEAD.
- Measured: `-fsyntax-only` with the release flags is clean for the test, `main.cpp` and `block_host_main.cpp`. clang-format (`~/.rime-tools`) was run on the four owned C++ files.
- The no-marker path is unchanged by inspection (same expressions in the else branches). Not run.

## Not done / limitations
- Not run: the CMake target `rime_gameplay_tests`, `ctest`, `--headless`, `--scene-info`, any run with a marker. **No server/network proof**: the avatar placement, client look and recovery are wired but unproven at runtime. The helper tests do not prove them. Live proof = add an enabled PlayerSpawn to a saved city scene, run `--scene-info` or `--play`, and read the "authored player spawn" line.
- Not checked: whether a stale `Parent` entity ref in a hand-edited scene is rejected earlier by the loader.
- `WorldTransform` is unreflected, so the "excluded derived handles" counter reads 0. I assert the file text contains no `WorldTransform` instead.
- Avatar start overlapping geometry is not checked. Rotation of the avatar entity stays identity (look is client-only).
- Other modes (headless, demo, viewmodel proof at ~4348) keep their cameras untouched.
