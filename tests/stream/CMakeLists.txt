# Tests for rime::stream. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest,
# applies the build policy, and registers it with CTest; we add the modules under test on top.

# The codec proof (S0.3) operates on CPU pixel buffers — **no GPU** — so it builds and runs on every
# CI OS, including the ones with no Vulkan device. It links only rime::stream (the codec libraries
# come through it, PRIVATE). This is the codec coverage the streaming plan asks to keep GPU-free.
rime_add_test(rime_stream_codec_tests
    frame_codec_test.cpp)

target_link_libraries(rime_stream_codec_tests
    PRIVATE rime::stream)

# The s1.2 video-codec proof (VideoEncoder/VideoDecoder — SVT-AV1 + dav1d) is likewise GPU-free
# CPU-buffer work: stream round-trip under a PSNR bound, one-in-one-out latency, forced-keyframe
# join, malformed-input rejection. The codec libraries stay PRIVATE to rime::stream.
rime_add_test(rime_stream_video_tests
    video_codec_test.cpp)

target_link_libraries(rime_stream_video_tests
    PRIVATE rime::stream)

# The s1.3 latency-ledger proof (LatencyLedger/LatencyStats/estimate_offset) is pure CPU
# bookkeeping — no GPU, no wire — so it runs on synthetic timelines with known delays on every OS.
rime_add_test(rime_stream_latency_tests
    latency_test.cpp)

target_link_libraries(rime_stream_latency_tests
    PRIVATE rime::stream)

# The protocol proof (S0.4) is also GPU-free: serialization round-trips plus a loopback integration
# test over 127.0.0.1 (handshake + input + a JPEG frame, end to end). It needs the sockets, so it
# links rime::platform alongside rime::stream.
rime_add_test(rime_stream_protocol_tests
    protocol_test.cpp)

target_link_libraries(rime_stream_protocol_tests
    PRIVATE rime::stream rime::platform)

# The frame-tap proof (S0.2) renders off-screen and reads the pixels back, so it needs the Vulkan
# backend and a real device — build it only when the backend is on. Set RIME_REQUIRE_VULKAN in the
# environment (CI does) to make a missing device a failure rather than a skip.
if(RIME_RHI_VULKAN)
    rime_add_test(rime_stream_tests
        frame_streamer_test.cpp)

    target_link_libraries(rime_stream_tests
        PRIVATE rime::stream rime::rhi)

    # The engine-side capstone (S0.6): a real rendered frame streamed end to end over a loopback
    # socket — render -> tap -> encode -> protocol -> decode -> verify bit-exact. Needs a device (the
    # render + readback), so it rides the same Vulkan guard and skips cleanly without one.
    rime_add_test(rime_stream_loopback_tests
        loopback_stream_test.cpp)

    target_link_libraries(rime_stream_loopback_tests
        PRIVATE rime::stream rime::rhi rime::platform)
endif()
