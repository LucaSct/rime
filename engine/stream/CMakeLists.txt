# rime_stream — graphics streaming: capture -> encode -> transport (Track S).
#
# S0.2 seeded this module with the **frame tap** (FrameStreamer): copy a rendered RHI texture back to
# the CPU, double-buffered and timed. S0.3 adds the **codec** (FrameEncoder/FrameDecoder): shrink that
# frame with JPEG or LZ4 so it fits a network (ADR-0017). S0.4 adds the **protocol** (ProtocolConnection
# + FrameMessage/InputEvent): a versioned, length-prefixed message stream over the platform TCP sockets
# (S0.1). The whole stack is meant to be a shippable, removable engine feature (engine/stream over the
# future engine/net), per ADR-0016 and docs/design/graphics-streaming.md.
#
# It depends only on the RHI *interface* (any backend) and — transitively — core, so it captures from
# Vulkan today and any future backend unchanged. No Vulkan headers here.
add_library(rime_stream STATIC
    src/frame_streamer.cpp
    src/frame_codec.cpp
    src/protocol.cpp)

# Namespaced alias so dependents link rime::stream (matching the other modules).
add_library(rime::stream ALIAS rime_stream)

target_include_directories(rime_stream
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The RHI interface gives us Device / handles / descriptors; rime::platform gives us the TCP sockets
# the protocol streams over (protocol.hpp names platform::TcpSocket, so PUBLIC); rime::core (log,
# handles) comes transitively.
#
# The codec libraries are linked PRIVATE: frame_codec.cpp is the only place that includes
# <turbojpeg.h>/<lz4.h> — the public header hides them behind opaque void* handles — so consumers of
# rime::stream (e.g. the S0.5 thin client) link the codecs transitively but never see their headers.
# Targets resolved in the root CMakeLists (…-static for our static build). See conanfile.py, ADR-0017.
target_link_libraries(rime_stream
    PUBLIC rime::rhi rime::platform
    PRIVATE ${RIME_TURBOJPEG_TARGET} ${RIME_LZ4_TARGET})

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_stream)
