# The authoring round trip for target-range (m15.8), as a CMake script so it runs identically from
# CTest on every OS without a shell.
#
#   target_range --digest <shipped>      the digest before
#   editor --smoke --scene <copy>        open it, edit it, SAVE it (the editor's own host)
#   target_range --digest <copy>         the digest after      -- MUST DIFFER
#   target_range --scene <copy>          and the game must still WIN on the saved file
#
# Both clauses matter. A differing digest alone would be satisfied by an editor that corrupted the
# scene; a passing self-check alone would be satisfied by an editor that changed nothing. Together
# they say the editor made a real change and the result is still a playable range.
#
# Skips cleanly when the editor binary is absent: it is built by the Rust toolchain, and a --cpp-only
# build legitimately has no editor. A skip says so rather than passing silently.

if(NOT EXISTS "${EDITOR}")
    message(STATUS "target-range round trip: no editor at ${EDITOR} — skipping (Rust half not built)")
    return()
endif()

# The shipped scene is read-only input; the editor writes a SEPARATE file. Two paths rather than an
# in-place edit, so a failed run cannot leave the committed scene modified in the working tree.
set(edited "${WORKDIR}/target_range_edited.rscene")
file(REMOVE "${edited}")

function(digest_of path out)
    execute_process(COMMAND "${TARGET_RANGE}" --scene "${path}" --digest
                    OUTPUT_VARIABLE d RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "target_range --digest failed on ${path} (exit ${rc})")
    endif()
    set(${out} "${d}" PARENT_SCOPE)
endfunction()

digest_of("${SCENE}" before)

# The editor smoke drives a REAL engine process over the protocol (it is a client of the engine, not a
# standalone app — ADR-0016), so it needs the engine binary. Passing it explicitly rather than relying
# on RIME_ENGINE_BIN keeps the test self-contained.
if(NOT EXISTS "${ENGINE}")
    message(STATUS "target-range round trip: no engine at ${ENGINE} — skipping")
    return()
endif()

# `--save` is what makes the editor write the world back; without it the smoke edits in memory and
# the file on disk is untouched — which is how this test first failed, reporting "the digest did not
# change" when in truth nothing had been saved at all. The engine does the writing, not the editor
# (m14.3), which is the point: the game's own host serialises the world.
execute_process(COMMAND "${EDITOR}" --smoke --engine "${ENGINE}" --scene "${SCENE}" --save "${edited}"
                RESULT_VARIABLE editor_rc OUTPUT_VARIABLE editor_out ERROR_VARIABLE editor_out)
if(NOT editor_rc EQUAL 0)
    message(FATAL_ERROR "editor --smoke failed (exit ${editor_rc}):\n${editor_out}")
endif()

if(NOT EXISTS "${edited}")
    message(FATAL_ERROR "editor --smoke exited 0 but wrote no file at ${edited}")
endif()

digest_of("${edited}" after)

if(before STREQUAL after)
    message(FATAL_ERROR
            "the editor saved the scene but the placement digest did not change (${before}).\n"
            "Either the edit did not reach the file, or the game is not reading the file it was given "
            "— the two failures this test exists to tell apart.")
endif()

execute_process(COMMAND "${TARGET_RANGE}" --scene "${edited}" --verbose
                RESULT_VARIABLE play_rc OUTPUT_VARIABLE play_out ERROR_VARIABLE play_out)
if(NOT play_rc EQUAL 0)
    message(FATAL_ERROR "the edited scene does not play (exit ${play_rc}):\n${play_out}")
endif()

message(STATUS "target-range round trip: digest ${before} -> ${after}, edited scene still clears")
