# Tests for rime::ecs (M4). rime_add_test (defined in tests/CMakeLists.txt) builds the exe, links
# doctest, applies the project build policy, and registers it with CTest; we add the module on top.
rime_add_test(rime_ecs_tests
    entity_directory_test.cpp
    world_test.cpp
    component_registry_test.cpp
    signature_test.cpp
    chunk_pool_test.cpp
    chunk_test.cpp)

target_link_libraries(rime_ecs_tests
    PRIVATE rime::ecs)
