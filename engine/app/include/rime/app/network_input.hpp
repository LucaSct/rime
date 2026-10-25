// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rime/app/launch.hpp"
#include "rime/replication/snapshot.hpp"

namespace rime::net {
class Link;
}

namespace rime::app {

// Each unreliable message kind has its own supersession domain. The replication streams are
// named here as well so this host never relies on Session's default stream.
inline constexpr std::uint8_t kRemoteWorldStateStream = replication::kStreamDelta;
inline constexpr std::uint8_t kRemoteInputResultStream = replication::kStreamInputAck;
inline constexpr std::uint8_t kRemoteOwnerCheckpointStream = 2;
inline constexpr std::uint8_t kRemoteBaselineAckStream = replication::kStreamBaselineAck;
inline constexpr std::uint8_t kRemoteInputCommandsStream = replication::kStreamInputCommands;

// Main-thread pump. The runner calls it at the fixed-tick boundary, where applying actions is
// legal and where the job system has finished the work whose result the next tick will observe.
class NetworkInputServer {
public:
    static std::unique_ptr<NetworkInputServer>
    bind(const std::string& host,
         std::uint16_t port,
         double tick_hz,
         const GameDefinition::NetworkInputProfile& profile,
         ecs::World& world,
         const GameDefinition::RemotePlayProfile* remote_play,
         std::string& error,
         SpawnFault fault = SpawnFault::None,
         std::uint32_t fault_ticks = 0);
    // A caller-owned link and clock let deterministic tests drive the exact production pump.
    static std::unique_ptr<NetworkInputServer>
    on_link(net::Link& link,
            double tick_hz,
            const GameDefinition::NetworkInputProfile& profile,
            ecs::World& world,
            const GameDefinition::RemotePlayProfile* remote_play,
            std::string& error);
    ~NetworkInputServer();

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] ActionState before_tick(std::uint64_t server_tick);
    [[nodiscard]] ActionState before_tick(std::uint64_t server_tick, std::uint64_t now_ms);
    [[nodiscard]] bool last_applied() const noexcept;
    [[nodiscard]] bool refuse_spawn() const noexcept;
    // m20.6c `--interaction-fault kick@T`: the server ticks at which to kick the interaction
    // object. `interaction_kick(tick)` is asked once per tick before the game runs; it counts the
    // kick and records whether the object was a member at that moment.
    void set_interaction_kicks(std::vector<std::uint64_t> ticks);
    [[nodiscard]] bool interaction_kick(std::uint64_t server_tick);
    [[nodiscard]] std::optional<CommandProvenance> command_provenance() const noexcept;
    void after_tick(std::uint64_t server_tick, std::span<const SpawnReport> spawns = {});
    void linger(std::uint64_t final_tick, std::uint32_t duration_ms);
    [[nodiscard]] RunReport::NetworkInputCounters counters() const noexcept;

private:
    struct Impl;
    explicit NetworkInputServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::app
