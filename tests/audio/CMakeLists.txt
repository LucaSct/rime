# Tests for rime::audio (M8.4 seam). The null backend is pure CPU, so this runs on every CI OS +
# ASan/UBSan + TSan with no device.
rime_add_test(rime_audio_tests
    audio_test.cpp)

target_link_libraries(rime_audio_tests
    PRIVATE rime::audio)
