# Tests for rime::editorhost (M9 / m9.1). Reflection-driven world (de)serialization + edits, plus a
# loopback over a real ProtocolConnection on the s1.4 local socket — all GPU-free CPU + socket work,
# so it builds and runs on every CI OS.
rime_add_test(rime_editorhost_tests
    editor_host_test.cpp)

# rime::scene + rime::render arrive with m14.3: saving writes a `.rscene`, and the proof loads it
# back to show the data survived. render for a second reflected component to save alongside the
# transform — a one-component round trip proves less than it looks.
target_link_libraries(rime_editorhost_tests
    PRIVATE rime::editorhost rime::ecs rime::stream rime::platform rime::scene rime::render
            rime::core)
