// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "rime/platform/socket.hpp"

// The Link seam (m11.1, ADR-0033 §3) — the thinnest possible statement of "move a datagram from
// one endpoint to another." Everything in engine/net above the raw socket is written against this
// interface, never against a concrete transport, so that:
//
//   - **UdpLink** carries real traffic over platform::UdpSocket (the OS's UDP), and
//   - **ScriptedNetwork** (below) replaces the internet with a deterministic in-process simulation
//     of it — scripted loss, latency, and reordering driven by a *virtual* clock — so every
//     networking proof runs GPU-free, bit-reproducibly, in CI. Loss and latency are test INPUTS,
//     not environment luck: a test that needs 30% loss *specifies* 30% loss and can assert that
//     loss actually happened (the network counts it), so the proof is never vacuous.
//
// This is the same discipline as the ByteStream seam that let the streaming protocol ride TCP or
// UDS unchanged (S1.4) — one protocol, two transports — applied one layer lower, to datagrams.
namespace rime::net {

using rime::platform::Endpoint;

class ScriptedNetwork; // defined below

// One received datagram: who it came from and what it carried. Owns its bytes because a poll
// drains whatever the OS has buffered — the caller processes them after the socket is done.
struct Datagram {
    Endpoint from;
    std::vector<std::byte> bytes;
};

// A datagram transport: send one packet, drain whatever arrived. Implementations are *not*
// required to deliver, to deliver in order, or to deliver at most once — that is what UDP is,
// and the reliability layer above (reliable_channel.hpp) is where those guarantees get built.
class Link {
public:
    virtual ~Link() = default;

    // Send one datagram to `to`. false on a local error (a closed socket, a bad endpoint) —
    // never on "the packet might not arrive", which UDP semantics make unknowable here.
    virtual bool send(const Endpoint& to, std::span<const std::byte> data) = 0;

    // Append every currently-available datagram to `out` and return how many arrived (0 is the
    // normal idle-poll result). Never blocks: the game loop polls once per tick.
    virtual std::size_t receive(std::vector<Datagram>& out) = 0;
};

// The real transport: a Link over platform::UdpSocket. Binds its own socket (ephemeral port by
// default — read it back with local_endpoint()).
class UdpLink final : public Link {
public:
    // Bind on `port` (0 = ephemeral) at `host` (default loopback; "0.0.0.0" for a server).
    // nullopt on failure (logged by the socket backend).
    [[nodiscard]] static std::optional<UdpLink> bind(std::uint16_t port,
                                                     std::string_view host = "127.0.0.1");

    bool send(const Endpoint& to, std::span<const std::byte> data) override;
    std::size_t receive(std::vector<Datagram>& out) override;

    [[nodiscard]] Endpoint local_endpoint() const noexcept { return local_; }

private:
    explicit UdpLink(rime::platform::UdpSocket socket, Endpoint local) noexcept
        : socket_(std::move(socket)), local_(local) {}

    rime::platform::UdpSocket socket_;
    Endpoint local_;
};

// One node's end of a ScriptedNetwork: send puts a packet in flight (or drops it, per the
// network's config); receive drains what advance_time() has delivered.
class ScriptedLink final : public Link {
public:
    bool send(const Endpoint& to, std::span<const std::byte> data) override;
    std::size_t receive(std::vector<Datagram>& out) override;

private:
    ScriptedLink(ScriptedNetwork& network, const Endpoint& endpoint) noexcept
        : network_(&network), endpoint_(endpoint) {}

    ScriptedNetwork* network_;
    Endpoint endpoint_;

    friend class ScriptedNetwork;
};

// A deterministic, in-process network for tests and headless proofs: N ScriptedLinks addressed by
// their Endpoints, with packets "in flight" between them delivered by advance_time(). Same seed +
// same script ⇒ same trace, every time, on every platform — the property that lets the reliability
// layer's loss/reorder/duplicate proofs live in CI instead of on someone's flaky Wi-Fi.
//
// The simulated imperfections are deliberately the ones UDP really has:
//   - **loss** (a packet never arrives — the sender is never told),
//   - **latency** (a packet arrives after a delay drawn from [min,max]),
//   - **reordering** (falls out of latency jitter for free: a later packet with a shorter draw
//     lands first),
//   - **duplication** (a packet arrives twice — rare on real links, common once resends exist).
//
// The PRNG is a tiny xorshift64 — not std::mt19937 — so the byte stream is identical on every
// standard library (determinism is the whole point; std engines' distributions are
// implementation-defined in their exact outputs).
class ScriptedNetwork {
public:
    struct Config {
        float loss_rate = 0.0f;      // [0,1): probability a sent packet vanishes
        float duplicate_rate = 0.0f; // [0,1): probability a sent packet arrives twice
        std::uint64_t min_latency_ms = 0;
        std::uint64_t max_latency_ms = 0; // >= min; the spread is what reorders
    };

    // Defined out-of-line: a nested struct's default member initializers may not be used by a
    // default argument *inside* the enclosing class definition.
    ScriptedNetwork(std::uint64_t seed, Config config);
    explicit ScriptedNetwork(std::uint64_t seed);

    // Register a node at `endpoint` and get its Link (owned by the network — no raw new; the
    // reference is stable because links live in a deque). The network must outlive its links.
    [[nodiscard]] ScriptedLink& add_node(const Endpoint& endpoint);

    // Move every packet whose arrival time is <= `now_ms` into its destination's inbox. The
    // caller owns the (virtual) clock; nothing moves without this call.
    void advance_time(std::uint64_t now_ms);

    // Counters so a test can assert the scenario really exercised what it claims — a "30% loss"
    // proof that dropped nothing proves nothing.
    [[nodiscard]] std::uint64_t packets_sent() const noexcept { return sent_; }

    [[nodiscard]] std::uint64_t packets_dropped() const noexcept { return dropped_; }

    [[nodiscard]] std::uint64_t packets_delivered() const noexcept { return delivered_; }

private:
    struct InFlight {
        std::uint64_t arrival_ms;
        Endpoint from;
        std::vector<std::byte> bytes;
    };

    struct Node {
        std::deque<InFlight> inbox; // due packets, oldest first
    };

    // xorshift64* — 8 lines, deterministic everywhere, plenty for a loss coin-flip.
    std::uint64_t next_random() noexcept {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return rng_ * 0x2545F4914F6CDD1Dull;
    }

    // A uniform float in [0,1) from the top 53 bits of the next draw.
    float next_unit() noexcept {
        return static_cast<float>(next_random() >> 11) * (1.0f / 9007199254740992.0f);
    }

    void transmit(const Endpoint& from, const Endpoint& to, std::span<const std::byte> data);

    Config config_;
    std::uint64_t rng_;
    std::uint64_t now_ms_ = 0;
    std::vector<std::pair<Endpoint, Node>> nodes_; // tiny N; linear scan is honest
    std::deque<ScriptedLink> links_; // owned, stable-addressed (deque never relocates)
    std::deque<std::pair<Endpoint, InFlight>> flight_; // packets not yet due, by destination
    std::uint64_t sent_ = 0, dropped_ = 0, delivered_ = 0;

    friend class ScriptedLink;
};

} // namespace rime::net
