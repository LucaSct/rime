# Independent integration review

Read-only review of current root `editor_host_app.cpp`, `gui.rs`, `gui/player.rs`, `gui/session.rs`. No builds, runtime tests or edits performed. Line numbers describe the reviewed working tree.

## Finding — corrected by root during review

**P2 — Inspect Stop discards key releases without resetting its legacy controller.** `engine/app/editor_host_app.cpp:1072` invokes `release_player_input()` unconditionally, including Inspect mode. The helper at `:605–611` runs `input_translator.release_all(input_events)` and immediately clears the generated release events. Walk has a dedicated transition that clears its latches, but the unchanged legacy `BlockFlyCamera` receives neither that transition nor those KeyUps. Its camera reconciliation resets cached input only when an external camera pose differs. Thus an Inspect Play stopped at the exact original pose (for example W+S held together) can retain held keys into a fresh same-pose Play; releasing only one after restarting exposes stale opposite movement.

Root corrected the helper with an early return when mode is not Player and no player is admitted. This preserves the existing Inspect route and closes this new regression. A focused regression can cover opposing keys → Stop with unchanged camera pose → fresh Inspect Play → release one key. This was a static control-flow finding; that live sequence was not executed here.

## Reviewed without another concrete finding

Walk Begin/refusal/Stop resource ordering; Step-from-Edit and paused Step; pause/release clearing without a tick; capture acknowledgement and session checks; retained keyboard focus loss handling; fitted-image capture boundary; server refusal of authoring/save during possession; GUI restore gating; ordered socket session state handling. Separate PlayState/PlayControlState packets can briefly show mixed UI state, but server refusal prevents authoring/save corruption during that interval.

Known limitation already raised by root: a controller tick can halt internally while the generic host still reports active/captured. The present void tick API offers no failure result. A sample log is useful evidence, but a future optional tick-status channel or callback should expose that halt to the host and release capture. This is not an additional verified regression in the reviewed patch.
