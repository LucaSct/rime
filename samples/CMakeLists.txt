# Sample projects that exercise the engine. Each is a small executable that links the engine
# modules it demonstrates and doubles as a runnable proof for the milestone that introduced it.
# Built when RIME_BUILD_SAMPLES is ON (the default); see the top-level CMakeLists.
add_subdirectory(jobs_core_saturation) # M1.6: work-stealing job system saturating all cores
add_subdirectory(00-hello-window)      # M2: open a native window, pump input (preview of M2.5)

# Needs the Vulkan RHI backend (the agnostic interface alone can't render).
if(RIME_RHI_VULKAN)
    add_subdirectory(01-hello-triangle) # M3.3/M3.4: a triangle off-screen, then presented in a window
    add_subdirectory(02-textured-quad)  # M3.5: an indexed, textured quad (M3's "done when")
    add_subdirectory(03-icem-viewer)    # viewer B1: load a computed part (STL), draw it lit + depth-correct
endif()
