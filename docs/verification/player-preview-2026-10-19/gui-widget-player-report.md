# Player widget integration checks

Modified only `tools/editor/src/gui/click_tests.rs` manually on the shared checkout; no commits. Preserved preexisting FakeHost KeyDown/KeyUp recording. FakeHost now validates decoded PlayControl messages but does not synthesize controller state; tests explicitly inject Shared.play_control acknowledgements.

Four new actual EditorApp/egui Harness tests pass: capability-gated Walk checkbox, correct SelectPlayer/SelectInspect session intents and no inferred mode acknowledgement; selected gizmo-handle viewport click requests Capture while pending W/raw look/pick/gizmo/edit are blocked; matching capture acknowledgement establishes zero virtual look origin and permits one HID W down edge/raw look, while Escape emits W up plus Release without stopping Preview; Player Active and Paused both disable Save and block Ctrl+S. Renamed obsolete keyboard test to inspect_preview_does_not_forward_player_keys with current explanation.

`cargo fmt -p editor` completed. Requested `cargo test -p editor --features gui` exit0: **170 passed,0 failed**,0.60s execution. Log: `/home/next/reports/rime-player-performance-2026-10-19/gui-widget-player.log`. No C++/GPU build or test. Root owns all other source edits and commits.
