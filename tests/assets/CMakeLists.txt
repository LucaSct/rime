# Tests for rime::assets. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest, applies
# the build policy, and registers it with CTest; we add the module under test on top.
#
# The whole suite is GPU-free — the reader, registry, and manifest operate on in-memory buffers (and,
# for load_mesh, a temporary file) — so it builds and runs on every CI OS, including the ones with no
# Vulkan device. It is one executable; cooked_mesh_test.cpp supplies doctest's main().
rime_add_test(rime_assets_tests
    cooked_mesh_test.cpp
    manifest_test.cpp
    registry_test.cpp)

# rime::assets is the module under test. rime::platform is linked because registry_test writes a
# temporary cooked file through platform::write_file (the registry's own file I/O is already inside
# the assets library, PRIVATE).
target_link_libraries(rime_assets_tests
    PRIVATE rime::assets rime::platform)
