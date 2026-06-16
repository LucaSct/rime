# rime_hello — a minimal launcher (Milestone-0 stub).
#
# Its only job today is to prove the engine builds, links a module (rime::core), and
# runs. It is the seed of the `app` module (application framework + main loop) described
# in docs/ARCHITECTURE.md, and will grow into the real entry point over later milestones.

add_executable(rime_hello
    main.cpp)

target_link_libraries(rime_hello
    PRIVATE rime::core fmt::fmt)

rime_apply_common_target_options(rime_hello)
