# jobs_core_saturation — the M1.6 proof: a runnable demo that saturates all cores through the
# work-stealing job system and reports the speedup. Links only rime::core.
add_executable(jobs_core_saturation main.cpp)

target_link_libraries(jobs_core_saturation
    PRIVATE rime::core)

rime_apply_common_target_options(jobs_core_saturation)
