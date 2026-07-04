# 05-ecs-playground — the M4.6 proof: 200k entities updating in parallel through the ECS (with a
# measured speedup) and a transform hierarchy composing correctly. CPU-only (no RHI/GPU), so it links
# just rime::ecs (which brings rime::core) and fmt for the report.
add_executable(ecs_playground main.cpp)

target_link_libraries(ecs_playground
    PRIVATE rime::ecs fmt::fmt)

rime_apply_common_target_options(ecs_playground)
