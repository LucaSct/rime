// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <string_view>

// Thread utilities the C++ standard library does not provide.
//
// std::thread can start and join threads, but it has no notion of a thread *name* — yet a named
// thread is the difference between a profiler/debugger showing "rime-main" and an anonymous
// numeric TID. Naming is therefore one of the few genuinely per-OS primitives in M2.1: Win32
// uses SetThreadDescription, while macOS and Linux use pthread_setname_np (with different
// signatures and length limits, handled in the per-OS backends).
namespace rime::platform {

// Set the *current* thread's name, for debuggers and profilers. Best-effort: the name is
// silently truncated to the OS limit (15 chars on Linux, 63 on macOS) and the call is a no-op
// if the OS rejects it. Takes UTF-8; the Win32 backend widens it to UTF-16.
void set_thread_name(std::string_view name);

} // namespace rime::platform
