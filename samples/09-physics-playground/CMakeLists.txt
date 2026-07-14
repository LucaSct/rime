# 09-physics-playground — Milestone 7's "done when" (M7.8): objects fall/collide/stack, raycasts hit,
# and the simulation runs on the job system inside the app's fixed tick. A static floor + a box tower
# + a pyramid + spheres, authored as ECS entities and driven through PhysicsSync; a raycast+impulse
# topples the tower; it settles, sleeps, and hashes identically across two runs (determinism,
# ADR-0026). GPU-free (physics is CPU-only), so it links rime::app (fixed-tick loop + ecs,
# transitively) and rime::physics — no RHI — and fmt for the report.
add_executable(physics_playground main.cpp)

target_link_libraries(physics_playground
    PRIVATE rime::app rime::physics fmt::fmt)

rime_apply_common_target_options(physics_playground)

# M7's "done when," in CI: the self-check returns non-zero on any failed assertion (the
# 05-ecs-playground pattern). No device is needed — pure CPU — so it runs everywhere ctest does,
# including under ASan/UBSan and TSan (the job-system parallel step is the threading surface).
add_test(NAME physics_playground_headless COMMAND physics_playground)
