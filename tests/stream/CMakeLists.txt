# Tests for rime::stream. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest,
# applies the build policy, and registers it with CTest; we add the modules under test on top.

# The codec proof (S0.3) operates on CPU pixel buffers — **no GPU** — so it builds and runs on every
# CI OS, including the ones with no Vulkan device. It links only rime::stream (the codec libraries
# come through it, PRIVATE). This is the codec coverage the streaming plan asks to keep GPU-free.
rime_add_test(rime_stream_codec_tests
    frame_codec_test.cpp)

target_link_libraries(rime_stream_codec_tests
    PRIVATE rime::stream)

# The frame-tap proof (S0.2) renders off-screen and reads the pixels back, so it needs the Vulkan
# backend and a real device — build it only when the backend is on. Set RIME_REQUIRE_VULKAN in the
# environment (CI does) to make a missing device a failure rather than a skip.
if(RIME_RHI_VULKAN)
    rime_add_test(rime_stream_tests
        frame_streamer_test.cpp)

    target_link_libraries(rime_stream_tests
        PRIVATE rime::stream rime::rhi)
endif()
