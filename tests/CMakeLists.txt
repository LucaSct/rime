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
    # doctest's header transitively includes libstdc++'s <ciso646>, whose C++20 deprecation
    # #warning Clang surfaces even from a system header (GCC suppresses it). Under our -Werror
    # that becomes a fatal error, so silence just the #warning diagnostic for test targets on
    # Clang — third-party header noise, not our code. (The flag is quoted so CMake doesn't read
    # the '#' as a comment.) This is what lets the Clang-based CI TSan job build the suite.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE "-Wno-#warnings")
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(core)
add_subdirectory(ecs)
add_subdirectory(physics) # rigid-body core: pure-CPU, GPU-free — always built (M7)
add_subdirectory(destruction) # part-based destruction runtime: pure-CPU, GPU-free (M8)
add_subdirectory(audio)  # audio seam: null backend logs play() calls — GPU-free (M8.4)
add_subdirectory(vfx)    # destruction dust stub: CPU particle sim — GPU-free (M8.4)
add_subdirectory(platform)
add_subdirectory(assets) # RMA1 reader + registry + manifest: fully GPU-free (in-memory buffers)
add_subdirectory(viewer) # viewer-side render math (orbit camera, …); pure rime::core, no GPU needed
add_subdirectory(stream) # codec proof is GPU-free (always built); the frame-tap proof self-guards on RIME_RHI_VULKAN
add_subdirectory(app)    # fixed-tick loop + determinism: GPU-free; the render-loop case self-guards on a device
if(RIME_BUILD_CAPI)
    add_subdirectory(capi) # the C ABI (M6.9), driven through librime_capi — GPU-free
endif()

# The RHI and render-graph tests render through the Vulkan backend, so they only exist when it
# is built.
if(RIME_RHI_VULKAN)
    add_subdirectory(rhi)
    add_subdirectory(render) # the render graph (M5.4): pixel proofs on lavapipe
endif()
