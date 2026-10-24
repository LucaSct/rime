# Authored thermal graphs and disposable previews above the core-only numerical fluids seam.
add_library(rime_fluids_scene STATIC src/preview.cpp)
add_library(rime::fluids_scene ALIAS rime_fluids_scene)
target_include_directories(rime_fluids_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rime_fluids_scene PUBLIC rime::ecs rime::fluids)
rime_apply_common_target_options(rime_fluids_scene)
rime_install_module(rime_fluids_scene)
