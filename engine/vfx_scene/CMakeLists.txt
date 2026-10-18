# Authored ECS sources and a bounded presentation consumer. Keeping this adapter above vfx
# preserves vfx's core-only numerical/particle seam and permits a game to omit ECS altogether.
add_library(rime_vfx_scene STATIC src/effects.cpp)
add_library(rime::vfx_scene ALIAS rime_vfx_scene)
target_include_directories(rime_vfx_scene
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rime_vfx_scene PUBLIC rime::ecs rime::vfx)
rime_apply_common_target_options(rime_vfx_scene)
rime_install_module(rime_vfx_scene)
