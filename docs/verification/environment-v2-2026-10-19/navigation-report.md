# Editor navigation v2

Commit: `7349019` (`fix(editor): reconcile camera ownership during live navigation`)
Worktree: `/home/next/projects/rime-wt/editor-navigation-v2`
Base verified: exact `b5bf53b`; no edits in main checkout, saved scenes or running windows.

## Root cause (measured source behavior; user-visible reproduction still pending)

- Before this patch, `samples/99-the-block/block_host_main.cpp:182-210` at base b5bf53b seeded a runtime fly camera once and wrote its cached pose into the world on **every Play tick**, including empty event spans. The Rust viewport sends absolute camera LocalTransform edits during Play. The next city host tick therefore overwrote those edits with the old pose. This directly explains teleporting back while inspecting running effects. No window was driven by this sub-agent, so matching the user's exact operating sequence remains an inference.
- Before this patch, `tools/editor/src/gui.rs:1253-1260` seeded Rust fly state only once per engine connection, including position. Inspector edits, active camera replacement, and Stop could change the world pose without changing the cached position, so the next movement restored an old pose. This is independently covered by headless UI tests.

## Change

- `samples/99-the-block/editor_fly_camera.hpp:13-44`: pure, host-testable camera ownership policy; compare current world pose to last output. An external author pose or restored pose rebases the controller and clears old held input; self-owned ticks preserve continuous heading and input latches. Author scale survives.
- `samples/99-the-block/block_host_main.cpp:183`: use that controller instead of silently overwriting editor poses. Native navigation remains available in Playing/Paused.
- `tools/editor/src/gui.rs:246`: rebase camera cache on new camera, Play phase or external edit; preserve accumulated local navigation across repeated delayed snapshots. Ordered pending poses acknowledge when an authoritative snapshot catches up; a gesture ending requests one confirmation snapshot. `send()` explicitly recognises new inspector/undo intent even if it restores an old pending pose.
- `tools/editor/src/gui.rs:1308`: skip inactive cameras when selecting the navigation target.
- `tools/editor/src/gui.rs:1779`: preserve streamed image aspect ratio in wide/tall docks. Picking, gizmo rays and navigation use the actual displayed image rect; letterbox bars cannot start scene gestures. Existing captured drags can release through bars. Right-drag no longer grabs primary gizmos or emits phantom primary mouse-up.
- No wire format, global engine API, dependencies, browser deployment or scene serialization changes.

## Verification measured

- `CARGO_TARGET_DIR=/home/next/projects/rime/tools/target cargo test --manifest-path tools/Cargo.toml -p editor --features gui`: **143 passed, 0 failed**, including all inherited tests (0.58 s execution). Log: `/home/next/reports/editor-navigation-gui-tests.log`.
- New UI regressions cover external edits, Stop/reflight, camera replacement/activation, 120 delayed snapshot samples, continuous movement with a stalled fake engine, release acknowledgment, explicit undo to a pending baseline, fitted aspect, letterbox clicks/keys/wheel, captured primary release and no phantom right-button release.
- `cargo clippy ... -p editor --features gui --all-targets -- -D warnings`: passed. Log: `/home/next/reports/editor-navigation-clippy.log`.
- `cargo fmt ... --package editor -- --check`: passed. `git diff --check`: passed. Changed C++ files formatted with repository clang-format.
- New C++ policy and added `input_map_test.cpp` cases compiled with **clang 23 -fsyntax-only**, using real release compile commands and documented doctest compatibility suppression flags: exit 0.
- C++ regression cases `city editor: Play ticks preserve repeated author camera writes and release` and `city editor: restored or externally replaced camera clears old runtime input` are ready for the orchestrator's integrated `rime_gameplay_tests` run. This sub-agent did **not** claim to execute those C++ cases, rebuild host, run sanitizers or drive a live window.

## Review and limitations

- Performed my own adversarial diff review. It found/fixed the explicit undo-to-pending-baseline ambiguity before commit; no premium consultation was used for this brick.
- The protocol has no per-camera edit revision; a truly external snapshot equal to an outstanding older navigation sample is conservatively considered stale until latest acknowledgment. Explicit local inspector/undo commands bypass this ambiguity. A phase transition forces a rebase.
- The host callback supplies no Play-session identity; same-pose Stop/new Play cannot be distinguished from an ordinary idle tick. An actual restored pose change clears old runtime keys. No new API was introduced just for that edge; orchestrator should decide if explicit session reset merits its own later brick.
- Multiple simultaneously active cameras retain the existing first-match assumption; the editor now skips inactive cameras but the protocol does not tell it the renderer-selected EditorId.
- Spec naming `tools/editor/src/lib.rs` contradicted this checkout: shared state is `gui/session.rs`. That file was read, not changed.
- `/home/next/.claude/CLAUDE.md` does not exist; the live global file `/home/next/.claude-switch/profiles/default/CLAUDE.md` and repository CLAUDE.md were read.
