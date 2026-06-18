# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# Turn a compiled SPIR-V binary into a C header that embeds it as a uint32_t array, so a target can
# #include the shader bytes directly with no runtime file I/O (ADR-0008 — offline shaders). Invoked
# by rime_add_shaders (see the top-level CMakeLists) as a build step:
#
#   cmake -DSPV=<in.spv> -DHDR=<out.h> -DSYM=<symbol> -P embed_spirv.cmake
#
# SPIR-V is a stream of 32-bit words stored little-endian in the .spv file. file(READ ... HEX) gives
# us the bytes in file order; we reassemble each little-endian word (b0 b1 b2 b3 -> 0xb3b2b1b0) so
# the emitted uint32_t values are correct regardless of the host's endianness when it later compiles
# the header.

file(READ "${SPV}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)

set(_words "")
set(_i 0)
set(_col 0)
while(_i LESS _hexlen)
    string(SUBSTRING "${_hex}" ${_i} 8 _w) # 8 hex chars = one 32-bit word, 4 bytes b0 b1 b2 b3
    string(SUBSTRING "${_w}" 0 2 _b0)
    string(SUBSTRING "${_w}" 2 2 _b1)
    string(SUBSTRING "${_w}" 4 2 _b2)
    string(SUBSTRING "${_w}" 6 2 _b3)
    string(APPEND _words "0x${_b3}${_b2}${_b1}${_b0}u,")
    math(EXPR _col "${_col} + 1")
    if(_col EQUAL 8)
        string(APPEND _words "\n    ")
        set(_col 0)
    else()
        string(APPEND _words " ")
    endif()
    math(EXPR _i "${_i} + 8")
endwhile()

set(_content "// SPDX-License-Identifier: Apache-2.0\n")
string(APPEND _content "// Copyright (c) 2026 The Rime Engine Authors.\n")
string(APPEND _content "// Generated from ${SPV} by cmake/embed_spirv.cmake. DO NOT EDIT.\n")
string(APPEND _content "#pragma once\n#include <cstdint>\n\n")
string(APPEND _content "static const uint32_t ${SYM}[] = {\n    ${_words}\n};\n")

file(WRITE "${HDR}" "${_content}")
