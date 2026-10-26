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

// An ACTIVE interaction profile must name its object and both server hooks; an inactive one
// (no spawn profile beside it) is inert and is not checked.
bool interaction_complete(const GameDefinition::RemotePlayProfile& remote_play) {
    const auto* interaction = remote_play.active_interaction();
    return interaction == nullptr ||
           (interaction->object && interaction->sample && interaction->capture);
}

// The opaque keys SeatTable admits on. A session key carries the SessionId's generation, so a
// recycled transport slot is a different key -- the same reason a PlayerSlot carries one.
std::uint64_t session_key(net::SessionId id) noexcept {
    return (std::uint64_t{id.index} << 32) | id.generation;
}

// Address AND port: one seat per source endpoint. The transport already keeps one session per
// endpoint (a reconnect from the same address:port REPLACES the old session, which releases its
// seat before the new hello can arrive), so the table's DuplicateEndpoint refusal is defence in
// depth here rather than the first line. Address alone would refuse every second client on one
// machine -- including the three-client localhost proof m20.8c needs.
std::uint64_t endpoint_key(const platform::Endpoint& peer) noexcept {
    return (std::uint64_t{peer.address} << 16) | peer.port;
}

const char* disconnect_reason_name(net::DisconnectReason reason) {
    // Every reason by its own name. Folding the rest into "timeout" made the server's OWN graceful
    // close (`LocalClose`, e.g. the end of --linger-ms) read as a dead client.
    switch (reason) {
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
}

} // namespace

// The default one-command input buffer plus the one send period a single lost datagram costs.
constexpr std::uint32_t kLoseOneSlack = 2;

// ── The pump, per seat (m20.8b, ADR-0108 §3) ─────────────────────────────────────────────────
//
// Until m20.8 everything here was single-instance: one admitted session, one schedule, one owner,
// one epoch, one outcome list. The move to N players is the PER-SEAT STATE ARRAY technique: every
// piece of state that belongs to a player lives in `Seat`, and `seats[slot.index]` finds it in
// O(1). The index comes from SeatTable (a generational slot map), so the slot a message, a spawn
// report or a hook names is checked against the seat's CURRENT generation before it may touch
// anything -- a late report for a departed player addresses nothing instead of the newcomer.
//
// The danger ADR-0108 names is SILENT CROSS-SEAT ALIASING: a field left single-instance while the
// rest went per-seat, so one player's input, epoch or verdicts leak into another's -- and since
// every client mirrors the same authority, digest agreement cannot see it. So the rule in this
// struct is: a member is either in `Seat`, or it carries a comment arguing why it is shared.
//
// One-seat (`max_players == 1`) is NOT a second code path: it is this one with one seat. The few
// places it differs are each marked `multi_seat` and exist only to keep m20.7's bytes.
struct NetworkInputServer::Impl {
    struct LiveSpawn {
        SpawnKey key{};
        ecs::Entity entity{};
        PredictedEntityId id{};
        std::uint64_t launch_tick = 0;
    };

    struct Seat {
        // ── The occupant: reset when the seat is released ────────────────────────────────────
        std::optional<net::SessionId> session; // set while seated, pending or admitted
        PlayerSlot slot{};                     // the slot the occupant was issued
        // False while a multi-seat join waits for the game's `on_player_joined` (the hello reply
        // is held until then). A one-seat admission is immediate, as in m20.7.
        bool admitted = false;
        std::optional<InputSchedule> schedule;
        // Outcomes name one incarnation only, so they die with the occupant: a reconnect cannot
        // acknowledge or bind the former peer's launches even if it reuses input sequences.
        std::vector<LiveSpawn> live_spawns;
        std::vector<SpawnOutcome> pending_outcomes;
        std::optional<SpawnKey> acknowledged_through;
        std::optional<SpawnKey> newest_outcome;
        // The seat's controlled body and the prediction hooks bound to it. One-seat: bound once
        // at construction from `owned_player` and kept for every occupant (hello-game's one
        // player exists before anyone connects). Multi-seat: bound after `on_player_joined` from
        // `seat_player`, cleared at release (the departed player's body is the game's to remove).
        ecs::Entity owner{};
        NetId owner_net_id{};
        PredictionHooks hooks;
        // ── Per INDEX: survives occupants ─────────────────────────────────────────────────────
        // Monotonic per index, deliberately: a newcomer's client starts a fresh ledger and does
        // not care, and m20.7's one seat never reset its epoch across reconnects, so keeping it
        // is what keeps a one-seat reconnect's checkpoint bytes unchanged.
        std::uint32_t membership_epoch = 0;
        // The interaction membership leaf for THIS seat's player (membership is "is this player
        // near the object", so two players are two policies). One-seat: built once and kept, as
        // m20.6b did. Multi-seat: rebuilt for each occupant, whose body is a different one.
        std::optional<InteractionMembershipPolicy> policy;
        RunReport::NetworkInputCounters::SeatCounters counts{};
    };

    static std::uint16_t seat_capacity(SeatConfig config) noexcept {
        if (config.max_players <= 1)
            return 1;
        return std::clamp<std::uint16_t>(
            std::min(config.max_clients, config.max_players), 1, SeatTable::kMaxSeatsLimit);
    }

    Impl(ListenerLink supplied_link,
         double tick_hz,
         const GameDefinition::NetworkInputProfile& profile,
         ecs::World& world,
         const GameDefinition::RemotePlayProfile* remote_play,
         SpawnFault fault,
         std::uint32_t fault_ticks,
         std::uint32_t input_slack,
         bool adaptive_input,
         SeatConfig seat_config)
        : link(std::move(supplied_link)), driver(link, driver_config(seat_capacity(seat_config))),
          profile(profile), world(world), remote_play(remote_play), fault(fault),
          fault_ticks(fault_ticks), input_slack(input_slack), adaptive_input(adaptive_input),
          multi_seat(seat_config.max_players > 1),
          neutral_after_ticks(static_cast<std::uint32_t>(std::max(1.0, std::ceil(tick_hz * 0.1)))),
          // A one-seat table never rate-limits (window 0): one seat cannot be hoarded, and m20.7
          // admitted every reconnect. Multi-seat: `join_burst` admissions per second of ticks.
          table(SeatTableParams{
              .max_seats = seat_capacity(seat_config),
              .join_burst = 4,
              .join_window_ticks =
                  multi_seat ? static_cast<std::uint32_t>(std::max(1.0, std::ceil(tick_hz)))
                             : 0u}) {
        driver.listen();
        schema_hash = input_schema_identity(profile, remote_play, seat_config.max_players);
        server_codec = multi_seat ? kInputCodecV3 : kInputCodecV2;
        seats.resize(table.max_seats());
        seat_results.resize(seats.size());
        for (std::size_t i = 0; i < seats.size(); ++i)
            seats[i].counts.index = static_cast<std::uint16_t>(i);
        if (remote_play && remote_play->spawn_prediction)
            spawn_profile = &*remote_play->spawn_prediction;
        if (remote_play)
            interaction = remote_play->active_interaction();
        if (remote_play) {
            if (!multi_seat) {
                seats[0].owner = remote_play->owned_player(world);
                seats[0].hooks = remote_play->prediction_hooks(world, seats[0].owner);
            }
            replicator = std::make_unique<ServerReplicator>(world);
            // Dedicated setup has finished and presentation has not run: this enumerates the
            // simulated entities whose structure a joining client needs.
            world.query<ecs::WorldTransform>().for_each(
                [&](ecs::Entity entity, const ecs::WorldTransform&) {
                    if (replicator->replicate(entity) == kNullNetId)
                        ++totals.replication_registration_refused;
                });
            if (!multi_seat) {
                if (const auto mapped = replicator->map().net_id_of(seats[0].owner))
                    seats[0].owner_net_id = *mapped;
            }
            if (interaction && interaction->object) {
                object = interaction->object(world);
                if (const auto mapped = replicator->map().net_id_of(object))
                    object_id = PredictedEntityId{mapped->index, mapped->generation};
                if (!multi_seat)
                    seats[0].policy.emplace(interaction->params);
            }
            replicator->set_budget(Budget{.max_bytes_per_tick = 1024});
        }
    }

    static net::NetDriver::Config driver_config(std::uint16_t seat_count) {
        net::NetDriver::Config config{};
        // One transport session per seat plus one, so a peer beyond the last seat can still
        // complete the connect and receive an explicit profile-level `full` refusal (m20.3's
        // "2 sessions for 1 input source", generalised).
        config.max_sessions = static_cast<std::size_t>(seat_count) + 1;
        // Keep the transport's schema identity neutral. InputHello owns the game input schema;
        // rejecting it at transport connect would prevent a counted `schema` hello refusal.
        return config;
    }

    // Folds an ending schedule into the totals AND into its seat index's own record, so a one-seat
    // run prints m20.7's totals and a multi-seat run can still say which seat did what.
    void save_schedule(Seat& seat) {
        if (!seat.schedule)
            return;
        const InputSchedule& schedule = *seat.schedule;
        totals.accepted += schedule.accepted();
        totals.late += schedule.late();
        totals.future += schedule.future();
        totals.duplicate += schedule.duplicate();
        totals.applied += schedule.applied();
        totals.synthesized += schedule.synthesized();
        totals.neutralized += schedule.neutralized();
        totals.stalls += schedule.stalls();
        totals.gaps_retired += schedule.gaps_retired();
        totals.late_missed += schedule.late_missed();
        totals.late_unclassified += schedule.late_unclassified();
        totals.max_margin = std::max<std::uint64_t>(totals.max_margin, schedule.max_margin());
        totals.final_margin = schedule.margin();
        seat.counts.accepted += schedule.accepted();
        seat.counts.applied += schedule.applied();
        seat.counts.synthesized += schedule.synthesized();
        seat.counts.late += schedule.late();
        seat.counts.stalls += schedule.stalls();
        seat.schedule.reset();
    }

    // The ADMITTED seat a session holds, or null (no seat, or a join still waiting for the game).
    [[nodiscard]] Seat* admitted_seat(net::SessionId id) noexcept {
        const auto slot = table.slot_of(session_key(id));
        if (!slot)
            return nullptr;
        Seat& seat = seats[slot->index];
        return seat.admitted ? &seat : nullptr;
    }

    void defer_refused_close(net::SessionId id) {
        // A reliable reply queued immediately before disconnect was not reaching the refused
        // client once world publication shared this driver. Keep its bounded transport slot
        // briefly so it can deliver the refusal first. A list, not one slot: several refused
        // peers can be in flight at once on a multi-seat server.
        const bool already = std::any_of(refused_close.begin(),
                                         refused_close.end(),
                                         [&](const auto& entry) { return entry.first == id; });
        if (!already) {
            refused_close.emplace_back(id, now + 250);
            ++totals.refusal_close_deferred;
        }
    }

    void send_hello_reply(net::Session& session, InputHelloReply reply) {
        std::array<std::byte, kInputHelloReplyV3Bytes> out{};
        const auto n = encode_input_hello_reply(reply, out);
        (void)session.send_reliable(std::span(out).first(n), now);
    }

    [[nodiscard]] InputHelloReply accepted_reply(const Seat& seat) const noexcept {
        InputHelloReply reply{};
        reply.codec = server_codec;
        if (multi_seat) {
            // Codec 3: tell the client which seat it holds. One-seat keeps the 3-byte reply.
            reply.seated = true;
            reply.seat = seat.slot.index;
            reply.seat_generation = seat.slot.generation;
        }
        return reply;
    }

    // The seat is now the occupant's. One-seat runs this straight from the hello (m20.7's order
    // and events); multi-seat runs it once the game has accepted the join.
    void admit_seat(Seat& seat, net::Session& session) {
        seat.admitted = true;
        if (replicator) {
            net::SessionEvent joined{};
            joined.kind = net::SessionEvent::Kind::Connected;
            joined.id = *seat.session;
            replicator->on_session_events(std::vector<net::SessionEvent>{joined});
            ++seat.membership_epoch;
        }
        ++totals.clients_admitted;
        ++seat.counts.joins;
        if (multi_seat)
            event(fmt::format(
                "{{\"event\":\"client_connected\",\"peer\":\"{}:{}\",\"seat\":{},\"gen\":{}}}",
                session.peer().address_string(),
                session.peer().port,
                seat.slot.index,
                seat.slot.generation));
        else
            event(fmt::format("{{\"event\":\"client_connected\",\"peer\":\"{}:{}\"}}",
                              session.peer().address_string(),
                              session.peer().port));
    }

    void make_schedule(Seat& seat) {
        // The leaf's six-tick default is 100 ms only at 60 Hz. Derive the silence bound from
        // this game's fixed rate before its first command. `lose-one` drops one datagram, so the
        // launch command's next copy -- the redundant window of the client's NEXT send -- arrives
        // one send period (one client tick) late. A one-command buffer has already retired it as
        // synthesized by then (measured: retired_without_launch 1), which would test synthesis,
        // not redundancy. One more buffered command absorbs exactly that one period; the fault leg
        // alone pays the tick.
        seat.schedule.emplace(
            std::max(input_slack, fault == SpawnFault::LoseOne ? kLoseOneSlack : 1u),
            neutral_after_ticks,
            adaptive_input ? InputBufferMode::Adaptive : InputBufferMode::Fixed);
    }

    void handle_hello(net::SessionId id,
                      net::Session& session,
                      const net::Received& message,
                      std::span<const std::byte> bytes) {
        if (message.channel != net::Channel::ReliableOrdered) {
            ++totals.hello_malformed;
            return;
        }
        InputHello hello{};
        if (!decode_input_hello(bytes, hello)) {
            ++totals.hello_malformed;
            return;
        }
        // Version and schema first, exactly as m20.3 ordered them: a client that cannot speak to
        // this server must learn that, not "full", even when every seat is taken.
        InputHelloReply reply = select_input_codec(hello, schema_hash, false, server_codec);
        if (reply.refusal == InputHelloRefusal::Version)
            ++totals.hello_version;
        if (reply.refusal == InputHelloRefusal::Schema)
            ++totals.hello_schema;
        Seat* seat = nullptr;
        bool newly_seated = false;
        if (reply.refusal == InputHelloRefusal::None) {
            const SeatAdmission admission =
                table.admit(session_key(id), endpoint_key(session.peer()), admission_tick);
            const auto refuse = [&](InputHelloRefusal specific, std::uint64_t& counter) {
                // One seat answers every seat refusal as m20.3's `Full`: a codec-2 client cannot
                // decode the newer codes, and with one seat "no seat for you" is the whole story.
                reply = InputHelloReply{};
                reply.refusal = multi_seat ? specific : InputHelloRefusal::Full;
                ++counter;
                ++totals.clients_refused;
            };
            switch (admission.refusal) {
                case SeatRefusal::Full:
                    refuse(InputHelloRefusal::Full, totals.hello_full);
                    break;
                case SeatRefusal::DuplicateEndpoint:
                    refuse(InputHelloRefusal::DuplicateEndpoint, totals.duplicate_endpoint);
                    break;
                case SeatRefusal::RateLimited:
                    refuse(InputHelloRefusal::RateLimited, totals.join_rate_limited);
                    break;
                case SeatRefusal::None:
                    seat = &seats[admission.slot.index];
                    newly_seated = !admission.already_seated;
                    if (newly_seated) {
                        seat->session = id;
                        seat->slot = admission.slot;
                        seat->admitted = false;
                    }
                    reply = accepted_reply(*seat);
                    break;
            }
        }
        // A multi-seat join is answered by the GAME (begin_seat_tick); until then -- and for a
        // re-sent hello while it waits -- nothing is sent. Everything else is answered now: a
        // refusal, a one-seat acceptance, or a re-sent hello from a seated client (harmless).
        const bool awaiting_game = seat != nullptr && multi_seat && !seat->admitted;
        if (!awaiting_game)
            send_hello_reply(session, reply);
        if (reply.refusal != InputHelloRefusal::None)
            defer_refused_close(id);
        if (seat != nullptr && newly_seated) {
            make_schedule(*seat);
            if (!multi_seat)
                admit_seat(*seat, session);
        }
    }

    // A session ended. If it held a seat, release it (generation + 1) and drop everything the
    // occupant owned; a multi-seat departure is queued for the game's `on_player_left`.
    void handle_departure(const net::SessionEvent& e) {
        const auto slot = table.slot_of(session_key(e.id));
        if (!slot)
            return;
        Seat& seat = seats[slot->index];
        if (seat.admitted) {
            if (replicator) {
                replicator->on_session_events(std::vector<net::SessionEvent>{e});
                ++totals.replication_clients_released;
            }
            ++totals.clients_left;
            ++seat.counts.leaves;
            const char* reason = disconnect_reason_name(e.reason);
            if (multi_seat)
                event(fmt::format(
                    "{{\"event\":\"client_left\",\"reason\":\"{}\",\"seat\":{},\"gen\":{}}}",
                    reason,
                    slot->index,
                    slot->generation));
            else
                event(fmt::format("{{\"event\":\"client_left\",\"reason\":\"{}\"}}", reason));
            if (multi_seat)
                left_slots.push_back(*slot);
        } else {
            // Seated by the table but never admitted: the join was still waiting for the game.
            ++totals.seats_abandoned;
        }
        save_schedule(seat);
        seat.pending_outcomes.clear();
        seat.live_spawns.clear();
        seat.acknowledged_through.reset();
        seat.newest_outcome.reset();
        seat.session.reset();
        seat.admitted = false;
        if (multi_seat)
            release_owner(seat);
        (void)table.release(session_key(e.id));
    }

    void release_owner(Seat& seat) {
        seat.owner = {};
        seat.owner_net_id = {};
        seat.hooks = {};
        if (seat.policy) {
            // Keep the departed occupant's policy counts in the run's totals.
            const auto& c = seat.policy->counters();
            policy_totals.joins += c.joins;
            policy_totals.leaves += c.leaves;
            policy_totals.joined_in_contact += c.joined_in_contact;
            policy_totals.leave_deferred_moving += c.leave_deferred_moving;
            policy_totals.leave_deferred_touching += c.leave_deferred_touching;
            policy_totals.leave_deferred_quiet += c.leave_deferred_quiet;
            seat.policy.reset();
        }
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
        std::erase_if(refused_close, [&](const auto& entry) {
            if (now < entry.second)
                return false;
            if (auto* refused = driver.session(entry.first))
                refused->disconnect();
            return true;
        });
        for (const auto& e : events) {
            if (e.kind == net::SessionEvent::Kind::Disconnected)
                handle_departure(e);
        }
        for (const auto id : driver.session_ids()) {
            net::Session* session = driver.session(id);
            if (!session)
                continue;
            messages.clear();
            session->drain_received(messages);
            if (Seat* seat = admitted_seat(id); replicator && seat) {
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
                    handle_hello(id, *session, message, bytes);
                    continue;
                }
                // Looked up per message: a one-seat hello earlier in this batch admits the
                // session for the messages after it, as m20.7's per-message check did.
                Seat* seat = admitted_seat(id);
                if (seat == nullptr) {
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
                    handle_spawn_ack(*seat, *session, message, bytes);
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
                if (drop_for_fault(*seat))
                    continue;
                // THE routing line of this file: a command reaches the schedule of the seat its
                // SESSION holds, and no other. The anti-aliasing test sends distinct axes on two
                // seats and checks each lands on its own PlayerInput.
                seat->schedule->offer(commands);
            }
        }
    }

    void handle_spawn_ack(Seat& seat,
                          const net::Session& session,
                          const net::Received& message,
                          std::span<const std::byte> bytes) {
        if (message.channel != net::Channel::ReliableOrdered) {
            ++totals.spawn_ack_wrong_channel;
            return;
        }
        SpawnOutcomeAck ack{};
        if (!decode_spawn_outcome_ack(bytes, ack)) {
            ++totals.spawn_ack_malformed;
            return;
        }
        if (ack.through.incarnation != session.incarnation()) {
            ++totals.spawn_ack_wrong_incarnation;
            return;
        }
        if (!seat.newest_outcome || ack.through > *seat.newest_outcome) {
            ++totals.spawn_ack_ahead;
            return;
        }
        if (seat.acknowledged_through && ack.through <= *seat.acknowledged_through) {
            ++totals.spawn_ack_stale;
            return;
        }
        if (duplicate_window_open()) {
            // Validated, then treated as not yet arrived: what a lost-then-resent ack looks like
            // to the client. after_tick applies it when the window ends.
            fault_held_ack = ack.through;
            return;
        }
        apply_ack(seat, ack.through);
    }

    // Faults cut a decoded command window, after transport and codec admission. One-seat only
    // (bind refuses a fault on a multi-seat server), so the fault state below is never shared
    // between two players.
    [[nodiscard]] bool drop_for_fault(Seat& seat) {
        if (fault != SpawnFault::LoseOne && fault != SpawnFault::LoseAll)
            return false;
        for (const auto& command : commands)
            if (command.pressed != 0 && !fault_sequence)
                fault_sequence = command.sequence;
        const bool carries_launch =
            fault_sequence && std::any_of(commands.begin(), commands.end(), [&](const auto& c) {
                return c.sequence == *fault_sequence;
            });
        const auto result = seat.schedule->result(0);
        const bool retired =
            fault_sequence && result.any_retired && result.retired_through >= *fault_sequence;
        if (carries_launch && !retired &&
            (fault == SpawnFault::LoseAll || totals.spawn_fault_lose_one == 0)) {
            if (fault == SpawnFault::LoseOne)
                ++totals.spawn_fault_lose_one;
            else
                ++totals.spawn_fault_lose_all;
            return true;
        }
        return false;
    }

    // ── Configuration and shared machinery: one per server, argued ────────────────────────────
    ListenerLink link;     // one socket serves every seat
    net::NetDriver driver; // one transport, one session per peer (sessions are not seats)
    GameDefinition::NetworkInputProfile profile;          // config
    ecs::World& world;                                    // the one authoritative world
    const GameDefinition::RemotePlayProfile* remote_play; // config
    // Process-proof faults: ONE-SEAT DIAGNOSTICS. `bind` refuses a fault when the game seats more
    // than one player, so this state -- which does name one client's sequence, ack and epoch --
    // can never be shared between two.
    SpawnFault fault = SpawnFault::None;
    std::uint32_t fault_ticks = 0;
    // Commands the server buffers before executing one (InputSchedule's slack). 1 is m20.3's
    // contract; `--input-slack` raises it for a run whose client cannot keep up with one tick of
    // slack (a starved CI VM). Config: every seat's schedule is built with it.
    std::uint32_t input_slack = 1;
    // m20.7 (ADR-0107): `dedicated --input-buffer adaptive`. Config, per schedule.
    bool adaptive_input = false;
    bool multi_seat = false;
    std::optional<std::uint32_t> fault_sequence;
    // `duplicate` holds the client's ack for its window, so the verdict rides every checkpoint in
    // it -- a repeated confirmation the client must recognise -- and applies it afterwards.
    std::optional<SpawnKey> fault_held_ack;
    std::optional<std::uint64_t> fault_launch_tick;
    std::uint32_t fault_prior_epoch = 0;
    // Clocks: one server, one tick, one millisecond clock.
    std::uint64_t current_server_tick = 0;
    std::uint64_t admission_tick = 0; // the tick the current pump admits at (the rate window)
    std::uint64_t now = 0;
    std::uint64_t schema_hash = 0; // config: one identity per server
    std::uint8_t server_codec = kInputCodecV2;
    std::uint32_t neutral_after_ticks = 6;
    // Refused sessions whose transport slot is held briefly to deliver the refusal. Per SESSION,
    // not per seat: a refused peer never had a seat.
    std::vector<std::pair<net::SessionId, std::uint64_t>> refused_close;
    // ONE replicator for every client: it is already per client inside (`ClientState` per
    // SessionId holds each client's baseline and relevancy), so the world stream needs no seat.
    std::unique_ptr<ServerReplicator> replicator;
    const GameDefinition::RemotePlayProfile::SpawnPredictionProfile* spawn_profile = nullptr;
    // m20.6b: the active interaction profile (ADR-0106 §2), its one object and that object's
    // roster id -- the OBJECT is one thing in one world, so these are shared; whether a given
    // player predicts it is that seat's `policy`. SERVER-DECLARED MEMBERSHIP WITH HYSTERESIS:
    // the client never decides what it predicts; it learns it from the roster and the epoch.
    const GameDefinition::RemotePlayProfile::InteractionProfile* interaction = nullptr;
    ecs::Entity object{};
    PredictedEntityId object_id{};
    // The last tick the policies ran. Shared because every seat's policy steps together, once per
    // tick: `linger` re-offers the final tick's checkpoint many times, and counting each re-offer
    // as another quiet tick would let the object leave during the drain.
    std::optional<std::uint64_t> interaction_tick;
    // Interaction epoch advances when a spawn fault started hiding its answer (one-seat fault).
    std::uint64_t fault_prior_interaction_epochs = 0;
    // m20.6c: server ticks at which the interaction object is kicked (one-seat diagnostic).
    std::vector<std::uint64_t> kick_ticks;
    // Policy counts of occupants that have left (multi-seat; a one-seat policy never leaves).
    InteractionCounters policy_totals{};

    // ── The seats ─────────────────────────────────────────────────────────────────────────────
    SeatTable table;
    std::vector<Seat> seats; // indexed by PlayerSlot::index; sized to the table
    // Seats vacated since the last begin_seat_tick, for the game's `on_player_left` (multi-seat).
    std::vector<PlayerSlot> left_slots;
    // This tick's per-seat inputs, ascending index (what TickContext::players points at).
    std::vector<PlayerInput> players;
    // The ONE-SEAT view of this tick's selection, kept for m20.7's accessors (`last_applied`,
    // `refuse_spawn`, `command_provenance`) and so `ctx.input`/`command`/`refuse_spawn` keep their
    // values. Neutral on a multi-seat server, where `players` is the only answer.
    std::optional<CommandProvenance> selected_provenance;
    bool last_applied = false;
    bool selected_refuse = false;

    RunReport::NetworkInputCounters totals{}; // run totals: the per-seat records sum into these
    // Scratch buffers, cleared before every use; nothing survives from one session to the next.
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;
    std::vector<InputCommand> commands;

    [[nodiscard]] bool duplicate_window_open() const noexcept {
        return fault == SpawnFault::Duplicate && fault_launch_tick &&
               current_server_tick < *fault_launch_tick + fault_ticks;
    }

    void apply_ack(Seat& seat, const SpawnKey& through) {
        // A frontier is a claim of APPLIED verdicts, not of sent packets. Only the client's
        // reliable ack can retire durable outcomes (ADR-0105 guardrail 5) -- and only that
        // client's own seat's outcomes.
        seat.acknowledged_through = through;
        std::erase_if(seat.pending_outcomes,
                      [&](const SpawnOutcome& outcome) { return outcome.key <= through; });
        ++totals.spawn_acks_applied;
    }

    // One seat's command for this tick. `applied` reports whether the client's own command ran,
    // independently of whether a session is still there to name its provenance.
    [[nodiscard]] PlayerInput step_seat(Seat& seat, bool& applied) {
        PlayerInput input{};
        input.slot = seat.slot;
        const ScheduledInput selected = seat.schedule->step();
        applied = selected.applied;
        if (const auto* session = driver.session(*seat.session))
            input.command = CommandProvenance{
                session->incarnation(), selected.command.sequence, selected.applied};
        input.actions = profile.to_actions(selected.command);
        if (fault_sequence && selected.applied && selected.command.sequence == *fault_sequence &&
            fault == SpawnFault::LoseOne && totals.spawn_fault_lose_one > 0)
            ++totals.launch_recovered_by_redundancy;
        if (fault == SpawnFault::Refuse && selected.applied && selected.command.pressed != 0 &&
            totals.spawn_fault_refuse == 0) {
            input.refuse_spawn = true;
            ++totals.spawn_fault_refuse;
        }
        return input;
    }

    // before_tick's answer. One-seat: select now (m20.7's order). Multi-seat: the selection waits
    // for begin_seat_tick, after the join/leave hooks, so last tick's `players` must not linger
    // into this one; the single-player answer is neutral.
    [[nodiscard]] ActionState begin_selection() {
        if (!multi_seat)
            return select_actions();
        players.clear();
        return {};
    }

    // One-seat selection, in before_tick (m20.7's order).
    [[nodiscard]] ActionState select_actions() {
        last_applied = false;
        selected_refuse = false;
        selected_provenance.reset();
        players.clear();
        Seat& seat = seats[0];
        if (!seat.admitted || !seat.schedule) {
            ++totals.synthesized;
            return {};
        }
        const PlayerInput input = step_seat(seat, last_applied);
        selected_refuse = input.refuse_spawn;
        selected_provenance = input.command;
        players.push_back(input);
        return input.actions;
    }

    // Binds the body the game spawned for a just-accepted seat. False if the game named none, or
    // one this server cannot checkpoint (the join is then refused, counted).
    [[nodiscard]] bool bind_owner(Seat& seat) {
        if (!remote_play)
            return true; // m20.3-style input only: nothing to checkpoint
        const ecs::Entity owner =
            remote_play->seat_player ? remote_play->seat_player(world, seat.slot) : ecs::Entity{};
        if (!world.is_alive(owner))
            return false;
        const NetId net_id = replicator->replicate(owner);
        if (net_id == kNullNetId)
            return false;
        PredictionHooks hooks = remote_play->prediction_hooks(world, owner);
        if (!hooks.capture || !hooks.restore || !hooks.capture_group || !hooks.restore_group ||
            !hooks.simulate)
            return false;
        seat.owner = owner;
        seat.owner_net_id = net_id;
        seat.hooks = std::move(hooks);
        if (interaction && interaction->object)
            seat.policy.emplace(interaction->params);
        return true;
    }

    // Multi-seat start of tick: leaves, then joins (each in ascending index), then every
    // admitted seat's command.
    void run_seat_changes(Game& game, TickContext& ctx) {
        std::sort(left_slots.begin(), left_slots.end(), [](PlayerSlot a, PlayerSlot b) {
            return a.index < b.index;
        });
        for (const PlayerSlot slot : left_slots)
            game.on_player_left(ctx, slot);
        left_slots.clear();
        for (Seat& seat : seats) {
            if (!seat.session || seat.admitted)
                continue;
            net::Session* session = driver.session(*seat.session);
            bool accepted = session != nullptr && game.on_player_joined(ctx, seat.slot);
            if (accepted && !bind_owner(seat)) {
                // The game took the player but gave the server nothing to predict for it: undo
                // the game's side so no orphan body remains, then refuse like the game did.
                ++totals.seat_owner_refused;
                game.on_player_left(ctx, seat.slot);
                accepted = false;
            }
            if (accepted) {
                admit_seat(seat, *session);
                send_hello_reply(*session, accepted_reply(seat));
                continue;
            }
            ++totals.game_refused_join;
            ++totals.clients_refused;
            const net::SessionId id = *seat.session;
            if (session != nullptr) {
                InputHelloReply refusal{};
                refusal.refusal = InputHelloRefusal::GameRefused;
                send_hello_reply(*session, refusal);
                defer_refused_close(id);
            }
            seat.schedule.reset();
            seat.session.reset();
            release_owner(seat);
            (void)table.release(session_key(id));
        }
        players.clear();
        for (Seat& seat : seats) {
            bool applied = false;
            if (seat.admitted && seat.schedule)
                players.push_back(step_seat(seat, applied));
        }
        // The tick nobody drove, counted as the one-seat path counts it.
        if (players.empty())
            ++totals.synthesized;
    }

    // Which seat a spawn report belongs to. One-seat: the one seat, whatever the report says
    // (one-player games never set `owner`, and the seat's generation moves on every reconnect).
    // Multi-seat: the report's owner, and only while that slot is current -- a report for a
    // departed player must not reach whoever sits in its index now.
    [[nodiscard]] Seat* spawn_seat(const SpawnReport& report) noexcept {
        if (!multi_seat)
            return seats[0].admitted ? &seats[0] : nullptr;
        if (!table.valid(report.owner))
            return nullptr;
        Seat& seat = seats[report.owner.index];
        return seat.admitted ? &seat : nullptr;
    }

    // One policy step per seat per server tick, from the state the tick just produced. A join or
    // a leave changes the roster that seat's next checkpoint carries, so it advances THAT seat's
    // epoch -- the server's statement that the set changed -- as an accepted or ended spawn does.
    void update_interaction(std::uint64_t server_tick) {
        if (!interaction || (interaction_tick && *interaction_tick == server_tick))
            return;
        bool any = false;
        for (Seat& seat : seats) {
            if (!seat.policy)
                continue;
            any = true;
            const auto sample = interaction->sample(world, seat.owner, object);
            const auto decision = seat.policy->update(sample);
            // A join taken while the object still moves: the sliding re-join the proof schedules.
            // The leaf cannot tell this apart from a resting join, and the proof must be able to.
            if (decision == MembershipDecision::Join &&
                sample.speed > seat.policy->params().rest_speed)
                ++totals.interaction_joined_sliding;
            if (decision != MembershipDecision::Stay) {
                ++seat.membership_epoch;
                ++totals.interaction_epoch_advances;
            }
            if (seat.policy->member())
                ++totals.interaction_member_ticks;
        }
        if (any)
            interaction_tick = server_tick;
    }

    void receive_spawns(std::span<const SpawnReport> reports) {
        if (!spawn_profile)
            return;
        for (const SpawnReport& report : reports) {
            if (!fault_launch_tick && report.kind == SpawnReport::Kind::Launched) {
                fault_launch_tick = current_server_tick;
                fault_prior_epoch = seats[0].membership_epoch;
                fault_prior_interaction_epochs = totals.interaction_epoch_advances;
            }
            Seat* seat = spawn_seat(report);
            if (seat == nullptr) {
                ++totals.spawn_without_client;
                continue;
            }
            const auto* session = driver.session(*seat->session);
            if (!session || report.key.incarnation != session->incarnation()) {
                ++totals.spawn_foreign_key;
                continue;
            }
            if (report.kind == SpawnReport::Kind::Ended) {
                const auto live = std::find_if(
                    seat->live_spawns.begin(), seat->live_spawns.end(), [&](const LiveSpawn& e) {
                        return e.key == report.key && e.entity == report.entity;
                    });
                if (live == seat->live_spawns.end()) {
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
                        seat->pending_outcomes.begin(),
                        seat->pending_outcomes.end(),
                        [&](const SpawnOutcome& entry) { return entry.key == report.key; });
                    outcome != seat->pending_outcomes.end()) {
                    outcome->kind = SpawnOutcomeKind::AcceptedEnded;
                    outcome->id = {};
                    ++totals.spawn_accepted_ended;
                }
                // Flight length is the game-agnostic trace of HOW it ended: a fixed-life
                // projectile that ran its full life missed, one that ended sooner hit.
                totals.spawn_last_flight_ticks = current_server_tick - live->launch_tick;
                replicator->despawn(live->entity);
                seat->live_spawns.erase(live);
                ++seat->membership_epoch;
                ++totals.spawn_epoch_advances;
                continue;
            }
            if (seat->newest_outcome && report.key <= *seat->newest_outcome) {
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
                seat->live_spawns.push_back(
                    {report.key, report.entity, outcome.id, current_server_tick});
                ++seat->membership_epoch;
                ++totals.spawn_epoch_advances;
                ++totals.spawn_accepted;
            }
            seat->newest_outcome = report.key;
            // The wire has a 16-verdict limit, not the durable store: refusing or forgetting
            // a game-allowed launch because of transport pressure would lie to the client.
            seat->pending_outcomes.push_back(outcome);
        }
    }

    // The seat's owner checkpoint: its own player, its own spawns, its own interaction
    // membership and group state -- never another seat's. Returns early (that seat only) on any
    // refusal, each counted.
    void send_checkpoint(Seat& seat,
                         net::Session& session,
                         const InputResult& result,
                         std::uint64_t server_tick) {
        OwnerCheckpoint checkpoint{};
        checkpoint.server_tick = server_tick;
        checkpoint.membership_epoch = seat.membership_epoch;
        checkpoint.result = result;
        // The client resolves this replicated NetId through its map to find its own mirror.
        const PredictedEntityId id{seat.owner_net_id.index, seat.owner_net_id.generation};
        auto state = seat.hooks.capture(id);
        if (state.empty()) {
            ++totals.checkpoint_capture_refused;
            return;
        }
        checkpoint.entities.push_back(PredictedEntityState{id, std::move(state)});
        if (seat.policy && seat.policy->member()) {
            // The member's FULL state, velocity included: the client seeds its prediction physics
            // from these bytes on the join tick and on every correction, never from the mirror.
            auto object_state = interaction->capture(world, object);
            if (object_state.empty()) {
                ++totals.interaction_capture_refused;
                ++totals.checkpoint_capture_refused;
                return;
            }
            checkpoint.entities.push_back(PredictedEntityState{object_id, std::move(object_state)});
        }
        SpawnCheckpointExtension extension{};
        if (spawn_profile) {
            // A delayed answer is redacted as a coherent older roster. Publishing the new entity
            // without its binding would make two visible identities for one key on the client.
            const bool hide_spawn =
                fault_launch_tick &&
                (fault == SpawnFault::LateAlive || fault == SpawnFault::LateEnded) &&
                server_tick < *fault_launch_tick + fault_ticks;
            if (hide_spawn) {
                // The redacted roster still carries the interaction object's CURRENT membership,
                // so its epoch moves on by every interaction change since the fault began. Known
                // limit (diagnostic only): if the object joined or left inside the window, that
                // number may also name a roster the client saw before, and the client counts the
                // mismatch.
                checkpoint.membership_epoch =
                    fault_prior_epoch +
                    static_cast<std::uint32_t>(totals.interaction_epoch_advances -
                                               fault_prior_interaction_epochs);
                if (fault == SpawnFault::LateAlive)
                    ++totals.spawn_fault_late_alive;
                else
                    ++totals.spawn_fault_late_ended;
            }
            extension.acknowledged_through = seat.acknowledged_through;
            for (const auto& live : seat.live_spawns) {
                if (hide_spawn)
                    continue;
                const auto spawned_state = spawn_profile->capture_spawned(world, live.entity);
                if (spawned_state.empty()) {
                    ++totals.checkpoint_capture_refused;
                    return;
                }
                checkpoint.entities.push_back({live.id, spawned_state});
                extension.bindings.push_back({live.key, live.id});
            }
            if (!hide_spawn) {
                const auto count =
                    std::min(seat.pending_outcomes.size(), kSpawnExtensionMaxOutcomes);
                extension.outcomes.assign(seat.pending_outcomes.begin(),
                                          seat.pending_outcomes.begin() +
                                              static_cast<std::ptrdiff_t>(count));
                totals.spawn_outcomes_deferred += seat.pending_outcomes.size() - count;
            }
        }
        checkpoint.group_state = seat.hooks.capture_group();
        std::array<std::byte, kCheckpointMaxMessageBytes> checkpoint_bytes{};
        std::size_t encoded_size = 0;
        if (spawn_profile) {
            const auto encoded = encode_spawn_checkpoint(checkpoint, extension, checkpoint_bytes);
            if (!encoded)
                ++totals.spawn_extension_refused;
            else
                encoded_size = encoded.bytes_written;
        } else {
            const auto encoded = encode_owner_checkpoint(checkpoint, checkpoint_bytes);
            if (encoded)
                encoded_size = encoded.bytes_written;
        }
        if (encoded_size == 0) {
            ++totals.checkpoint_encode_refused;
        } else if (!session.send_unreliable(std::span(checkpoint_bytes).first(encoded_size),
                                            now,
                                            kRemoteOwnerCheckpointStream)) {
            ++totals.checkpoint_send_refused;
        } else {
            ++totals.checkpoint_encoded;
            ++seat.counts.checkpoints;
            if (fault == SpawnFault::Duplicate && fault_launch_tick &&
                server_tick >= *fault_launch_tick &&
                server_tick < *fault_launch_tick + fault_ticks) {
                if (session.send_unreliable(std::span(checkpoint_bytes).first(encoded_size),
                                            now,
                                            kRemoteOwnerCheckpointStream))
                    ++totals.spawn_fault_duplicate;
            }
        }
    }

    void after_tick(std::uint64_t server_tick,
                    std::span<const SpawnReport> spawns,
                    std::span<const ecs::Entity> despawns) {
        current_server_tick = server_tick;
        // Game-ended entities go BEFORE publication, so the retraction rides this tick's records
        // instead of the orphan backstop repairing it a tick late. Through the replicator when
        // there is one (it retracts the NetId first), else a plain world despawn.
        for (const ecs::Entity entity : despawns) {
            if (!world.is_alive(entity)) {
                ++totals.game_despawns_dead;
                continue;
            }
            if (replicator)
                replicator->despawn(entity);
            else
                world.despawn(entity);
            ++totals.game_despawns;
        }
        receive_spawns(spawns);
        update_interaction(server_tick);
        if (fault_held_ack && !duplicate_window_open()) {
            apply_ack(seats[0], *fault_held_ack);
            fault_held_ack.reset();
        }
        // Per seat: the input result, then (adaptive) the pacing report. Results are stashed so
        // each seat's checkpoint below carries ITS result.
        bool any_sent = false;
        for (Seat& seat : seats) {
            if (!seat.admitted || !seat.schedule)
                continue;
            net::Session* session = driver.session(*seat.session);
            if (!session) {
                ++totals.publish_missing_session;
                continue;
            }
            any_sent = true;
            std::array<std::byte, kInputResultBytes> bytes{};
            seat_results[seat.slot.index] = seat.schedule->result(server_tick);
            const auto n = encode_input_result(seat_results[seat.slot.index], bytes);
            if (!session->send_unreliable(std::span(bytes).first(n), now, kRemoteInputResultStream))
                ++totals.input_result_send_refused;
            if (adaptive_input) {
                // After the result, on its own stream: the client's pacer reads the margin AFTER
                // this tick's step. Unreliable like the result -- the next report replaces a lost
                // one.
                std::array<std::byte, kInputPacingBytes> pacing_bytes{};
                const auto pn =
                    encode_input_pacing(seat.schedule->pacing(server_tick), pacing_bytes);
                if (session->send_unreliable(
                        std::span(pacing_bytes).first(pn), now, kRemoteInputPacingStream))
                    ++totals.pacing_sent;
                else
                    ++totals.pacing_send_refused;
            }
        }
        if (!any_sent || !replicator)
            return;
        // ONE publication for every client (the replicator is per-client inside), between the
        // results and the checkpoints -- m20.7's order, which one seat reproduces exactly.
        replicator->publish(driver, now);
        for (Seat& seat : seats) {
            if (!seat.admitted || !seat.schedule)
                continue;
            if (net::Session* session = driver.session(*seat.session))
                send_checkpoint(seat, *session, seat_results[seat.slot.index], server_tick);
        }
    }

    // Each seat's result from this after_tick, so its checkpoint carries ITS result (indexed by
    // seat, sized with `seats`).
    std::vector<InputResult> seat_results;
};

NetworkInputServer::NetworkInputServer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NetworkInputServer::~NetworkInputServer() = default;

namespace {

// What both factories check before building the pump. One-seat: m20.4's checks, unchanged.
// Multi-seat: the game must name each seat's body (`seat_player`), and there is no player to
// check yet -- bodies arrive with `on_player_joined` and are checked there.
const char* remote_play_incomplete(const GameDefinition::RemotePlayProfile* remote_play,
                                   SeatConfig seats) {
    if (!remote_play)
        return nullptr;
    if (!remote_play->owned_player || !remote_play->prediction_hooks ||
        (remote_play->spawn_prediction && !remote_play->spawn_prediction->capture_spawned) ||
        !interaction_complete(*remote_play))
        return "remote play profile is incomplete";
    if (seats.max_players > 1 && !remote_play->seat_player)
        return "a multi-seat remote play profile needs seat_player";
    return nullptr;
}

} // namespace

std::unique_ptr<NetworkInputServer>
NetworkInputServer::bind(const std::string& host,
                         std::uint16_t port,
                         double tick_hz,
                         const GameDefinition::NetworkInputProfile& profile,
                         ecs::World& world,
                         const GameDefinition::RemotePlayProfile* remote_play,
                         std::string& error,
                         SpawnFault fault,
                         std::uint32_t fault_ticks,
                         std::uint32_t input_slack,
                         bool adaptive_input,
                         SeatConfig seats) {
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
    if (const char* incomplete = remote_play_incomplete(remote_play, seats)) {
        error = incomplete;
        return nullptr;
    }
    // The fault state names ONE client's sequence, ack and epoch; on a multi-seat server it would
    // be shared between players. A diagnostic must not be the aliasing bug it exists to find.
    if (seats.max_players > 1 && fault != SpawnFault::None) {
        error = "spawn faults are one-seat diagnostics";
        return nullptr;
    }
    const bool multi = seats.max_players > 1;
    if (remote_play && !multi && !world.is_alive(remote_play->owned_player(world))) {
        error = "owned player is not alive";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(ListenerLink{std::move(*socket)},
                                       tick_hz,
                                       profile,
                                       world,
                                       remote_play,
                                       fault,
                                       fault_ticks,
                                       input_slack,
                                       adaptive_input,
                                       seats);
    const auto& seat0 = impl->seats[0];
    if (remote_play && !multi &&
        (seat0.owner_net_id == kNullNetId || !seat0.hooks.capture || !seat0.hooks.restore ||
         !seat0.hooks.capture_group || !seat0.hooks.restore_group || !seat0.hooks.simulate)) {
        error = "remote play prediction hooks are incomplete";
        return nullptr;
    }
    if (impl->interaction && impl->object_id == PredictedEntityId{}) {
        error = "interaction object is not replicated";
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
                            std::string& error,
                            SeatConfig seats,
                            bool adaptive_input) {
    const bool multi = seats.max_players > 1;
    if (remote_play_incomplete(remote_play, seats) != nullptr ||
        (remote_play && !multi && !world.is_alive(remote_play->owned_player(world)))) {
        error = "remote play profile has no live owned player";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(ListenerLink{link},
                                       tick_hz,
                                       profile,
                                       world,
                                       remote_play,
                                       SpawnFault::None,
                                       0,
                                       1,
                                       adaptive_input,
                                       seats);
    const auto& seat0 = impl->seats[0];
    if (remote_play && !multi &&
        (seat0.owner_net_id == kNullNetId || !seat0.hooks.capture || !seat0.hooks.restore ||
         !seat0.hooks.capture_group || !seat0.hooks.restore_group || !seat0.hooks.simulate)) {
        error = "remote play prediction hooks are incomplete";
        return nullptr;
    }
    if (impl->interaction && impl->object_id == PredictedEntityId{}) {
        error = "interaction object is not replicated";
        return nullptr;
    }
    return std::unique_ptr<NetworkInputServer>(new NetworkInputServer(std::move(impl)));
}

std::uint16_t NetworkInputServer::local_port() const noexcept {
    return impl_->link.port();
}

ActionState NetworkInputServer::before_tick(std::uint64_t server_tick) {
    impl_->admission_tick = server_tick;
    impl_->pump();
    return impl_->begin_selection();
}

ActionState NetworkInputServer::before_tick(std::uint64_t server_tick, std::uint64_t now_ms) {
    impl_->admission_tick = server_tick;
    impl_->pump(now_ms);
    return impl_->begin_selection();
}

void NetworkInputServer::begin_seat_tick(Game& game, TickContext& ctx) {
    if (impl_->multi_seat)
        impl_->run_seat_changes(game, ctx);
    ctx.players = impl_->players;
}

std::span<const PlayerInput> NetworkInputServer::players() const noexcept {
    return impl_->players;
}

bool NetworkInputServer::last_applied() const noexcept {
    return impl_->last_applied;
}

bool NetworkInputServer::refuse_spawn() const noexcept {
    return impl_->selected_refuse;
}

void NetworkInputServer::set_interaction_kicks(std::vector<std::uint64_t> ticks) {
    impl_->kick_ticks = std::move(ticks);
}

bool NetworkInputServer::interaction_kick(std::uint64_t server_tick) {
    // A one-seat diagnostic: "was the object a member" is a question about one player.
    const auto& policy = impl_->seats[0].policy;
    if (!impl_->interaction || impl_->multi_seat || !policy ||
        std::find(impl_->kick_ticks.begin(), impl_->kick_ticks.end(), server_tick) ==
            impl_->kick_ticks.end())
        return false;
    ++impl_->totals.interaction_kicks;
    // Membership as of the previous tick's policy step: the state the client's roster reflects.
    ++(policy->member() ? impl_->totals.interaction_kicks_member
                        : impl_->totals.interaction_kicks_nonmember);
    return true;
}

std::optional<CommandProvenance> NetworkInputServer::command_provenance() const noexcept {
    return impl_->selected_provenance;
}

void NetworkInputServer::after_tick(std::uint64_t server_tick,
                                    std::span<const SpawnReport> spawns,
                                    std::span<const ecs::Entity> despawns) {
    impl_->after_tick(server_tick, spawns, despawns);
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
    for (const auto& seat : impl_->seats) {
        if (!seat.session)
            continue;
        if (auto* session = impl_->driver.session(*seat.session))
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
    // Live schedules add in, in ascending seat index; `final_margin` is therefore the highest
    // seated index's margin (with one seat: the one seat's, as m20.7 reported).
    for (const auto& seat : impl_->seats) {
        auto record = seat.counts;
        record.generation = seat.slot.generation;
        record.seated = seat.admitted;
        record.membership_epoch = seat.membership_epoch;
        if (seat.schedule) {
            const InputSchedule& schedule = *seat.schedule;
            counts.accepted += schedule.accepted();
            counts.late += schedule.late();
            counts.future += schedule.future();
            counts.duplicate += schedule.duplicate();
            counts.applied += schedule.applied();
            counts.synthesized += schedule.synthesized();
            counts.neutralized += schedule.neutralized();
            counts.stalls += schedule.stalls();
            counts.gaps_retired += schedule.gaps_retired();
            counts.late_missed += schedule.late_missed();
            counts.late_unclassified += schedule.late_unclassified();
            counts.max_margin = std::max<std::uint64_t>(counts.max_margin, schedule.max_margin());
            counts.final_margin = schedule.margin();
            record.accepted += schedule.accepted();
            record.applied += schedule.applied();
            record.synthesized += schedule.synthesized();
            record.late += schedule.late();
            record.stalls += schedule.stalls();
        }
        counts.seats.push_back(record);
    }
    counts.max_clients = impl_->seats.size();
    counts.input_buffer_adaptive = impl_->adaptive_input;
    if (impl_->interaction) {
        auto policy = impl_->policy_totals;
        bool any_policy = false;
        for (const auto& seat : impl_->seats) {
            if (!seat.policy)
                continue;
            any_policy = true;
            const auto& c = seat.policy->counters();
            policy.joins += c.joins;
            policy.leaves += c.leaves;
            policy.joined_in_contact += c.joined_in_contact;
            policy.leave_deferred_moving += c.leave_deferred_moving;
            policy.leave_deferred_touching += c.leave_deferred_touching;
            policy.leave_deferred_quiet += c.leave_deferred_quiet;
        }
        if (any_policy || impl_->multi_seat) {
            counts.interaction_joins = policy.joins;
            counts.interaction_leaves = policy.leaves;
            counts.interaction_joined_in_contact = policy.joined_in_contact;
            counts.interaction_leave_deferred_moving = policy.leave_deferred_moving;
            counts.interaction_leave_deferred_touching = policy.leave_deferred_touching;
            counts.interaction_leave_deferred_quiet = policy.leave_deferred_quiet;
        }
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
