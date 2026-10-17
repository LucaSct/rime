# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# A POST_BUILD companion identifies the bytes just linked. Export-time git HEAD may describe
# newer source than a --no-build executable; a binary hash binds this stamp to that executable.
# This is recorded build provenance, not a signature or a reproducible-build attestation.
if(NOT EXISTS "${GAME_BINARY}" OR NOT IS_DIRECTORY "${GAME_SOURCE}")
    message(FATAL_ERROR "game provenance: missing executable or source directory")
endif()
set(_revision "unknown")
set(_dirty "null")
if(GAME_GIT AND EXISTS "${GAME_GIT}")
    execute_process(COMMAND "${GAME_GIT}" -C "${GAME_SOURCE}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _head_result)
    execute_process(COMMAND "${GAME_GIT}" -C "${GAME_SOURCE}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE _changes RESULT_VARIABLE _status_result)
    if(_head_result EQUAL 0 AND _status_result EQUAL 0 AND _head MATCHES "^[0-9a-f]+$")
        set(_revision "${_head}")
        set(_dirty "false")
        if(NOT _changes STREQUAL "")
            set(_dirty "true")
        endif()
    endif()
endif()
file(SHA256 "${GAME_BINARY}" _binary_sha256)
set(_stamp "{\n  \"schema_version\": 1,\n  \"binary_sha256\": \"${_binary_sha256}\",\n  \"source_revision\": \"${_revision}\",\n  \"source_dirty\": ${_dirty}\n}\n")
file(WRITE "${GAME_BINARY}.provenance.json.tmp" "${_stamp}")
file(RENAME "${GAME_BINARY}.provenance.json.tmp" "${GAME_BINARY}.provenance.json")
