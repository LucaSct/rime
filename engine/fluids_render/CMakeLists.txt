# Optional validated thermal state -> transient rendering factors. Neither lower module knows it.
add_library(rime_fluids_render STATIC src/phase_surface.cpp)
add_library(rime::fluids_render ALIAS rime_fluids_render)
target_include_directories(rime_fluids_render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rime_fluids_render PUBLIC rime::fluids_scene rime::render)
rime_apply_common_target_options(rime_fluids_render)
rime_install_module(rime_fluids_render)
