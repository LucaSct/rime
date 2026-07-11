# 04-remote-view — the S0 remote-view endpoint (Track S). One binary, three roles:
#   server: render a scene off-screen, stream it (tap -> codec -> protocol) and apply the input a
#           client sends back — the headless endpoint you run on a lavapipe/GPU box.
#   client --headless: connect, script input, receive+decode frames, report (GPU-free).
#   client --window: the interactive viewer (S0.7) — present each decoded frame as a full-window
#           textured quad through a swapchain, and forward window key/mouse events as InputEvents.
#           Needs a display (a real screen, or Xvfb on this headless box). rime_add_shaders compiles
#           shaders/quad.{vert,frag} for that present path.
# Links rime::stream (the whole Track-S stack), rime::rhi (render + present), rime::platform (window,
# input, sockets).
add_executable(remote_view main.cpp)

target_link_libraries(remote_view
    PRIVATE rime::stream rime::rhi rime::platform)

rime_add_shaders(remote_view
    shaders/quad.vert
    shaders/quad.frag)

rime_apply_common_target_options(remote_view)

# S0.7 windowed smoke: present a few synthetic frames through the real window → swapchain → present →
# teardown path, in one process (no server). This is the regression net for the WINDOWED path, which
# the off-screen CI proof never exercised — so a window-teardown crash lived undetected until an Xvfb
# run surfaced it. The client SKIPS gracefully (exit 0) where there is no display or no Vulkan device,
# so this is a harmless no-op on a bare headless runner; the Linux CI build job starts a virtual
# display (Xvfb) so it actually runs there, and locally `xvfb-run ctest -R remote_view_window_smoke`
# exercises it. Needs no network, so it is a single fast process under the sanitizers.
add_test(NAME remote_view_window_smoke
    COMMAND remote_view client --window --selftest --frames 30)
