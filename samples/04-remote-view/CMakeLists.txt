# 04-remote-view — the S0 remote-view endpoint (Track S). One binary, two roles:
#   server: render a scene off-screen, stream it (tap -> codec -> protocol) and apply the input a
#           client sends back — the headless endpoint you run on a lavapipe/GPU box.
#   client --headless: connect, script input, receive+decode frames, report (GPU-free).
# The windowed client (present decoded frames in a real window) is roadmapped (S0.5) and needs a
# display. Links rime::stream (the whole Track-S stack), rime::rhi (the server's render), and
# rime::platform (the sockets).
add_executable(remote_view main.cpp)

target_link_libraries(remote_view
    PRIVATE rime::stream rime::rhi rime::platform)

rime_apply_common_target_options(remote_view)
