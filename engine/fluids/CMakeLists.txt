# A removable, CPU-only buoyancy seam. Physics is a consumer, never a dependency.
add_library(rime_fluids STATIC src/buoyancy.cpp)
add_library(rime::fluids ALIAS rime_fluids)

target_include_directories(rime_fluids
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rime_fluids PUBLIC rime::core)
rime_apply_common_target_options(rime_fluids)
rime_install_module(rime_fluids)
