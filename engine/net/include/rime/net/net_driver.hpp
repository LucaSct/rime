// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rime/net/session.hpp"

// NetDriver (m11.2, ADR-0033 §7) — the session layer the ReliableChannel header has been waiting
// for: it owns the endpoint→session routing table, polls the shared Link exactly once per tick,
// runs the connection handshake, and reaps dead peers. It is ROLE-AGNOSTIC: the same type serves a
// dedicated server (listen() + N sessions) and a client (connect() + 1 session). A listen server is
// both in one process — an embedding, not an architecture (ADR-0033 §1).
//
// The driver holds the Link NON-OWNING (Link&), the same discipline ReliableChannel already uses:
// ScriptedNetwork owns its links' lifetimes, and the deterministic proofs depend on that. What the
// driver owns is the exclusive right to receive() from it — and everything it routes.
//
// Wire framing (every datagram this layer emits): [salt:u32][payload...]
//   - payload[0] <  0x80 → a ReliableChannel packet, byte-identical to m11.1; routed by endpoint,
//                          dropped unless salt == the session's incarnation.
//   - payload[0] >= 0x80 → a session CONTROL packet (control_packets.hpp documents the layouts).
//
// The salt is the fix for a hole m11.1 could not see: when a dead peer reincarnates, its old
// channel's packets may still be in flight, and a fresh channel starts every sequence space at 0 —
// stale bytes would be buffered as legitimate early traffic and silently corrupt the new stream.
// The salt stamps the incarnation on every datagram; stale bytes die here, at the driver, before
// they reach channel state. The price is 4 bytes per datagram (17 bytes of header on channel
// traffic in total) — accepted, ADR-0033 A7.
//
// DoS posture, in order of cheapness — validate BEFORE allocate, always:
//   1. parse: magic + length + tag via bounds-checked ByteReader (cost: one counter);
//   2. salt: must match a live session, unless the packet is a ConnectRequest;
//   3. identity: protocol + app_id + schema_hash checked before any state is created.
// Only then does allocation happen, and it is bounded by max_sessions. Residual risk, named
// honestly: a spoofer WITH the correct identity triple can fill the table up to max_sessions per
// timeout window. The fix is a challenge-cookie exchange (the server proves the source owns its
// return path before allocating) — S2 internet-transport scope, and it folds in as one more
// control packet without touching anything here. ADR-0033 §6 already puts anti-cheat and
// encryption out of v1 scope; this is the same boundary.
//
// engine/net links only rime::platform. schema_hash arrives as an opaque std::uint64_t in Config —
// computed by the APP (ecs::component_schema_hash), never here.
//
// Time is always an input (now_ms). No clock reads anywhere in engine/net.
namespace rime::net {

// One thing that happened, handed to the game by value at its tick boundary. Polled (drained from
// update()), never a callback: net code must not re-enter game code mid-pump, and tests assert
// against the event stream directly — it is the proof artifact.
struct SessionEvent {
    enum class Kind : std::uint8_t {
        Connected,     // handshake complete (either role)
        Disconnected,  // an established session ended — reason says why
        ConnectFailed, // an outbound connect() never completed — reason says why
    };

    Kind kind = Kind::Connected;
    SessionId id{}; // the session this concerns; already reaped for Disconnected/ConnectFailed
    Endpoint endpoint{};
    DisconnectReason reason = DisconnectReason::Graceful; // Disconnected/ConnectFailed

    // Mismatch diagnostics — the numbers the HUMAN needs to fix the build (ADR-0033 §4's promise).
    // Zero unless reason is ProtocolMismatch/AppMismatch/SchemaMismatch.
    std::uint64_t expected = 0; // what the server runs
    std::uint64_t actual = 0;   // what we sent
};

// A one-line, actionable rendering of an event: "connection to 127.0.0.1:7777 failed: schema hash
// mismatch (server 0x9c41…, client 0x77aa…) — the client is a different build; rebuild/re-cook
// against the server's component set." A rejection is worthless if it cannot reach a human; this is
// the reach. A game that wants its own UI still has the raw fields.
[[nodiscard]] std::string format(const SessionEvent& event);

class NetDriver {
public:
    struct Config {
        // Identity, sent in the clear at connect and compared field-by-field so a rejection can
        // name the exact mismatch (see control_packets.hpp).
        std::uint32_t app_id = 0;      // whose game this is; 0 matches only 0
        std::uint64_t schema_hash = 0; // ecs::component_schema_hash(world), computed by the app

        // The DoS bound: the only unbounded-growth guard that matters, because everything else on
        // the accept path is validated before it can allocate.
        std::size_t max_sessions = 8;

        std::uint64_t heartbeat_interval_ms = 500; // idle keepalive; any traffic suppresses it
        std::uint64_t timeout_ms = 5000;           // silence past this = peer death
        std::uint64_t connect_retry_ms = 250;      // ConnectRequest re-send cadence
        int connect_attempts = 20;                 // ~5 s of trying before ConnectTimeout
        int linger_sends = 3;                      // graceful-close Disconnect re-sends
        std::uint64_t resend_ms = 100;             // channel RTO, passed through to ReliableChannel

        // Salt PRNG seed. Randomness follows the ScriptedNetwork discipline: an INPUT, never an
        // entropy read inside net code — tests pass a fixed seed (bit-reproducible incarnations), a
        // game passes OS entropy at startup. Two peers must not share a seed in a test that also
        // reincarnates them, or their salts collide and the reincarnation check cannot fire.
        std::uint64_t salt_seed = 0x9E3779B97F4A7C15ull;
    };

    NetDriver(Link& link, const Config& config) noexcept;

    // Server: start accepting ConnectRequests. Without this call they are dropped — a client has no
    // business accepting peers, and a driver that accepted by default would turn every client into
    // an open port. Idempotent.
    void listen() noexcept { accepting_ = true; }

    // Client: begin connecting to `server`. The outcome arrives as a SessionEvent (Connected or
    // ConnectFailed) from a future update() — connecting is a state, not a return value, because
    // the wire decides on the wire's schedule. Returns the id of the pending session (already
    // addressable, state() == Connecting), or nullopt if the table is full.
    std::optional<SessionId> connect(const Endpoint& server, std::uint64_t now_ms);

    // The per-tick pump, and the ONLY entry point that touches the wire: poll the Link once, route
    // every datagram, run every session's timers, reap the dead, retry pending handshakes. Events
    // are APPENDED to `events_out` (not cleared — the same contract as Link::receive). now_ms is an
    // input, as everywhere in engine/net.
    void update(std::uint64_t now_ms, std::vector<SessionEvent>& events_out);

    // Session access. session(id) returns null for a stale generational handle — the safe way to
    // hold a session across frames. session_ids() is for broadcast/iteration and is rebuilt each
    // update(), so it is valid until the next one.
    [[nodiscard]] Session* session(SessionId id) noexcept;

    [[nodiscard]] std::span<const SessionId> session_ids() const noexcept { return live_ids_; }

    [[nodiscard]] std::size_t session_count() const noexcept { return live_ids_.size(); }

    // Counters so proofs are never vacuous (the m11.1 harness discipline: a "30% loss" proof that
    // dropped nothing proves nothing). datagrams_dropped counts everything the driver refused —
    // garbage, wrong magic, stale salt, unroutable endpoint.
    [[nodiscard]] std::uint64_t datagrams_dropped() const noexcept { return dropped_; }

    [[nodiscard]] std::uint64_t connect_requests_seen() const noexcept { return requests_seen_; }

private:
    // A Session is immovable (session.hpp explains: its channel holds a Link* into its own member),
    // so the slot owns it through a unique_ptr — the object's address is then fixed for its whole
    // life no matter what the table does, which lets the table itself be a plain vector that can
    // grow and be reordered freely. The alternative (storing Sessions inline and reaching for a
    // deque's stable addresses) couples the container choice to a subtle lifetime invariant and
    // still trips over deque's MoveInsertable requirement for an immovable element. One pointer
    // chase per lookup, at N ≤ max_sessions, is not worth that.
    struct Slot {
        std::unique_ptr<Session> session; // null = free slot
        std::uint32_t generation = 0;     // bumped on every reap, so stale SessionIds dangle safely
    };

    // Routing is a LINEAR SCAN over the slots, not a hash map — the same call the ScriptedNetwork
    // makes for its node table ("tiny N; linear scan is honest"). max_sessions is a handful today
    // and 64 at the ADR's scale target: 64 comparisons of a 6-byte POD, once per datagram, is
    // nothing next to the syscall that delivered it. Keeping it a scan also means
    // platform::Endpoint does not have to grow std::hash/operator<=> for a speculative need; if a
    // profile ever says otherwise, adding them and swapping in a map is a contained change.
    [[nodiscard]] Slot* find_by_endpoint(const Endpoint& endpoint) noexcept;

    void on_control(const Endpoint& from,
                    std::uint32_t salt,
                    std::span<const std::byte> payload,
                    std::uint64_t now_ms,
                    std::vector<SessionEvent>& out);
    void on_connect_request(const Endpoint& from,
                            const ConnectRequest& request,
                            std::uint64_t now_ms,
                            std::vector<SessionEvent>& out);
    void on_connect_accept(Slot& slot,
                           const ConnectAccept& accept,
                           std::uint64_t now_ms,
                           std::vector<SessionEvent>& out);

    // Put one datagram on the wire with an EXPLICIT salt, bypassing any session. The handshake
    // needs this because its packets are framed with the requester's client_salt — the only value
    // a peer recognizes before the incarnation exists — while a session's own adapter stamps the
    // (different) folded incarnation salt. Rejections need it because, by design, they happen with
    // no session at all.
    void send_framed(const Endpoint& to, std::uint32_t salt, std::span<const std::byte> payload);

    // Send a rejection WITHOUT allocating anything — the whole point of validate-before-allocate.
    // The salt frame carries the requester's client_salt, the only value it recognizes yet.
    void send_reject(const Endpoint& to, const ConnectReject& reject);

    // Allocate (or recycle) a slot and construct the Session in place. Returns null when the table
    // is at max_sessions.
    [[nodiscard]] Slot* allocate_session(const Endpoint& peer,
                                         std::uint32_t salt,
                                         std::uint32_t client_salt,
                                         std::uint32_t server_salt,
                                         SessionState initial_state,
                                         std::vector<std::byte> connect_request,
                                         std::uint64_t now_ms);

    // Reap a slot, bumping its generation so every outstanding SessionId for it goes stale, and
    // append the matching event. `kind` distinguishes a connection that died from one that never
    // formed — the game shows those differently.
    // `expected`/`actual` ride along for the mismatch reasons, so the event the game drains carries
    // the two numbers a human needs and format() can render them.
    void kill(Slot& slot,
              DisconnectReason reason,
              SessionEvent::Kind kind,
              std::vector<SessionEvent>& out,
              std::uint64_t expected = 0,
              std::uint64_t actual = 0);

    void rebuild_live_ids();

    // xorshift64*, the same generator (and the same reasoning) as ScriptedNetwork's: deterministic
    // on every standard library, so a seeded test reproduces its incarnations exactly. Never
    // returns kNoSalt, which is reserved for "no session".
    [[nodiscard]] std::uint32_t next_salt() noexcept;

    Link* link_; // non-owning; see the file comment
    Config config_;
    bool accepting_ = false;

    std::vector<Slot> slots_;
    std::vector<SessionId> live_ids_; // rebuilt each update(), for iteration/broadcast
    std::vector<Datagram> datagrams_; // poll scratch, reused across ticks
    std::uint64_t rng_;
    std::uint64_t dropped_ = 0;
    std::uint64_t requests_seen_ = 0;
};

} // namespace rime::net
