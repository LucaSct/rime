# 00-hello-window — opens a native window and pumps input through the M2 platform layer. A minimal
# preview of the M2.5 proof (no rendering yet; that is M3). Links only rime::platform.
add_executable(hello_window main.cpp)

target_link_libraries(hello_window
    PRIVATE rime::platform)

rime_apply_common_target_options(hello_window)
