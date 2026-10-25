// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/app/network_input.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/platform/socket.hpp"
#include "rime/replication/input_v2.hpp"
#include "rime/replication/server_replicator.hpp"
#include "rime/replication/spawn_prediction.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#endif

namespace rime::app {
namespace {

using namespace replication;

// UdpLink currently only exposes a bind() that enables SO_REUSEADDR. Its socket constructor is
// private, so this Link adapter owns the no-reuse socket in production and forwards to a
// caller-owned ScriptedNetwork link in the deterministic proof. Keep at most one poll's endpoints:
// anonymous addresses must not grow a permanent allocation.
class ListenerLink final : public net::Link {
public:
    explicit ListenerLink(platform::UdpSocket socket) : socket_(std::move(socket)) {}

    explicit ListenerLink(net::Link& external) : external_(&external) {}

    // NO AMPLIFICATION (ADR-0103 §6), and only where it applies. A peer with no session yet is
    // anonymous: it may be a spoofed source address, so nothing we send it may be larger than the
    // datagram it sent us in this poll -- otherwise a 25-byte forged request buys a 28-byte reply
    // aimed at a victim (measured: the driver's connect REJECTION is 28 bytes). A peer that holds a
    // session completed the connect exchange and is exempt; holding its retransmits and acks to its
    // last datagram's size would starve the very session the server admitted.
    bool send(const platform::Endpoint& to, std::span<const std::byte> bytes) override {
        // ScriptedNetwork owns every endpoint in the proof; anonymous amplification is a UDP
        // listener concern and remains guarded below on the production socket path.
        if (external_)
            return external_->send(to, bytes);
        if (std::find(established_.begin(), established_.end(), to) == established_.end()) {
            std::optional<std::size_t> smallest;
            for (const auto& [peer, length] : received_sizes_) {
                if (peer == to && (!smallest || length < *smallest)) {
                    smallest = length;
                }
            }
            // The driver may reject before allocating a session. Do not let that rejection turn
            // a tiny anonymous datagram into a larger reply; a retry needs a fresh request.
            if (!smallest || bytes.size() > *smallest) {
                ++amplification_blocked_;
                return false;
            }
        }
        return socket_->send_to(to, bytes).has_value();
    }

    std::size_t receive(std::vector<net::Datagram>& out) override {
        if (external_)
            return external_->receive(out);
        received_sizes_.clear();
        std::size_t count = 0;
        // Read the whole UDP datagram so oversize input is refused explicitly, not silently
        // truncated into a plausible 1200-byte packet by the receive buffer.
        std::array<std::byte, 65536> buffer{};
        // A fixed poll budget bounds anonymous work even when a peer floods the socket.
        while (count < 64) {
            platform::Endpoint from{};
            const auto n = socket_->recv_from(from, buffer);
            if (!n) {
                break;
            }
            if (*n > 1200) {
                ++oversized_;
                ++count;
                continue;
            }
            received_sizes_.emplace_back(from, *n);
            out.push_back(net::Datagram{from, {buffer.begin(), buffer.begin() + *n}});
            ++count;
        }
        return count;
    }

    // The peers that hold a session as of this pump. Refreshed before every driver update, so a
    // session the driver drops this tick loses its exemption at the next one.
    void set_established(std::vector<platform::Endpoint> peers) { established_ = std::move(peers); }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return socket_ ? socket_->local_port() : 0;
    }

    [[nodiscard]] std::uint64_t amplification_blocked() const noexcept {
        return amplification_blocked_;
    }

    [[nodiscard]] std::uint64_t oversized() const noexcept { return oversized_; }

private:
    std::optional<platform::UdpSocket> socket_;
    net::Link* external_ = nullptr;
    std::vector<platform::Endpoint> established_;
    std::vector<std::pair<platform::Endpoint, std::size_t>> received_sizes_;
    std::uint64_t amplification_blocked_ = 0;
    std::uint64_t oversized_ = 0;
};

void event(std::string_view line) {
    fmt::print("{}\n", line);
    std::fflush(stdout);
}

std::string error_name() {
#if defined(_WIN32)
    // Winsock keeps socket failures in its own error slot; name the equivalent errno so the
    // bind_failed event has the same vocabulary on every platform.
    switch (::WSAGetLastError()) {
        case WSAEADDRINUSE:
            return "EADDRINUSE";
        case WSAEADDRNOTAVAIL:
            return "EADDRNOTAVAIL";
        case WSAEACCES:
            return "EACCES";
        case WSAEINVAL:
            return "EINVAL";
        default:
            return "EUNKNOWN";
    }
#else
    const int code = errno;
    switch (code) {
        case EADDRINUSE:
            return "EADDRINUSE";
        case EADDRNOTAVAIL:
            return "EADDRNOTAVAIL";
        case EACCES:
            return "EACCES";
        case EINVAL:
            return "EINVAL";
        default:
            return "EUNKNOWN";
    }
#endif
}

void count_refusal(RunReport::NetworkInputCounters& counts, InputV2Refusal reason) {
    switch (reason) {
        case InputV2Refusal::None:
            break;
        case InputV2Refusal::Truncated:
            ++counts.truncated;
            break;
        case InputV2Refusal::BadLength:
            ++counts.bad_length;
            break;
        case InputV2Refusal::BadCount:
            ++counts.bad_count;
            break;
        case InputV2Refusal::UnknownCodec:
            ++counts.unknown_codec;
            break;
        case InputV2Refusal::ReservedNonZero:
            ++counts.reserved_nonzero;
            break;
        case InputV2Refusal::NonFinite:
            ++counts.non_finite;
            break;
        case InputV2Refusal::SequenceOrder:
            ++counts.sequence_order;
            break;
    }
}

} // namespace

struct NetworkInputServer::Impl {
    Impl(ListenerLink supplied_link,
         double tick_hz,
         const GameDefinition::NetworkInputProfile& profile,
         ecs::World& world,
         const GameDefinition::RemotePlayProfile* remote_play)
        : link(std::move(supplied_link)), driver(link, driver_config()), profile(profile),
          world(world),
          neutral_after_ticks(static_cast<std::uint32_t>(std::max(1.0, std::ceil(tick_hz * 0.1)))) {
        driver.listen();
        schema_hash = input_schema_identity(profile, remote_play);
        if (remote_play && remote_play->spawn_prediction)
            spawn_profile = &*remote_play->spawn_prediction;
        if (remote_play) {
            owner = remote_play->owned_player(world);
            hooks = remote_play->prediction_hooks(world, owner);
            replicator = std::make_unique<ServerReplicator>(world);
            // Dedicated setup has finished and presentation has not run: this enumerates the
            // simulated entities whose structure a joining client needs.
            world.query<ecs::WorldTransform>().for_each(
                [&](ecs::Entity entity, const ecs::WorldTransform&) {
                    if (replicator->replicate(entity) == kNullNetId)
                        ++totals.replication_registration_refused;
                });
            if (const auto mapped = replicator->map().net_id_of(owner))
                owner_net_id = *mapped;
            replicator->set_budget(Budget{.max_bytes_per_tick = 1024});
        }
    }

    static net::NetDriver::Config driver_config() {
        net::NetDriver::Config config{};
        // Two transport sessions let a second peer receive an explicit profile-level `full`
        // refusal while the simulation still admits exactly one input source.
        config.max_sessions = 2;
        // Keep the transport's schema identity neutral. InputHello owns the game input schema;
        // rejecting it at transport connect would prevent a counted `schema` hello refusal.
        return config;
    }

    void save_schedule() {
        if (!schedule)
            return;
        totals.accepted += schedule->accepted();
        totals.late += schedule->late();
        totals.future += schedule->future();
        totals.duplicate += schedule->duplicate();
        totals.applied += schedule->applied();
        totals.synthesized += schedule->synthesized();
        totals.neutralized += schedule->neutralized();
        schedule.reset();
    }

    void pump(std::optional<std::uint64_t> clock_override = {}) {
        const auto now_ms = clock_override.value_or(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()));
        now = now_ms;
        events.clear();
        std::vector<platform::Endpoint> established;
        for (const auto id : driver.session_ids()) {
            if (const net::Session* session = driver.session(id))
                established.push_back(session->peer());
        }
        link.set_established(std::move(established));
        driver.update(now, events);
        if (refused_close && now >= refused_close->second) {
            if (auto* refused = driver.session(refused_close->first))
                refused->disconnect();
            refused_close.reset();
        }
        for (const auto& e : events) {
            if (e.kind == net::SessionEvent::Kind::Disconnected && admitted && e.id == *admitted) {
                if (replicator) {
                    replicator->on_session_events(std::vector<net::SessionEvent>{e});
                    ++totals.replication_clients_released;
                }
                ++totals.clients_left;
                // Every reason by its own name. Folding the rest into "timeout" made the server's
                // OWN graceful close (`LocalClose`, e.g. the end of --linger-ms) read as a dead
                // client.
                const auto reason = [&] {
                    switch (e.reason) {
                        case net::DisconnectReason::Graceful:
                            return "graceful";
                        case net::DisconnectReason::LocalClose:
                            return "server_closed";
                        case net::DisconnectReason::Timeout:
                            return "timeout";
                        case net::DisconnectReason::Replaced:
                            return "replaced";
                        case net::DisconnectReason::ConnectTimeout:
                            return "connect_timeout";
                        case net::DisconnectReason::ProtocolMismatch:
                            return "protocol_mismatch";
                        case net::DisconnectReason::AppMismatch:
                            return "app_mismatch";
                        case net::DisconnectReason::SchemaMismatch:
                            return "schema_mismatch";
                        case net::DisconnectReason::ServerFull:
                            return "server_full";
                    }
                    return "unknown";
                }();
                event(fmt::format("{{\"event\":\"client_left\",\"reason\":\"{}\"}}", reason));
                admitted.reset();
                save_schedule();
                // Outcomes name one incarnation only; a reconnect cannot acknowledge or bind
                // the former peer's launches even if it happens to reuse input sequences.
                pending_outcomes.clear();
                live_spawns.clear();
                acknowledged_through.reset();
                newest_outcome.reset();
            }
        }
        for (const auto id : driver.session_ids()) {
            net::Session* session = driver.session(id);
            if (!session)
                continue;
            messages.clear();
            session->drain_received(messages);
            if (replicator && admitted && id == *admitted) {
                std::size_t ack_tags = 0;
                for (const auto& message : messages)
                    if (!message.bytes.empty() &&
                        message.bytes[0] ==
                            std::byte{static_cast<std::uint8_t>(MessageTag::BaselineAck)})
                        ++ack_tags;
                const auto applied = replicator->apply_messages(id, messages);
                totals.replication_acks += applied;
                totals.replication_ack_refused += ack_tags - std::min(ack_tags, applied);
            }
            for (const auto& message : messages) {
                const auto bytes = std::span<const std::byte>(message.bytes);
                if (bytes.empty()) {
                    ++totals.bad_length;
                    continue;
                }
                if (bytes[0] == std::byte{kInputHelloTag}) {
                    if (message.channel != net::Channel::ReliableOrdered) {
                        ++totals.hello_malformed;
                        continue;
                    }
                    InputHello hello{};
                    if (!decode_input_hello(bytes, hello)) {
                        ++totals.hello_malformed;
                        continue;
                    }
                    const auto reply = select_input_codec(
                        hello, schema_hash, admitted.has_value() && id != *admitted);
                    if (reply.refusal == InputHelloRefusal::Version)
                        ++totals.hello_version;
                    if (reply.refusal == InputHelloRefusal::Schema)
                        ++totals.hello_schema;
                    if (reply.refusal == InputHelloRefusal::Full)
                        ++totals.clients_refused;
                    std::array<std::byte, kInputHelloReplyBytes> out{};
                    const auto n = encode_input_hello_reply(reply, out);
                    (void)session->send_reliable(std::span(out).first(n), now);
                    if (reply.refusal != InputHelloRefusal::None) {
                        // A reliable reply queued immediately before disconnect was not reaching
                        // the second client once world publication shared this driver. Keep the
                        // bounded transport slot briefly so it can deliver the refusal first.
                        if (!refused_close || refused_close->first != id) {
                            refused_close = std::pair{id, now + 250};
                            ++totals.refusal_close_deferred;
                        }
                    }
                    if (reply.refusal == InputHelloRefusal::None && !admitted) {
                        admitted = id;
                        if (replicator) {
                            net::SessionEvent joined{};
                            joined.kind = net::SessionEvent::Kind::Connected;
                            joined.id = id;
                            replicator->on_session_events(std::vector<net::SessionEvent>{joined});
                            ++membership_epoch;
                        }
                        // The leaf's six-tick default is 100 ms only at 60 Hz. Derive the
                        // silence bound from this game's fixed rate before its first command.
                        schedule.emplace(1, neutral_after_ticks);
                        ++totals.clients_admitted;
                        event(fmt::format("{{\"event\":\"client_connected\",\"peer\":\"{}:{}\"}}",
                                          session->peer().address_string(),
                                          session->peer().port));
                    }
                    continue;
                }
                if (!admitted || id != *admitted) {
                    ++totals.unadmitted_messages;
                    continue;
                }
                if (replicator &&
                    bytes[0] == std::byte{static_cast<std::uint8_t>(MessageTag::BaselineAck)}) {
                    if (message.channel != net::Channel::UnreliableSequenced)
                        ++totals.wrong_channel;
                    continue;
                }
                if (spawn_profile && bytes[0] == std::byte{kSpawnOutcomeAckTag}) {
                    if (message.channel != net::Channel::ReliableOrdered) {
                        ++totals.spawn_ack_wrong_channel;
                        continue;
                    }
                    SpawnOutcomeAck ack{};
                    if (!decode_spawn_outcome_ack(bytes, ack)) {
                        ++totals.spawn_ack_malformed;
                        continue;
                    }
                    if (ack.through.incarnation != session->incarnation()) {
                        ++totals.spawn_ack_wrong_incarnation;
                        continue;
                    }
                    if (!newest_outcome || ack.through > *newest_outcome) {
                        ++totals.spawn_ack_ahead;
                        continue;
                    }
                    if (acknowledged_through && ack.through <= *acknowledged_through) {
                        ++totals.spawn_ack_stale;
                        continue;
                    }
                    // A frontier is a claim of APPLIED verdicts, not of sent packets. Only this
                    // reliable message can retire durable outcomes (ADR-0105 guardrail 5).
                    acknowledged_through = ack.through;
                    std::erase_if(pending_outcomes, [&](const SpawnOutcome& outcome) {
                        return outcome.key <= ack.through;
                    });
                    ++totals.spawn_acks_applied;
                    continue;
                }
                if (message.channel != net::Channel::UnreliableSequenced) {
                    ++totals.wrong_channel;
                    continue;
                }
                if (bytes[0] != std::byte{kInputV2Tag}) {
                    ++totals.unknown_codec;
                    continue;
                }
                commands.clear();
                const auto decoded = decode_input_v2(bytes, commands);
                if (!decoded) {
                    count_refusal(totals, decoded.refusal);
                    continue;
                }
                schedule->offer(commands);
            }
        }
    }

    ListenerLink link;
    net::NetDriver driver;
    GameDefinition::NetworkInputProfile profile;
    ecs::World& world;
    std::uint64_t schema_hash = 0;
    std::uint32_t neutral_after_ticks = 6;
    std::optional<net::SessionId> admitted;
    std::optional<std::pair<net::SessionId, std::uint64_t>> refused_close;
    std::optional<InputSchedule> schedule;
    std::unique_ptr<ServerReplicator> replicator;
    PredictionHooks hooks;
    const GameDefinition::RemotePlayProfile::SpawnPredictionProfile* spawn_profile = nullptr;

    struct LiveSpawn {
        SpawnKey key{};
        ecs::Entity entity{};
        PredictedEntityId id{};
    };

    std::vector<LiveSpawn> live_spawns;
    std::vector<SpawnOutcome> pending_outcomes;
    std::optional<SpawnKey> acknowledged_through;
    std::optional<SpawnKey> newest_outcome;
    std::optional<CommandProvenance> selected_provenance;
    ecs::Entity owner{};
    NetId owner_net_id{};
    std::uint32_t membership_epoch = 0;
    RunReport::NetworkInputCounters totals{};
    std::uint64_t now = 0;
    bool last_applied = false;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;
    std::vector<InputCommand> commands;

    [[nodiscard]] ActionState select_actions() {
        last_applied = false;
        selected_provenance.reset();
        if (!schedule) {
            ++totals.synthesized;
            return {};
        }
        const ScheduledInput selected = schedule->step();
        last_applied = selected.applied;
        if (admitted) {
            if (const auto* session = driver.session(*admitted))
                selected_provenance = CommandProvenance{
                    session->incarnation(), selected.command.sequence, selected.applied};
        }
        return profile.to_actions(selected.command);
    }

    void receive_spawns(std::span<const SpawnReport> reports) {
        if (!spawn_profile)
            return;
        for (const SpawnReport& report : reports) {
            if (!admitted) {
                ++totals.spawn_without_client;
                continue;
            }
            const auto* session = driver.session(*admitted);
            if (!session || report.key.incarnation != session->incarnation()) {
                ++totals.spawn_foreign_key;
                continue;
            }
            if (report.kind == SpawnReport::Kind::Ended) {
                const auto live = std::find_if(
                    live_spawns.begin(), live_spawns.end(), [&](const LiveSpawn& entry) {
                        return entry.key == report.key && entry.entity == report.entity;
                    });
                if (live == live_spawns.end()) {
                    // A launch this pump never adopted (refused on roster grounds, or forgotten by
                    // a reconnect) still left an entity in the world, and the game hands its
                    // despawn to whoever listens for reports. Despawn it here, or it outlives its
                    // flight forever on the server.
                    ++totals.spawn_end_unknown;
                    if (world.is_alive(report.entity))
                        replicator->despawn(report.entity);
                    continue;
                }
                // End the world-stream identity before publication. If the verdict has not
                // been acknowledged, its Accepted becomes AcceptedEnded, with no stale binding.
                if (const auto outcome = std::find_if(
                        pending_outcomes.begin(),
                        pending_outcomes.end(),
                        [&](const SpawnOutcome& entry) { return entry.key == report.key; });
                    outcome != pending_outcomes.end()) {
                    outcome->kind = SpawnOutcomeKind::AcceptedEnded;
                    outcome->id = {};
                    ++totals.spawn_accepted_ended;
                }
                replicator->despawn(live->entity);
                live_spawns.erase(live);
                ++membership_epoch;
                ++totals.spawn_epoch_advances;
                continue;
            }
            if (newest_outcome && report.key <= *newest_outcome) {
                ++totals.spawn_out_of_order;
                continue;
            }
            SpawnOutcome outcome{};
            outcome.key = report.key;
            if (report.kind == SpawnReport::Kind::Refused) {
                outcome.kind = SpawnOutcomeKind::Rejected;
                outcome.reason = report.reason;
                ++totals.spawn_rejected;
            } else {
                const NetId net_id = replicator->replicate(report.entity);
                if (net_id == kNullNetId || (net_id.index & kProvisionalIndexBit) != 0) {
                    if (net_id != kNullNetId)
                        replicator->despawn(report.entity);
                    ++totals.spawn_roster_refused;
                    continue;
                }
                outcome.kind = SpawnOutcomeKind::Accepted;
                outcome.id = {net_id.index, net_id.generation};
                live_spawns.push_back({report.key, report.entity, outcome.id});
                ++membership_epoch;
                ++totals.spawn_epoch_advances;
                ++totals.spawn_accepted;
            }
            newest_outcome = report.key;
            // The wire has a 16-verdict limit, not the durable store: refusing or forgetting
            // a game-allowed launch because of transport pressure would lie to the client.
            pending_outcomes.push_back(outcome);
        }
    }
};

NetworkInputServer::NetworkInputServer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NetworkInputServer::~NetworkInputServer() = default;

std::unique_ptr<NetworkInputServer>
NetworkInputServer::bind(const std::string& host,
                         std::uint16_t port,
                         double tick_hz,
                         const GameDefinition::NetworkInputProfile& profile,
                         ecs::World& world,
                         const GameDefinition::RemotePlayProfile* remote_play,
                         std::string& error) {
    const auto local = platform::Endpoint::from_string(host, port);
    if (!local) {
        error = "EINVAL";
        return nullptr;
    }
    auto socket = platform::UdpSocket::bind(port, host, false);
    if (!socket) {
        error = error_name();
        return nullptr;
    }
    if (remote_play &&
        (!remote_play->owned_player || !remote_play->prediction_hooks ||
         (remote_play->spawn_prediction && !remote_play->spawn_prediction->capture_spawned))) {
        error = "remote play profile is incomplete";
        return nullptr;
    }
    if (remote_play && !world.is_alive(remote_play->owned_player(world))) {
        error = "owned player is not alive";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(
        ListenerLink{std::move(*socket)}, tick_hz, profile, world, remote_play);
    if (remote_play &&
        (impl->owner_net_id == kNullNetId || !impl->hooks.capture || !impl->hooks.restore ||
         !impl->hooks.capture_group || !impl->hooks.restore_group || !impl->hooks.simulate)) {
        error = "remote play prediction hooks are incomplete";
        return nullptr;
    }
    return std::unique_ptr<NetworkInputServer>(new NetworkInputServer(std::move(impl)));
}

std::unique_ptr<NetworkInputServer>
NetworkInputServer::on_link(net::Link& link,
                            double tick_hz,
                            const GameDefinition::NetworkInputProfile& profile,
                            ecs::World& world,
                            const GameDefinition::RemotePlayProfile* remote_play,
                            std::string& error) {
    if (remote_play &&
        (!remote_play->owned_player || !remote_play->prediction_hooks ||
         !world.is_alive(remote_play->owned_player(world)) ||
         (remote_play->spawn_prediction && !remote_play->spawn_prediction->capture_spawned))) {
        error = "remote play profile has no live owned player";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(ListenerLink{link}, tick_hz, profile, world, remote_play);
    if (remote_play &&
        (impl->owner_net_id == kNullNetId || !impl->hooks.capture || !impl->hooks.restore ||
         !impl->hooks.capture_group || !impl->hooks.restore_group || !impl->hooks.simulate)) {
        error = "remote play prediction hooks are incomplete";
        return nullptr;
    }
    return std::unique_ptr<NetworkInputServer>(new NetworkInputServer(std::move(impl)));
}

std::uint16_t NetworkInputServer::local_port() const noexcept {
    return impl_->link.port();
}

ActionState NetworkInputServer::before_tick(std::uint64_t) {
    impl_->pump();
    return impl_->select_actions();
}

ActionState NetworkInputServer::before_tick(std::uint64_t, std::uint64_t now_ms) {
    impl_->pump(now_ms);
    return impl_->select_actions();
}

bool NetworkInputServer::last_applied() const noexcept {
    return impl_->last_applied;
}

std::optional<CommandProvenance> NetworkInputServer::command_provenance() const noexcept {
    return impl_->selected_provenance;
}

void NetworkInputServer::after_tick(std::uint64_t server_tick,
                                    std::span<const SpawnReport> spawns) {
    impl_->receive_spawns(spawns);
    if (!impl_->admitted || !impl_->schedule)
        return;
    net::Session* session = impl_->driver.session(*impl_->admitted);
    if (!session) {
        ++impl_->totals.publish_missing_session;
        return;
    }
    std::array<std::byte, kInputResultBytes> bytes{};
    const InputResult result = impl_->schedule->result(server_tick);
    const auto n = encode_input_result(result, bytes);
    if (!session->send_unreliable(std::span(bytes).first(n), impl_->now, kRemoteInputResultStream))
        ++impl_->totals.input_result_send_refused;
    if (!impl_->replicator)
        return;
    impl_->replicator->publish(impl_->driver, impl_->now);
    OwnerCheckpoint checkpoint{};
    checkpoint.server_tick = server_tick;
    checkpoint.membership_epoch = impl_->membership_epoch;
    checkpoint.result = result;
    // The client resolves this replicated NetId through its map to find its own mirror.
    const PredictedEntityId id{impl_->owner_net_id.index, impl_->owner_net_id.generation};
    auto state = impl_->hooks.capture(id);
    if (state.empty()) {
        ++impl_->totals.checkpoint_capture_refused;
        return;
    }
    checkpoint.entities.push_back(PredictedEntityState{id, std::move(state)});
    SpawnCheckpointExtension extension{};
    if (impl_->spawn_profile) {
        extension.acknowledged_through = impl_->acknowledged_through;
        for (const auto& live : impl_->live_spawns) {
            const auto spawned_state =
                impl_->spawn_profile->capture_spawned(impl_->world, live.entity);
            if (spawned_state.empty()) {
                ++impl_->totals.checkpoint_capture_refused;
                return;
            }
            checkpoint.entities.push_back({live.id, spawned_state});
            extension.bindings.push_back({live.key, live.id});
        }
        const auto count = std::min(impl_->pending_outcomes.size(), kSpawnExtensionMaxOutcomes);
        extension.outcomes.assign(impl_->pending_outcomes.begin(),
                                  impl_->pending_outcomes.begin() + count);
        impl_->totals.spawn_outcomes_deferred += impl_->pending_outcomes.size() - count;
    }
    checkpoint.group_state = impl_->hooks.capture_group();
    std::array<std::byte, kCheckpointMaxMessageBytes> checkpoint_bytes{};
    std::size_t encoded_size = 0;
    if (impl_->spawn_profile) {
        const auto encoded = encode_spawn_checkpoint(checkpoint, extension, checkpoint_bytes);
        if (!encoded)
            ++impl_->totals.spawn_extension_refused;
        else
            encoded_size = encoded.bytes_written;
    } else {
        const auto encoded = encode_owner_checkpoint(checkpoint, checkpoint_bytes);
        if (encoded)
            encoded_size = encoded.bytes_written;
    }
    if (encoded_size == 0) {
        ++impl_->totals.checkpoint_encode_refused;
    } else if (!session->send_unreliable(std::span(checkpoint_bytes).first(encoded_size),
                                         impl_->now,
                                         kRemoteOwnerCheckpointStream)) {
        ++impl_->totals.checkpoint_send_refused;
    } else {
        ++impl_->totals.checkpoint_encoded;
    }
}

void NetworkInputServer::linger(std::uint64_t final_tick, std::uint32_t duration_ms) {
    // Authority is frozen here. Re-offer the named final snapshot while acks and reliable spawns
    // drain; another simulation tick would change the very digest the client must hold at close.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        impl_->pump();
        after_tick(final_tick);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (impl_->admitted) {
        if (auto* session = impl_->driver.session(*impl_->admitted))
            session->disconnect();
    }
    // Disconnect frames and their resends are emitted by driver.update, not disconnect().
    for (int i = 0; i < 5; ++i) {
        impl_->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

RunReport::NetworkInputCounters NetworkInputServer::counters() const noexcept {
    auto counts = impl_->totals;
    if (impl_->schedule) {
        counts.accepted += impl_->schedule->accepted();
        counts.late += impl_->schedule->late();
        counts.future += impl_->schedule->future();
        counts.duplicate += impl_->schedule->duplicate();
        counts.applied += impl_->schedule->applied();
        counts.synthesized += impl_->schedule->synthesized();
        counts.neutralized += impl_->schedule->neutralized();
    }
    counts.amplification_blocked = impl_->link.amplification_blocked();
    counts.driver_dropped = impl_->driver.datagrams_dropped();
    counts.connect_requests_seen = impl_->driver.connect_requests_seen();
    counts.oversized = impl_->link.oversized();
    if (impl_->replicator) {
        counts.replication_records_too_large = impl_->replicator->records_too_large();
        counts.replication_budget_deferred = impl_->replicator->entities_dropped_over_budget();
        counts.replication_orphans = impl_->replicator->net_ids_orphaned();
    }
    return counts;
}

} // namespace rime::app
