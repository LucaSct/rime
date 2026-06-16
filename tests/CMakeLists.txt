# Rime unit tests.
#
# Each engine module gets a small test executable that links the module under test and
# doctest (header-only, provided by Conan). We register each executable as a single CTest
# test: doctest's runner returns a non-zero exit code if any assertion fails, which is
# exactly the signal CTest uses to report pass/fail. (If we later want per-case reporting
# we can switch to doctest_discover_tests, which scans the binary for individual cases.)

# Helper so adding a module's tests is a one-liner. It builds the test executable, links
# doctest, applies the project-wide build policy (C++20 + warnings-as-errors, from the
# top-level CMakeLists), and registers the binary with CTest. doctest is header-only and
# ships no prebuilt main(), so exactly one source file per executable must define
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN to supply one.
function(rime_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    rime_apply_common_target_options(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(core)
