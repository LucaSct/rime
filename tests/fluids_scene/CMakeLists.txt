rime_add_test(rime_fluids_scene_tests preview_test.cpp)
target_link_libraries(rime_fluids_scene_tests PRIVATE
    rime::fluids_scene rime::worldkit rime::editorhost rime::scene rime::core)
