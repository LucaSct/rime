# rime_app_tests — the application framework proofs (M5.7): the FixedTimestep accumulator (pure),
# and the fixed-tick loop's behaviour — tick determinism, a headless smoke run, input reaching a
# sim system, and (self-guarded on a device) the GPU-owning render-loop path. rime::app links
# rime::render → rime::rhi, so the suite always builds; the one GPU case skips when no device exists.
# editor_play_test.cpp (m9.7) additionally links rime::editorhost (the PlaySession state machine)
# and rime::physics (the side-table the play/restore proof exercises) — both GPU-free, so the whole
# suite still runs headless on every CI OS.
rime_add_test(rime_app_tests
    fixed_timestep_test.cpp
    application_test.cpp
    editor_play_test.cpp)

target_link_libraries(rime_app_tests
    PRIVATE rime::app rime::editorhost rime::physics)
