// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <pthread.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "rime/platform/threads.hpp"

// macOS thread naming. Darwin's pthread_setname_np names only the *calling* thread (note the
// single-argument signature — there is no thread-handle parameter as on Linux) and caps the
// name at 63 characters (MAXTHREADNAME - 1). We copy into a fixed, null-terminated buffer so a
// std::string_view — which is not guaranteed to be null-terminated — is safe to hand to the C API.
namespace rime::platform {

void set_thread_name(std::string_view name) {
    constexpr std::size_t kMaxLen = 63; // Darwin MAXTHREADNAME (64) minus the null terminator.
    std::array<char, kMaxLen + 1> buf{};
    const std::size_t n = std::min(name.size(), kMaxLen);
    std::memcpy(buf.data(), name.data(), n);
    buf[n] = '\0';
    pthread_setname_np(buf.data());
}

} // namespace rime::platform
