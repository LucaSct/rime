# rime_stream — graphics streaming: capture -> encode -> transport (Track S).
#
# S0.2 seeds this module with the **frame tap** (FrameStreamer): copy a rendered RHI texture back to
# the CPU, double-buffered and timed, so a later brick can encode it (S0.3) and send it over the
# platform TCP sockets (S0.1). Later bricks add the codec and the versioned protocol; the whole
# stack is meant to be a shippable, removable engine feature (engine/stream over the future
# engine/net), per ADR-0016 and docs/design/graphics-streaming.md.
#
# It depends only on the RHI *interface* (any backend) and — transitively — core, so it captures from
# Vulkan today and any future backend unchanged. No Vulkan headers here.
add_library(rime_stream STATIC
    src/frame_streamer.cpp)

# Namespaced alias so dependents link rime::stream (matching the other modules).
add_library(rime::stream ALIAS rime_stream)

target_include_directories(rime_stream
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The RHI interface gives us Device / handles / descriptors; rime::core (log, handles) comes
# transitively through rime::rhi.
target_link_libraries(rime_stream
    PUBLIC rime::rhi)

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_stream)
