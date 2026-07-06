# rime_app_tests — the application framework proofs (M5.7): the FixedTimestep accumulator (pure),
# and the fixed-tick loop's behaviour — tick determinism, a headless smoke run, input reaching a
# sim system, and (self-guarded on a device) the GPU-owning render-loop path. rime::app links
# rime::render → rime::rhi, so the suite always builds; the one GPU case skips when no device exists.
rime_add_test(rime_app_tests
    fixed_timestep_test.cpp
    application_test.cpp)

target_link_libraries(rime_app_tests
    PRIVATE rime::app)
