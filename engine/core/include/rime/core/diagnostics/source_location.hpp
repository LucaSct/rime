// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// A minimal stand-in for std::source_location.
//
// Why not std::source_location? It is C++20, but libc++ on Apple Clang 15 (one of our three
// CI compilers) does not ship <source_location>. Rather than #ifdef per compiler at every
// call site, we capture the call site ourselves with the classic preprocessor triplet
// wrapped in RIME_SRC_LOC. The fields point at string literals / the compiler's static
// __func__ storage, so a SourceLocation owns nothing and is trivially copyable.
namespace rime::core {

struct SourceLocation {
    const char* file = "";
    int line = 0;
    const char* function = "";
};

} // namespace rime::core

// Capture the current call site. This must be a macro, not a function: it has to expand at
// the point of use so __FILE__/__LINE__/__func__ refer to the caller. __func__ is a
// predefined identifier naming the enclosing function (only valid inside a function body,
// which is the only place these diagnostics are used).
#define RIME_SRC_LOC                                                                               \
    (::rime::core::SourceLocation{__FILE__, __LINE__, static_cast<const char*>(__func__)})
