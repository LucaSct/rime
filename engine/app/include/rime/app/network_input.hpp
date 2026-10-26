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
// m20.7: the adaptive buffer's pacing report. Streams 0 and 1 are the replication ones, 2 is the
// checkpoint; 3 is the next free (Session carries 16). Its own stream, because a newer pacing
// report says nothing about a checkpoint or an input result and must not supersede either.
inline constexpr std::uint8_t kRemoteInputPacingStream = 3;
inline constexpr std::uint8_t kRemoteBaselineAckStream = replication::kStreamBaselineAck;
inline constexpr std::uint8_t kRemoteInputCommandsStream = replication::kStreamInputCommands;

// m20.8b (ADR-0108 §5): how many players the server seats. `max_players` is the GAME's ceiling
// (GameDefinition::max_players); `max_clients` is the operator's, already resolved and checked by
// the launcher (1..min(128, max_players)). With `max_players == 1` the seat count is 1 whatever
// `max_clients` says, and the pump is m20.7's byte for byte.
struct SeatConfig {
    std::uint16_t max_players = 1;
    std::uint16_t max_clients = 1;
};

// Main-thread pump. The runner calls it at the fixed-tick boundary, where applying actions is
// legal and where the job system has finished the work whose result the next tick will observe.
//
// One tick, in order (launch.cpp's dedicated loop and the in-process tests do exactly this):
//   1. `before_tick`       -- pump the transport: hellos, seat admission, departures, commands.
//                             One-seat: also selects the one seat's command (m20.7's order).
//   2. `begin_seat_tick`   -- multi-seat: the game's leave and join hooks, then every seat's
//                             command, into `TickContext::players`. One-seat: only fills
//                             `players` with the entry step 1 selected.
//   3. the game's `fixed_tick`
//   4. `after_tick`        -- spawn routing, interaction policy, and per seat: input result,
//                             pacing, then one world publication, then per seat: owner checkpoint.
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
         std::uint32_t fault_ticks = 0,
         std::uint32_t input_slack = 1,
         bool adaptive_input = false,
         SeatConfig seats = {});
    // A caller-owned link and clock let deterministic tests drive the exact production pump.
    static std::unique_ptr<NetworkInputServer>
    on_link(net::Link& link,
            double tick_hz,
            const GameDefinition::NetworkInputProfile& profile,
            ecs::World& world,
            const GameDefinition::RemotePlayProfile* remote_play,
            std::string& error,
            SeatConfig seats = {},
            bool adaptive_input = false);
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
    // `kick@cmd:N`: fire once, on the first tick whose input step retires command sequence >= N.
    // A stall or a pre-start tick retires nothing, so it never fires; asked after `before_tick`.
    void set_interaction_kick_commands(std::vector<std::uint32_t> sequences);
    [[nodiscard]] bool interaction_kick(std::uint64_t server_tick);
    [[nodiscard]] std::optional<CommandProvenance> command_provenance() const noexcept;
    // Step 2 above. Sets `ctx.players` to a span this server owns (valid until the next
    // `before_tick`). Multi-seat: runs `game.on_player_left` / `on_player_joined` first and
    // answers each pending hello with the game's verdict.
    void begin_seat_tick(Game& game, TickContext& ctx);
    [[nodiscard]] std::span<const PlayerInput> players() const noexcept;
    void after_tick(std::uint64_t server_tick,
                    std::span<const SpawnReport> spawns = {},
                    std::span<const ecs::Entity> despawns = {});
    void linger(std::uint64_t final_tick, std::uint32_t duration_ms);
    [[nodiscard]] RunReport::NetworkInputCounters counters() const noexcept;

private:
    struct Impl;
    explicit NetworkInputServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::app
