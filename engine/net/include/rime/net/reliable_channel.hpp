// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "rime/net/link.hpp"

// ReliableChannel (m11.1, ADR-0033 §3) — one peer-to-peer conversation over a Link, offering the
// two delivery contracts M11's replication is built from:
//
//   send_reliable()   → reliable-ordered:   arrives exactly once, in send order (events,
//                                           spawn/despawn — the destruction event stream of
//                                           ADR-0029/ADR-0033 rides this).
//   send_unreliable() → unreliable-sequenced: arrives at most once and only if newer than
//                                           everything delivered so far (snapshots — late state
//                                           is garbage, so it is dropped, never resent).
//
// The channel does NOT drain the Link: a driver owns the socket, polls it once, and routes each
// datagram to the channel whose peer sent it (process_packet). That is what lets one UDP socket
// serve N peers — a channel that drained the shared Link would eat everyone else's packets.
// (m11.1's tests route by hand; m11.2's session layer is the driver.)
//
// Time is an INPUT (now_ms parameters), never read from a clock: the same code then runs on wall
// time in a game and on a virtual clock in the deterministic ScriptedNetwork proofs — the
// property that keeps every networking test bit-reproducible. The design notes (sequence spaces,
// the ack scheme and why it is frontier-anchored, the deliberate simplifications) live at the top
// of reliable_channel.cpp.
namespace rime::net {

// Which of the two delivery contracts a packet or message belongs to. AckOnly is not a contract
// but a control packet: a header with no payload, emitted by update() when reliable traffic
// arrived since the last packet we sent — so a one-way reliable conversation (a receiver with
// no data of its own for acks to ride home on) still acks. AckOnly touches neither sequence
// space; at most one is emitted per tick.
enum class Channel : std::uint8_t {
    ReliableOrdered = 0,
    UnreliableSequenced = 1,
    AckOnly = 2,
};

// One delivered message: which contract it arrived under and its bytes.
struct Received {
    Channel channel;
    std::vector<std::byte> bytes;
};

class ReliableChannel {
public:
    // The largest payload that fits one datagram alongside the header — sized to stay under the
    // de-facto Internet MTU (1500) with room for IP/UDP headers, so no fragmentation ever occurs
    // (v1 does no packet fragmentation of its own).
    static constexpr std::size_t kMaxPayload = 1200;

    // The in-flight window, binding BOTH directions of the reliable stream: the sender keeps at
    // most this many transmitted-but-unacked packets in flight (further sends queue locally until
    // acks free budget — so the receiver never sees traffic it would have to drop), and the
    // receiver buffers out-of-order arrivals up to this far ahead of the delivery frontier. One
    // number because the two are the same flow-control contract, and 32 because the ack bitfield
    // is 32 bits: every seq the receiver can be holding is reportable in one ack. (Widening the
    // window past 32 means widening the bitfield to u64 — 4 more header bytes per packet; not
    // worth it at 13-byte headers.)
    static constexpr std::uint32_t kWindow = 32;

    // Backpressure bound on queued-but-unsent + in-flight reliable messages. Past it
    // send_reliable() returns false — the peer is dead or drowning, and unbounded queuing is how
    // servers OOM. (The caller's response — drop the connection, slow production — is a
    // session-layer decision, m11.2.)
    static constexpr std::size_t kMaxPending = 256;

    // `resend_ms`: how long a reliable packet may stay unacked before update() retransmits it.
    // Fixed for v1 (RTT-adaptive RTO is a measured follow-up).
    ReliableChannel(Link& link, const Endpoint& peer, std::uint64_t resend_ms = 100) noexcept;

    // Queue one message under the chosen contract. Reliable messages transmit immediately if the
    // in-flight window has budget, else sit until acks free some (update() pumps them). false =
    // refused (payload over kMaxPayload; or, for reliable, the backlog hit kMaxPending).
    bool send_reliable(std::span<const std::byte> message, std::uint64_t now_ms);
    bool send_unreliable(std::span<const std::byte> message);

    // Per-tick housekeeping: retransmit in-flight packets unacked past resend_ms, transmit queued
    // messages the window now has budget for, and emit one AckOnly if reliable traffic arrived
    // since our last packet. Call once per tick.
    void update(std::uint64_t now_ms);

    // Feed one datagram that arrived from our peer (the driver routed it by endpoint). Appends
    // every message that became deliverable — reliable ones in exact send order — to `out`, and
    // returns how many were appended (0 is normal).
    std::size_t process_packet(std::span<const std::byte> packet, std::vector<Received>& out);

    // Counters so proofs can assert behaviour, not just outcomes (a loss test that resent
    // nothing proved nothing).
    [[nodiscard]] std::uint64_t packets_sent() const noexcept { return packets_sent_; }

    [[nodiscard]] std::uint64_t packets_resent() const noexcept { return packets_resent_; }

    [[nodiscard]] std::uint64_t messages_delivered() const noexcept { return messages_delivered_; }

    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Pending {
        std::uint32_t seq;
        std::vector<std::byte> bytes;
        std::uint64_t last_sent_ms = 0;
        bool transmitted = false; // false = queued behind a full in-flight window
    };

    void transmit(Channel channel, std::uint32_t seq, std::span<const std::byte> payload);
    void pump(std::uint64_t now_ms); // transmit queued reliables the window has budget for
    void process_ack(std::uint32_t ack, std::uint32_t ack_bits);
    void receive_reliable(std::uint32_t seq,
                          std::span<const std::byte> payload,
                          std::vector<Received>& out);
    void receive_unreliable(std::uint32_t seq,
                            std::span<const std::byte> payload,
                            std::vector<Received>& out);
    void deliver(std::vector<Received>& out, std::span<const std::byte> payload);
    bool mark_received(std::uint32_t seq); // true = was already marked (duplicate)
    [[nodiscard]] std::uint32_t ack() const noexcept;
    [[nodiscard]] std::uint32_t ack_bits() const noexcept;

    Link* link_;
    Endpoint peer_;
    std::uint64_t resend_ms_;

    // Sender side.
    std::uint32_t next_reliable_seq_ = 0;
    std::uint32_t next_unreliable_seq_ = 0;
    std::deque<Pending> pending_; // unacked (or unsent) reliable messages, in seq order
    std::uint32_t in_flight_ = 0; // transmitted-but-unacked count (the window usage)
    bool ack_dirty_ = false;      // reliable traffic arrived since our last packet

    // Receiver side, reliable: deliver_seq_ is the delivery frontier (next in-order seq to
    // release — and the ack anchor); early_ holds out-of-order arrivals within the window; the
    // bitfield ring backs the ack reports.
    std::uint32_t deliver_seq_ = 0;
    std::deque<std::pair<std::uint32_t, std::vector<std::byte>>> early_;
    std::uint64_t received_bits_ = 0;
    std::uint32_t window_base_ = 0;

    // Receiver side, unreliable-sequenced.
    std::uint32_t latest_unreliable_seq_ = 0;
    bool have_unreliable_ = false;

    std::vector<std::byte> packet_buf_; // scratch for transmit(), avoids per-send allocation

    std::uint64_t packets_sent_ = 0;
    std::uint64_t packets_resent_ = 0;
    std::uint64_t messages_delivered_ = 0;
};

} // namespace rime::net
