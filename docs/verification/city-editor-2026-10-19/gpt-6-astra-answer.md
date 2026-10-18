### Proposal

Finish water/ice/UI first, then deliver an explicit **author → save → launch game** workflow, not a replacement for editor Play or a production exporter.

**Scene and editor boundary**
- Generate the editable city scene with the same `city::expand` used by the game. Launching `--scene` must continue loading exactly that scene—no implicit city expansion.
- Extend `the-block-host` authoring previews for the procedural lamp/barrier patterns. Keep predicted player, weapons, and viewmodel resources runtime-owned.
- Label existing editor Play as **Preview**; add **Save & Run Game** for the external full runtime.

**Minimal launch API**
Accept optional `--game <executable>` and repeated `--game-arg <argument>`. Spawn directly as argv, never through a shell. The editor appends the authoritative `--play --scene <absolute-save-path>`; reject conflicting scene arguments.

Use one serialized save coordinator for manual saves and launch saves. State:
`Idle → SavingForRun → Running`, with failures returning to `Idle` and displaying the error.

For a launch:
1. Capture the target path and lock document switching, saving, and edits until completion.
2. Submit a save only after earlier saves have completed.
3. Await a **fresh ordered** SaveResult for that exact path; never accept an already populated `last_save`.
4. Launch only on success.

Because SaveResult lacks request IDs, this requires exclusive save ownership and a reliable fresh-result boundary. If that cannot be guaranteed, add request IDs before shipping; path matching alone is insufficient.

Disable launch during Preview, pending save, or an active game child. Track child exit and spawn errors visibly. Do not silently terminate the game when the editor closes; offer an explicit choice.

**Delivery**
Provide a development city bundle with copied scene, executable-relative cooked/viewmodel paths, and validated dependencies, assets, and attribution notices. Do not call the hello-game exporter a city production exporter.

**Negative tests**
Cover failed saves, stale/same-path results, queued manual saves, document switching, double clicks, missing executable/assets, paths containing spaces, conflicting arguments, child crashes, and editor shutdown. Verify save failure never launches an older scene and custom scenes never gain implicit city content.
