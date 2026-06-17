// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// pthread_setname_np is a GNU extension, so _GNU_SOURCE must be defined before any libc header
// is pulled in. We compile with -std=c++20 (extensions off), under which the toolchain does not
// define it for us — hence this explicit define at the very top of the translation unit.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "rime/platform/threads.hpp"

// Linux thread naming. glibc's pthread_setname_np takes the target thread (unlike Darwin's) and
// caps the name at 16 bytes *including* the null terminator — so 15 usable characters. We name
// the calling thread (pthread_self) into a fixed, null-terminated buffer.
namespace rime::platform {

void set_thread_name(std::string_view name) {
    constexpr std::size_t kMaxLen = 15; // 16-byte limit (TASK_COMM_LEN) minus the null terminator.
    std::array<char, kMaxLen + 1> buf{};
    const std::size_t n = std::min(name.size(), kMaxLen);
    std::memcpy(buf.data(), name.data(), n);
    buf[n] = '\0';
    pthread_setname_np(pthread_self(), buf.data());
}

} // namespace rime::platform
