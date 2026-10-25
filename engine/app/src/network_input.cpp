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
#include <utility>
#include <vector>

#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/platform/socket.hpp"
#include "rime/replication/input_v2.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#endif

namespace rime::app {
namespace {

using namespace replication;

// UdpLink currently only exposes a bind() that enables SO_REUSEADDR. Its socket constructor is
// private, so this small Link adapter owns the no-reuse socket until that seam can be widened.
// Keep at most one poll's endpoints: anonymous addresses must not grow a permanent allocation.
class ListenerLink final : public net::Link {
public:
    explicit ListenerLink(platform::UdpSocket socket) : socket_(std::move(socket)) {}

    // NO AMPLIFICATION (ADR-0103 §6), and only where it applies. A peer with no session yet is
    // anonymous: it may be a spoofed source address, so nothing we send it may be larger than the
    // datagram it sent us in this poll -- otherwise a 25-byte forged request buys a 28-byte reply
    // aimed at a victim (measured: the driver's connect REJECTION is 28 bytes). A peer that holds a
    // session completed the connect exchange and is exempt; holding its retransmits and acks to its
    // last datagram's size would starve the very session the server admitted.
    bool send(const platform::Endpoint& to, std::span<const std::byte> bytes) override {
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
        return socket_.send_to(to, bytes).has_value();
    }

    std::size_t receive(std::vector<net::Datagram>& out) override {
        received_sizes_.clear();
        std::size_t count = 0;
        // Read the whole UDP datagram so oversize input is refused explicitly, not silently
        // truncated into a plausible 1200-byte packet by the receive buffer.
        std::array<std::byte, 65536> buffer{};
        // A fixed poll budget bounds anonymous work even when a peer floods the socket.
        while (count < 64) {
            platform::Endpoint from{};
            const auto n = socket_.recv_from(from, buffer);
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

    [[nodiscard]] std::uint16_t port() const noexcept { return socket_.local_port(); }

    [[nodiscard]] std::uint64_t amplification_blocked() const noexcept {
        return amplification_blocked_;
    }

    [[nodiscard]] std::uint64_t oversized() const noexcept { return oversized_; }

private:
    platform::UdpSocket socket_;
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
    Impl(platform::UdpSocket socket,
         double tick_hz,
         const GameDefinition::NetworkInputProfile& profile)
        : link(std::move(socket)), driver(link, driver_config()), profile(profile),
          neutral_after_ticks(static_cast<std::uint32_t>(std::max(1.0, std::ceil(tick_hz * 0.1)))) {
        driver.listen();
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

    void pump() {
        const auto now_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
        now = now_ms;
        events.clear();
        std::vector<platform::Endpoint> established;
        for (const auto id : driver.session_ids()) {
            if (const net::Session* session = driver.session(id))
                established.push_back(session->peer());
        }
        link.set_established(std::move(established));
        driver.update(now, events);
        for (const auto& e : events) {
            if (e.kind == net::SessionEvent::Kind::Disconnected && admitted && e.id == *admitted) {
                ++totals.clients_left;
                const auto reason = e.reason == net::DisconnectReason::Graceful   ? "graceful"
                                    : e.reason == net::DisconnectReason::Replaced ? "replaced"
                                                                                  : "timeout";
                event(fmt::format("{{\"event\":\"client_left\",\"reason\":\"{}\"}}", reason));
                admitted.reset();
                save_schedule();
            }
        }
        for (const auto id : driver.session_ids()) {
            net::Session* session = driver.session(id);
            if (!session)
                continue;
            messages.clear();
            session->drain_received(messages);
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
                        hello,
                        input_schema_hash(profile.profile_id, profile.profile_version),
                        admitted.has_value() && id != *admitted);
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
                        // A refused profile must not occupy one of the two transport slots until
                        // the idle timeout; the reply is queued before closing the session.
                        session->disconnect();
                    }
                    if (reply.refusal == InputHelloRefusal::None && !admitted) {
                        admitted = id;
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
    std::uint32_t neutral_after_ticks = 6;
    std::optional<net::SessionId> admitted;
    std::optional<InputSchedule> schedule;
    RunReport::NetworkInputCounters totals{};
    std::uint64_t now = 0;
    bool last_applied = false;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;
    std::vector<InputCommand> commands;
};

NetworkInputServer::NetworkInputServer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NetworkInputServer::~NetworkInputServer() = default;

std::unique_ptr<NetworkInputServer>
NetworkInputServer::bind(const std::string& host,
                         std::uint16_t port,
                         double tick_hz,
                         const GameDefinition::NetworkInputProfile& profile,
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
    return std::unique_ptr<NetworkInputServer>(
        new NetworkInputServer(std::make_unique<Impl>(std::move(*socket), tick_hz, profile)));
}

std::uint16_t NetworkInputServer::local_port() const noexcept {
    return impl_->link.port();
}

ActionState NetworkInputServer::before_tick(std::uint64_t) {
    impl_->pump();
    impl_->last_applied = false;
    if (!impl_->schedule) {
        ++impl_->totals.synthesized;
        return {};
    }
    const ScheduledInput selected = impl_->schedule->step();
    impl_->last_applied = selected.applied;
    return impl_->profile.to_actions(selected.command);
}

bool NetworkInputServer::last_applied() const noexcept {
    return impl_->last_applied;
}

void NetworkInputServer::after_tick(std::uint64_t server_tick) {
    if (!impl_->admitted || !impl_->schedule)
        return;
    net::Session* session = impl_->driver.session(*impl_->admitted);
    if (!session)
        return;
    std::array<std::byte, kInputResultBytes> bytes{};
    const auto n = encode_input_result(impl_->schedule->result(server_tick), bytes);
    (void)session->send_unreliable(std::span(bytes).first(n), impl_->now);
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
    return counts;
}

} // namespace rime::app
