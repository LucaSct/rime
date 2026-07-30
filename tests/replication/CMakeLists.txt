# Tests for rime::replication (m11.3). GPU-free and deterministic: every case runs over the
# ScriptedNetwork's virtual clock, so injected loss and reordering are inputs rather than luck.
rime_add_test(rime_replication_tests
    replication_test.cpp)

target_link_libraries(rime_replication_tests
    PRIVATE rime::replication)
