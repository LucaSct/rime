# Tests for rime::worldkit (m14.1) — the component profile.
#
# GPU-free and asset-free: the block's `.rscene` is assembled and serialized in memory, so this needs
# no cooked `.rdest` and runs on every CI OS and under both sanitizers. That matters, because the
# case it exists for — "the block's scene loads with zero unknown component types" — is the gate that
# would have caught `blockkit::SlabRole` on the day m13.2c landed, and a gate is only worth having if
# it runs everywhere.
#
# It links rime::blockkit deliberately: the profile must be shown NOT to register a game's content
# (the negative control), and the only way to show that is to have a game's content to hand.
rime_add_test(rime_worldkit_tests
    worldkit_test.cpp)

target_link_libraries(rime_worldkit_tests
    PRIVATE rime::worldkit rime::blockkit rime::scene rime::gameplay rime::physics
            rime::render rime::ecs rime::core)
