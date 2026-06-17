# Sample projects that exercise the engine. Each is a small executable that links the engine
# modules it demonstrates and doubles as a runnable proof for the milestone that introduced it.
# Built when RIME_BUILD_SAMPLES is ON (the default); see the top-level CMakeLists.
add_subdirectory(jobs_core_saturation) # M1.6: work-stealing job system saturating all cores
add_subdirectory(00-hello-window)      # M2: open a native window, pump input (preview of M2.5)
