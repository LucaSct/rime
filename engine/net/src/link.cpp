// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The two Link implementations (m11.1, ADR-0033 §3): UdpLink over the OS's UDP, and the
// deterministic in-process ScriptedNetwork the proofs run on. See link.hpp for the seam's why.

#include "rime/net/link.hpp"

#include <utility>

namespace rime::net {

// ── UdpLink ─────────────────────────────────────────────────────────────────────────────
std::optional<UdpLink> UdpLink::bind(std::uint16_t port, std::string_view host) {
    auto socket = rime::platform::UdpSocket::bind(port, host);
    if (!socket) {
        return std::nullopt;
    }
    Endpoint local{};
    const auto parsed = Endpoint::from_string(std::string(host), socket->local_port());
    local = parsed.value_or(Endpoint{0, socket->local_port()});
    return UdpLink(std::move(*socket), local);
}

bool UdpLink::send(const Endpoint& to, std::span<const std::byte> data) {
    return socket_.send_to(to, data).has_value();
}

std::size_t UdpLink::receive(std::vector<Datagram>& out) {
    // Drain whatever the OS has buffered, one recv_from per datagram, until the non-blocking
    // socket says "empty". Bounded per call so a flooded socket can't starve the tick — the rest
    // is picked up next poll.
    constexpr std::size_t kMaxPerPoll = 64;
    std::size_t count = 0;
    std::byte buf[64 * 1024]; // UDP's hard ceiling is 65507 bytes of payload
    while (count < kMaxPerPoll) {
        Endpoint from{};
        const auto n = socket_.recv_from(from, buf);
        if (!n) {
            break; // empty (normal) or error (logged by the backend)
        }
        out.push_back(Datagram{from, std::vector<std::byte>(buf, buf + *n)});
        ++count;
    }
    return count;
}

// ── ScriptedNetwork ─────────────────────────────────────────────────────────────────────
ScriptedNetwork::ScriptedNetwork(std::uint64_t seed, Config config)
    : config_(config), rng_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

ScriptedNetwork::ScriptedNetwork(std::uint64_t seed) : ScriptedNetwork(seed, Config{}) {}

ScriptedLink& ScriptedNetwork::add_node(const Endpoint& endpoint) {
    nodes_.emplace_back(endpoint, Node{});
    // The temporary is constructed here, where friendship applies; the deque move-assigns it in.
    links_.push_back(ScriptedLink(*this, endpoint));
    return links_.back();
}

void ScriptedNetwork::advance_time(std::uint64_t now_ms) {
    now_ms_ = now_ms;
    // Deliver everything due. The flight queue is append-ordered, not sorted, so scan it all —
    // N is tiny in every test/proof, and a priority queue would buy nothing measurable here.
    for (auto it = flight_.begin(); it != flight_.end();) {
        if (it->second.arrival_ms <= now_ms) {
            for (auto& [addr, node] : nodes_) {
                if (addr == it->first) {
                    node.inbox.push_back(std::move(it->second));
                    ++delivered_;
                    break;
                }
            }
            it = flight_.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptedNetwork::transmit(const Endpoint& from,
                               const Endpoint& to,
                               std::span<const std::byte> data) {
    ++sent_;
    if (config_.loss_rate > 0.0f && next_unit() < config_.loss_rate) {
        ++dropped_; // the packet vanishes — and, exactly like UDP, nobody is told
        return;
    }
    const std::uint64_t spread = config_.max_latency_ms - config_.min_latency_ms;
    const std::uint64_t latency =
        config_.min_latency_ms + (spread > 0 ? next_random() % (spread + 1) : 0);
    InFlight packet{now_ms_ + latency, from, std::vector<std::byte>(data.begin(), data.end())};
    flight_.emplace_back(to, packet);
    if (config_.duplicate_rate > 0.0f && next_unit() < config_.duplicate_rate) {
        flight_.emplace_back(to, std::move(packet)); // the rare-but-real double delivery
    }
}

// ── ScriptedLink ────────────────────────────────────────────────────────────────────────
bool ScriptedLink::send(const Endpoint& to, std::span<const std::byte> data) {
    network_->transmit(endpoint_, to, data);
    return true; // "sent" — arrival is the network's business, not the sender's
}

std::size_t ScriptedLink::receive(std::vector<Datagram>& out) {
    for (auto& [addr, node] : network_->nodes_) {
        if (addr == endpoint_) {
            const std::size_t count = node.inbox.size();
            while (!node.inbox.empty()) {
                ScriptedNetwork::InFlight packet = std::move(node.inbox.front());
                node.inbox.pop_front();
                out.push_back(Datagram{packet.from, std::move(packet.bytes)});
            }
            return count;
        }
    }
    return 0; // a link whose node was never registered delivers nothing
}

} // namespace rime::net
