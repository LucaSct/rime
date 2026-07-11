# Tests for rime::capi — the C ABI (M6.9). Links the real librime_capi shared library and drives it
# through its public C header, so this suite proves the ABI end to end on every CI OS and under the
# ASan/UBSan job. Reuses the shared assets fixtures (quad.rmesh) — the same bytes the cross-language
# Rust and C++ reader tests consume.
rime_add_test(rime_capi_tests capi_test.cpp)
target_link_libraries(rime_capi_tests PRIVATE rime::capi)
target_compile_definitions(rime_capi_tests
    PRIVATE RIME_CAPI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/fixtures")
