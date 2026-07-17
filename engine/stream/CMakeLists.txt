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
    src/video_codec.cpp
    src/protocol.cpp)

# Namespaced alias so dependents link rime::stream (matching the other modules).
add_library(rime::stream ALIAS rime_stream)

target_include_directories(rime_stream
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The RHI interface gives us Device / handles / descriptors; rime::platform gives us the TCP sockets
# the protocol streams over (protocol.hpp names platform::TcpSocket, so PUBLIC); rime::core (log,
# handles) comes transitively.
#
# The codec libraries are linked PRIVATE: frame_codec.cpp is the only place that includes
# <turbojpeg.h>/<lz4.h>, and video_codec.cpp the only place that includes <EbSvtAv1Enc.h> /
# <dav1d/dav1d.h> (s1.2) — the public headers hide them behind opaque void* handles — so consumers
# of rime::stream (e.g. the S0.5 thin client) link the codecs transitively but never see their
# headers. Targets resolved in the root CMakeLists. See conanfile.py, ADR-0017, ADR-0030.
target_link_libraries(rime_stream
    PUBLIC rime::rhi rime::platform
    PRIVATE ${RIME_TURBOJPEG_TARGET} ${RIME_LZ4_TARGET} ${RIME_SVTAV1_TARGET} ${RIME_DAV1D_TARGET})

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_stream)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_stream)
