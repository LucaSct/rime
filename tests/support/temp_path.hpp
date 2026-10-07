// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// temp_path(): a scratch-file path that no other process can have claimed.
//
// Why this exists. A test that writes `temp_directory_path() / "some_fixed_name"` is correct only
// while it is the sole writer of that name on the machine. Hosted CI never notices, because every
// run is a fresh VM. A persistent self-hosted runner, a developer with two accounts, or two
// concurrent `ctest` runs all do: /tmp is sticky, so a file left by another user cannot be opened
// for writing (fopen returns nullptr), and two instances of one binary can read each other's
// half-written file. Both failures were measured (see docs notes in the brick that added this).
//
// The idea. Mint ONE directory per process — <temp>/rime-<pid>-<random> — on first use, and put
// every scratch file under it. Per process rather than per call on purpose: a test that writes a
// file in one test case and reads it back in another still sees the same path. The pid names the
// owner for a human reading /tmp; the random suffix covers pid reuse after a crash left a stale
// directory behind. The directory is removed when the process exits normally (a function-local
// static's destructor); a crash leaves it behind, which is harmless because the name is never
// reused.
#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace rime::test {

namespace detail {

struct ProcessTempDir {
    std::filesystem::path dir;

    ProcessTempDir() {
#if defined(_WIN32)
        const auto pid = static_cast<unsigned long long>(_getpid());
#else
        const auto pid = static_cast<unsigned long long>(::getpid());
#endif
        std::random_device rd;
        dir = std::filesystem::temp_directory_path() /
              ("rime-" + std::to_string(pid) + "-" + std::to_string(rd()));
        // Throws on failure: a test with nowhere to write cannot meaningfully continue.
        std::filesystem::create_directories(dir);
    }

    ~ProcessTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    ProcessTempDir(const ProcessTempDir&) = delete;
    ProcessTempDir& operator=(const ProcessTempDir&) = delete;
};

} // namespace detail

// The per-process scratch directory (created on first use).
inline const std::filesystem::path& process_temp_dir() {
    static const detail::ProcessTempDir d;
    return d.dir;
}

// <process scratch dir>/<name>. `name` is a file or directory name; it is NOT created.
inline std::filesystem::path temp_path(std::string_view name) {
    return process_temp_dir() / std::filesystem::path(name);
}

} // namespace rime::test
