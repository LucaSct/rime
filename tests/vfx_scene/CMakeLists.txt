rime_add_test(rime_vfx_scene_tests effects_test.cpp)
target_link_libraries(rime_vfx_scene_tests PRIVATE
    rime::vfx_scene rime::worldkit rime::editorhost rime::scene rime::core)
