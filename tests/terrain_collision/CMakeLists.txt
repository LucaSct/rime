# m19.8c: regional terrain collision — headless, GPU-free, no threads and no sleeps (the "async"
# loader is a fake whose completions the test delivers in the order it chooses).
rime_add_test(rime_terrain_collision_tests
    determinism_test.cpp
    admission_test.cpp
    residency_test.cpp
    refusal_test.cpp
    prediction_test.cpp
    asset_server_source_test.cpp)

# gameplay_net is the TEST's dependency, not the module's: the client fixture drives the real
# Predictor against a world this module populates. The module itself links neither.
target_link_libraries(rime_terrain_collision_tests
    PRIVATE rime::terrain_collision rime::gameplay_net)

target_include_directories(rime_terrain_collision_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(rime_terrain_collision_tests
    PRIVATE RIME_ASSETS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
