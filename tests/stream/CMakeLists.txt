# Tests for rime::stream. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest,
# applies the build policy, and registers it with CTest; we add the modules under test on top.
#
# The frame-tap proof renders off-screen and reads the pixels back, so it needs a real device — it
# is added under the RIME_RHI_VULKAN guard in tests/CMakeLists.txt, and set RIME_REQUIRE_VULKAN in
# the environment (CI does) to make a missing device a failure rather than a skip.
rime_add_test(rime_stream_tests
    frame_streamer_test.cpp)

target_link_libraries(rime_stream_tests
    PRIVATE rime::stream rime::rhi)
