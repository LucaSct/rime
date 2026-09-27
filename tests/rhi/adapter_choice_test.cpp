// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Pure-function tests for choose_adapter. No GPU, no create_device, no RIME_REQUIRE_VULKAN: this
// exercises the selection rules directly, so the tie rule and the explicit-preference rule are
// pinned without needing hardware. device_test.cpp provides doctest's main() for this executable.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "rime/rhi/adapter_choice.hpp"

namespace {

using rime::rhi::AdapterCandidate;
using rime::rhi::choose_adapter;

constexpr int kDiscrete = 1000;
constexpr int kIntegrated = 500;
constexpr int kCpu = 100;

} // namespace

TEST_CASE("choose_adapter: empty candidate list returns nullopt") {
    const std::vector<AdapterCandidate> empty;
    CHECK_FALSE(choose_adapter(empty, "").has_value());
}

TEST_CASE("choose_adapter: no preference prefers discrete over integrated") {
    const std::vector<AdapterCandidate> candidates = {
        {"Intel UHD Graphics", kIntegrated},
        {"NVIDIA GeForce RTX 3060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 1);
    CHECK_FALSE(choice->preference_matched);
    CHECK_FALSE(choice->preference_unmatched);
}

TEST_CASE("choose_adapter: two discrete GPUs, no preference -> first enumerated wins") {
    // The tie rule is first-enumerated-wins: with two equally-scored discrete GPUs and no
    // preference, we take the driver's enumeration order rather than a coin flip, so the choice is
    // deterministic on a given machine.
    const std::vector<AdapterCandidate> candidates = {
        {"NVIDIA GeForce GTX 1060", kDiscrete},
        {"NVIDIA GeForce RTX 3060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 0);
}

TEST_CASE("choose_adapter: preference '3060' selects the RTX 3060") {
    const std::vector<AdapterCandidate> candidates = {
        {"NVIDIA GeForce GTX 1060", kDiscrete},
        {"NVIDIA GeForce RTX 3060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "3060");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 1);
    CHECK(choice->preference_matched);
    CHECK_FALSE(choice->preference_unmatched);
}

TEST_CASE("choose_adapter: preference is case-insensitive") {
    const std::vector<AdapterCandidate> candidates = {
        {"NVIDIA GeForce GTX 1060", kDiscrete},
        {"NVIDIA GeForce RTX 3060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "gtx 1060");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 0);
    CHECK(choice->preference_matched);
}

TEST_CASE("choose_adapter: unmatched preference falls back to default and is flagged") {
    const std::vector<AdapterCandidate> candidates = {
        {"NVIDIA GeForce GTX 1060", kDiscrete},
        {"NVIDIA GeForce RTX 3060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "9090");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 0); // best-by-type, tie -> lowest index
    CHECK_FALSE(choice->preference_matched);
    CHECK(choice->preference_unmatched);
}

TEST_CASE("choose_adapter: explicit preference outranks type score") {
    // A preference beating a discrete GPU for a CPU device is deliberate, not an accident: "run
    // THIS on the CPU" is a stronger instruction than "prefer a discrete GPU".
    const std::vector<AdapterCandidate> candidates = {
        {"llvmpipe (LLVM 17)", kCpu},
        {"NVIDIA GeForce GTX 1060", kDiscrete},
    };
    const auto choice = choose_adapter(candidates, "llvmpipe");
    REQUIRE(choice.has_value());
    CHECK(choice->index == 0);
    CHECK(choice->preference_matched);
}
