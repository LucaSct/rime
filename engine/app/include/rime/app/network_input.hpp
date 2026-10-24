// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "rime/app/launch.hpp"

namespace rime::app {

// Main-thread pump. The runner calls it at the fixed-tick boundary, where applying actions is
// legal and where the job system has finished the work whose result the next tick will observe.
class NetworkInputServer {
public:
    static std::unique_ptr<NetworkInputServer>
    bind(const std::string& host,
         std::uint16_t port,
         double tick_hz,
         const GameDefinition::NetworkInputProfile& profile,
         std::string& error);
    ~NetworkInputServer();

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] ActionState before_tick(std::uint64_t server_tick);
    void after_tick(std::uint64_t server_tick);
    [[nodiscard]] RunReport::NetworkInputCounters counters() const noexcept;

private:
    struct Impl;
    explicit NetworkInputServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::app
