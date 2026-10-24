add_executable(thermal_courtyard main.cpp scene.cpp)
set_target_properties(thermal_courtyard PROPERTIES OUTPUT_NAME thermal-courtyard)
target_link_libraries(thermal_courtyard PRIVATE rime::editorhost_app rime::worldkit
    rime::fluids_render rime::vfx_scene rime::scene rime::render)
rime_apply_common_target_options(thermal_courtyard)
add_test(NAME thermal_courtyard_selftest COMMAND thermal_courtyard --selftest --scene ${CMAKE_CURRENT_SOURCE_DIR}/courtyard.rscene)
