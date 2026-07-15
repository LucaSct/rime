# Tests for rime::vfx (M8.4 dust stub). Pure-CPU simulation (the GPU draw is m8.6), so this runs on
# every CI OS + ASan/UBSan + TSan with no device.
rime_add_test(rime_vfx_tests
    dust_test.cpp)

target_link_libraries(rime_vfx_tests
    PRIVATE rime::vfx)
