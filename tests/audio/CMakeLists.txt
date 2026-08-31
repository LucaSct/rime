# Tests for rime::audio (M8.4 seam). The null backend and the mixer are pure CPU, so those run on
# every CI OS + ASan/UBSan + TSan with no device. sink_test.cpp additionally TRIES to open the real
# platform sink and skips itself when there is no device (or when one opens but never consumes) —
# sink.hpp makes "no device" an ordinary answer, so it must never redden a deaf runner.
rime_add_test(rime_audio_tests
    audio_test.cpp
    mixer_test.cpp
    sink_test.cpp)

target_link_libraries(rime_audio_tests
    PRIVATE rime::audio)
