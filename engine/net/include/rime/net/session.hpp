// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "rime/net/control_packets.hpp"
#include "rime/net/link.hpp"
#include "rime/net/reliable_channel.hpp"

// Session (m11.2, ADR-0033 §7) — ONE relationship with ONE peer: its ReliableChannel, its
// connection state, its liveness timers, and the messages it has delivered that the game has not
// drained yet. Sessions are created, driven, and reaped exclusively by NetDriver (the friend
// below): the driver polls the shared Link exactly once per tick and routes each datagram to the
// session whose incarnation sent it. A Session never receives from the Link directly.
//
// Incarnation, in one paragraph: when a dead peer reconnects, its OLD incarnation's packets may
// still be in flight, and a fresh ReliableChannel starts every sequence space at 0 — so stale bytes
// are indistinguishable from early legitimate traffic and would be buffered into the new stream.
// The fix is the salt: every datagram this session emits carries a 4-byte incarnation identifier,
// stamped by the SessionLink adapter on the way out and checked by the driver on the way in.
// ReliableChannel itself is untouched; it never knows the frame exists.
//
// Time is always an input (now_ms), as everywhere in engine/net — no clock reads.
namespace rime::net {

// A stable handle to one session slot. Generational for the same reason ecs::Entity is: sessions
// get REPLACED (a crashed client's reincarnation recycles the slot), and a stale SessionId must
// address nothing rather than silently addressing the new incarnation. Resolve with
// NetDriver::session(id) — null means the handle is stale.
struct SessionId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const SessionId&, const SessionId&) = default;
};

enum class SessionState : std::uint8_t {
    Connecting, // client-side only: ConnectRequest in flight, retrying on a timer
    Connected,  // handshake complete; channel traffic flows
    Closing,    // graceful disconnect announced; lingering to re-send it a few times
};

// The timers a session runs, copied out of NetDriver::Config at construction. A plain struct (not
// a Config&) so the session owns its timing outright — no dangling if the driver's config outlives
// it or is copied.
struct SessionTiming {
    std::uint64_t heartbeat_interval_ms = 500; // idle keepalive; any traffic suppresses it
    std::uint64_t timeout_ms = 5000;           // silence past this = peer death
    std::uint64_t connect_retry_ms = 250;      // ConnectRequest re-send cadence
    int connect_attempts = 20;                 // ~5 s of trying before ConnectTimeout
    int linger_sends = 3;                      // graceful-close re-sends, one per tick
};

class Session {
public:
    // Constructed only by NetDriver — everything that DRIVES a session (its timers, its salts, its
    // routing) is private below, so a hand-made Session is inert and harmless. The constructor
    // itself is public for one mundane reason: the driver owns sessions through
    // std::make_unique<Session>, and make_unique is not a friend of anybody. Naming that here is
    // cheaper than the passkey ceremony it would take to hide it.
    Session(SessionId id,
            const Endpoint& peer,
            Link& link,
            std::uint32_t salt,
            std::uint32_t client_salt,
            std::uint32_t server_salt,
            std::uint64_t resend_ms,
            const SessionTiming& timing,
            SessionState initial_state,
            std::vector<std::byte> connect_request,
            std::uint64_t now_ms);

    // Channels into the conversation, with the channel's own contracts (reliable_channel.hpp).
    // Both refuse when state() != Connected: sending to a half-open peer is how bytes end up in a
    // fresh incarnation's sequence space. send_reliable can additionally refuse under kMaxPending
    // backpressure — surfaced as-is; whether to drop a drowning peer is game policy (call
    // disconnect()), not a session default.
    [[nodiscard]] bool send_reliable(std::span<const std::byte> message, std::uint64_t now_ms);
    [[nodiscard]] bool send_unreliable(std::span<const std::byte> message, std::uint64_t now_ms);

    // Move every message delivered since the last drain into `out` (appended, not cleared — the
    // same contract as Link::receive); returns the count. Polled, not called back: the game asks at
    // its tick boundary, the session never re-enters game code.
    std::size_t drain_received(std::vector<Received>& out);

    // Ask to leave gracefully: transition to Closing, which announces Disconnect once per tick for
    // linger_sends ticks (one datagram is a coin-flip on a lossy link), then lets the driver reap
    // the slot. The peer's timeout is the real backstop if every copy is lost — graceful close is
    // an optimization of latency, never of correctness.
    void disconnect() noexcept;

    [[nodiscard]] SessionId id() const noexcept { return id_; }

    [[nodiscard]] const Endpoint& peer() const noexcept { return peer_; }

    [[nodiscard]] SessionState state() const noexcept { return state_; }

    // Liveness introspection for diagnostics and tests — the timeout proof reads these to show
    // heartbeats really flowed before the peer died; a proof that proves nothing if it was idle.
    [[nodiscard]] std::uint64_t last_recv_ms() const noexcept { return last_recv_ms_; }

    [[nodiscard]] std::uint64_t heartbeats_sent() const noexcept { return heartbeats_sent_; }

private:
    // Only NetDriver constructs and drives sessions — the slot, salt, and timer bookkeeping is the
    // driver's, and construction needs the wiring below to happen exactly once, in order.
    friend class NetDriver;

    // The salt-stamping adapter the channel is constructed over. On the wire, every channel
    // datagram is now [salt:4][channel header:13][payload] — 17 bytes of header, the accepted price
    // of incarnation safety (ADR-0033 A7). The salt is mutable exactly once in the session's life:
    // promote() swaps the pre-incarnation client_salt for the folded incarnation salt when the
    // accept arrives.
    //
    // receive() is a deliberate dead end: the driver owns polling of the real Link. It exists only
    // because Link is an interface; ReliableChannel never calls it.
    class SessionLink final : public Link {
    public:
        SessionLink(Link& inner, std::uint32_t salt) noexcept : inner_(&inner), salt_(salt) {}

        bool send(const Endpoint& to, std::span<const std::byte> data) override;
        std::size_t receive(std::vector<Datagram>& out) override;

        [[nodiscard]] std::uint32_t salt() const noexcept { return salt_; }

        void set_salt(std::uint32_t salt) noexcept { salt_ = salt; }

    private:
        Link* inner_; // non-owning, same discipline as ReliableChannel's Link*
        std::uint32_t salt_;
        // Reused send scratch: one framing buffer per session instead of one allocation per
        // datagram. Safe because sends are synchronous and single-threaded — the buffer is dead
        // again by the time send() returns.
        std::vector<std::byte> scratch_;
    };

    // Immovable, and this is load-bearing: channel_ stores a Link* that points AT our own link_
    // member. If a Session ever moved, channel_ would keep pointing at the old address — a dangling
    // pointer that would fire only once the driver's slot storage grew. The driver cooperates by
    // holding each Session through a unique_ptr, so the object's address is fixed for its whole
    // life and the slot table itself stays a plain, movable vector (see NetDriver::Slot).
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Driver entry points, all reached only from NetDriver::update.
    void on_channel_packet(std::span<const std::byte> payload, std::uint64_t now_ms);

    void on_heartbeat(std::uint64_t now_ms) noexcept { last_recv_ms_ = now_ms; }

    void promote(std::uint32_t incarnation_salt, std::uint64_t now_ms);

    // Runs this session's timers (channel resend, heartbeat, connect retry, close linger). Returns
    // the reason it wants to be reaped, if any — the driver turns that into the SessionEvent,
    // because event ordering across sessions is the driver's business.
    [[nodiscard]] std::optional<DisconnectReason> update(std::uint64_t now_ms);

    // Emit one control packet to the peer through the salt-stamping adapter, and stamp last_sent_
    // so an explicit send suppresses this tick's heartbeat.
    void send_control(std::span<const std::byte> payload, std::uint64_t now_ms);

    [[nodiscard]] std::uint32_t salt() const noexcept { return link_.salt(); }

    [[nodiscard]] std::uint32_t client_salt() const noexcept { return client_salt_; }

    [[nodiscard]] std::uint32_t server_salt() const noexcept { return server_salt_; }

    // Member declaration order IS construction order, and it is load-bearing exactly once: link_
    // must precede channel_, because channel_ is constructed over &link_.
    SessionId id_;
    Endpoint peer_;
    SessionTiming timing_;
    SessionState state_;
    // The salts that make this session addressable across reincarnation: client_salt_ tells a
    // duplicate ConnectRequest from this endpoint apart from a reincarnated one; server_salt_ lets
    // the server re-send the identical accept. Both are kNoSalt where not yet meaningful.
    std::uint32_t client_salt_;
    std::uint32_t server_salt_;
    SessionLink link_;           // FIRST of the pair: the adapter must exist before...
    ReliableChannel channel_;    // ...the channel constructed over it
    std::deque<Received> inbox_; // delivered by the channel, not yet drained by the game
    // The ConnectRequest PAYLOAD (no salt frame — the adapter stamps the current salt, which while
    // Connecting IS the client_salt). Kept so retries re-send byte-identical requests: a duplicate
    // request must be recognizable as the same attempt, not a reincarnation.
    std::vector<std::byte> connect_request_;
    std::uint64_t last_recv_ms_ = 0;   // last time ANY valid packet arrived from the peer
    std::uint64_t last_sent_ms_ = 0;   // last time we put anything on the wire
    std::uint64_t next_action_ms_ = 0; // Connecting: next retry deadline
    int attempts_left_ = 0;            // Connecting retries / Closing linger sends
    std::uint64_t heartbeats_sent_ = 0;
};

} // namespace rime::net
