// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The reliable-ordered / unreliable-sequenced channel pair (m11.1, ADR-0033 §3) — the minimal
// reliability layer that turns "datagrams, maybe, eventually" into the two delivery contracts a
// game actually wants:
//
//   - **Reliable-ordered** (events, spawn/despawn, handshake): every message arrives, exactly
//     once, in send order. Each packet carries a sequence number; the receiver reports its
//     receive state with an ack + ack-bitfield piggybacked on every packet it sends; the sender
//     retransmits whatever stays unacked past a resend timeout. The receiver buffers out-of-order
//     arrivals and releases them in sequence.
//
//   - **Unreliable-sequenced** (snapshots, transforms): freshness over completeness. Each packet
//     has a sequence number; the receiver delivers a packet only if it is *newer than everything
//     seen so far* and drops the rest. Nothing is ever resent — a late snapshot is garbage, and
//     the next one is already on its way.
//
// Design points worth understanding (each was earned by a failing test):
//
//   1. **Each channel has its OWN sequence space.** Sharing one space between channels creates a
//      nasty stall: reliable-ordered delivery waits for contiguous sequences, so if the missing
//      sequence belonged to a lost UNRELIABLE packet (which is never resent), the reliable stream
//      would wait forever. Separate spaces make each channel's ordering story self-contained.
//
//   2. **The ack is the delivery frontier, with a FORWARD-looking bitfield** — not the classic
//      "newest received seq + 32 bits looking back." The classic form has two holes, both
//      reproduced here before this design landed:
//        (a) a peer that has received NOTHING reports newest=0, and the sender's `seq == ack`
//            test reads that as "seq 0 acked" — the very first packet is dropped from the resend
//            queue while actually lost, deadlocking the stream;
//        (b) a packet lost then recovered *after* newer seqs arrived can never be reported again
//            (it is neither "newest" nor within 32 behind it), so the sender resends it forever.
//      Anchoring the ack at deliver_seq_ ("next expected") makes both evaporate: ack=0 means
//      "I have delivered nothing" and clears nothing (s < 0 is impossible); and anything ever
//      delivered is permanently `seq < deliver_seq_`, so recovery is reported forever by
//      `seq < ack`. Bit i of the bitfield reports received(deliver_seq_ + i) — the buffered
//      out-of-order arrivals just past the frontier. The window is 32 precisely so every
//      bufferable seq has a bit (see kWindow in the header).
//
//   3. **Acks ride every packet, and a dirty flag buys one AckOnly per tick.** Every data packet
//      carries the current ack state; update() additionally emits a header-only AckOnly packet
//      iff reliable traffic arrived since we last sent anything — a one-way reliable
//      conversation (a silent receiver) must still ack, but at most one control packet per tick.
//
//   4. **The channel never drains the Link.** A driver polls the shared socket once and routes
//      datagrams by sender endpoint — one UDP socket serves N peers. (See the header for why
//      this decision was taken before sessions landed.)
//
// Simplifications (labeled, per house rules; all are additive seams, not rewrites):
//   - One message per datagram (no coalescing) and no fragmentation: kMaxPayload keeps a packet
//     under the path MTU so the IP layer never fragments either.
//   - Sequence numbers are u32 and do not wrap (2^32 packets per channel per session ≈ 49 days at
//     1000/s; sessions are m11.2, wrap-around arithmetic lands with them if ever needed).
//   - The resend timeout is a fixed constructor parameter; RTT-adaptive RTO is a measured
//     follow-up, not a guess.

#include "rime/net/reliable_channel.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"

namespace rime::net {

// The header: channel u8 | seq u32 | ack u32 | ack_bits u32 — 13 bytes, written/read field by
// field through core's bounds-checked cursors (this is the engine's first untrusted-remote-input
// surface; nothing here is hand-parsed).

ReliableChannel::ReliableChannel(Link& link, const Endpoint& peer, std::uint64_t resend_ms) noexcept
    : link_(&link), peer_(peer), resend_ms_(resend_ms) {}

bool ReliableChannel::send_reliable(std::span<const std::byte> message, std::uint64_t now_ms) {
    if (message.size() > kMaxPayload || pending_.size() >= kMaxPending) {
        return false; // too big for one datagram, or the peer isn't acking (backpressure)
    }
    Pending p;
    p.seq = next_reliable_seq_++;
    p.bytes.assign(message.begin(), message.end());
    pending_.push_back(std::move(p));
    pump(now_ms); // transmit now if the window has budget, else acks will free some later
    return true;
}

bool ReliableChannel::send_unreliable(std::span<const std::byte> message) {
    if (message.size() > kMaxPayload) {
        return false;
    }
    transmit(Channel::UnreliableSequenced, next_unreliable_seq_++, message);
    ++packets_sent_;
    return true;
}

void ReliableChannel::pump(std::uint64_t now_ms) {
    // The in-flight window is the flow-control contract: the receiver buffers at most kWindow
    // seqs past its frontier, so the sender keeps at most kWindow packets unacked-and-sent —
    // anything more would be dropped on arrival and paid for again in resends.
    for (Pending& p : pending_) {
        if (p.transmitted || in_flight_ >= kWindow) {
            continue;
        }
        p.transmitted = true;
        p.last_sent_ms = now_ms;
        ++in_flight_;
        transmit(Channel::ReliableOrdered, p.seq, p.bytes);
        ++packets_sent_;
    }
}

void ReliableChannel::update(std::uint64_t now_ms) {
    // Retransmit in-flight packets unacked past the timeout. The seq stays the same, so the
    // receiver's duplicate suppression makes a resend indistinguishable from a duplicated
    // packet — which is exactly why resending is safe on a duplicating network.
    for (Pending& p : pending_) {
        if (p.transmitted && now_ms - p.last_sent_ms >= resend_ms_) {
            transmit(Channel::ReliableOrdered, p.seq, p.bytes);
            p.last_sent_ms = now_ms;
            ++packets_resent_;
        }
    }
    pump(now_ms); // acks may have freed window budget for queued messages
    if (ack_dirty_) {
        // Reliable traffic arrived and we sent nothing for the ack to ride home on — or what we
        // sent predates the latest arrivals. One header-only packet catches the sender up.
        transmit(Channel::AckOnly, 0, {});
        ack_dirty_ = false;
    }
}

std::size_t ReliableChannel::process_packet(std::span<const std::byte> packet,
                                            std::vector<Received>& out) {
    core::ByteReader reader(packet);
    std::uint8_t channel = 0;
    std::uint32_t seq = 0, ack = 0, ack_bits = 0;
    if (!reader.u8(channel) || !reader.u32(seq) || !reader.u32(ack) || !reader.u32(ack_bits)) {
        return 0; // too short to be one of our packets: ignore (untrusted input, clean drop)
    }
    std::span<const std::byte> payload;
    if (!reader.bytes(payload, reader.remaining())) {
        return 0; // cannot happen (remaining is by definition available), but never parse on faith
    }

    process_ack(ack, ack_bits);

    const std::size_t before = out.size();
    switch (static_cast<Channel>(channel)) {
        case Channel::ReliableOrdered:
            ack_dirty_ = true; // our ack state advanced; make sure the sender hears it
            receive_reliable(seq, payload, out);
            break;
        case Channel::UnreliableSequenced:
            receive_unreliable(seq, payload, out);
            break;
        case Channel::AckOnly:
            break; // its ack state was already processed above; no payload, no sequence space
    }
    return out.size() - before;
}

void ReliableChannel::transmit(Channel channel,
                               std::uint32_t seq,
                               std::span<const std::byte> payload) {
    // Every packet carries the current receive state (ack + ack_bits), so acks flow back on
    // whichever direction has traffic.
    packet_buf_.clear();
    core::ByteWriter writer(packet_buf_);
    writer.u8(static_cast<std::uint8_t>(channel));
    writer.u32(seq);
    writer.u32(ack());
    writer.u32(ack_bits());
    if (!payload.empty()) { // memcpy(dst, nullptr, 0) is UB by the letter of the standard
        writer.bytes(payload);
    }
    link_->send(peer_, packet_buf_);
}

// ── Sender side: ack processing ─────────────────────────────────────────────────────────
void ReliableChannel::process_ack(std::uint32_t ack, std::uint32_t ack_bits) {
    // `ack` is the peer's delivery frontier: everything before it was delivered, permanently —
    // the frontier never retreats. Bit i reports a buffered arrival at deliver_seq_ + i.
    for (auto it = pending_.begin(); it != pending_.end();) {
        const std::uint32_t s = it->seq;
        bool acked = (s < ack);
        if (!acked && s - ack < kWindow) { // unsigned: s < ack wraps huge and fails the test
            acked = ((ack_bits >> (s - ack)) & 1u) != 0;
        }
        if (acked) {
            if (it->transmitted) {
                --in_flight_;
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Receiver side: reliable-ordered ─────────────────────────────────────────────────────
void ReliableChannel::receive_reliable(std::uint32_t seq,
                                       std::span<const std::byte> payload,
                                       std::vector<Received>& out) {
    if (seq < deliver_seq_) {
        return; // already delivered — a duplicate or a resend of it (still acked via ack state)
    }
    if (seq >= deliver_seq_ + kWindow) {
        return; // beyond the window; the sender's own window means it will be (re)sent in time
    }
    if (mark_received(seq)) {
        return; // already buffered — a duplicate
    }

    if (seq != deliver_seq_) {
        // Out of order: hold until the gap fills. The window bounds this buffer.
        early_.emplace_back(seq, std::vector<std::byte>(payload.begin(), payload.end()));
        return;
    }

    // In order: deliver, then release everything the arrival made contiguous, in sequence.
    deliver(out, payload);
    ++deliver_seq_;
    while (!early_.empty()) {
        const auto it = std::find_if(
            early_.begin(), early_.end(), [&](const auto& e) { return e.first == deliver_seq_; });
        if (it == early_.end()) {
            break; // the next seq hasn't arrived — wait for it (or its resend)
        }
        deliver(out, it->second);
        early_.erase(it);
        ++deliver_seq_;
    }
}

void ReliableChannel::deliver(std::vector<Received>& out, std::span<const std::byte> payload) {
    out.push_back(
        Received{Channel::ReliableOrdered, std::vector<std::byte>(payload.begin(), payload.end())});
    ++messages_delivered_;
}

// ── Receiver side: unreliable-sequenced ─────────────────────────────────────────────────
void ReliableChannel::receive_unreliable(std::uint32_t seq,
                                         std::span<const std::byte> payload,
                                         std::vector<Received>& out) {
    if (have_unreliable_ && seq <= latest_unreliable_seq_) {
        return; // stale or duplicate: a fresher packet already made this one garbage
    }
    latest_unreliable_seq_ = seq;
    have_unreliable_ = true;
    out.push_back(Received{Channel::UnreliableSequenced,
                           std::vector<std::byte>(payload.begin(), payload.end())});
    ++messages_delivered_;
}

// ── Receive bookkeeping: the ack state every outgoing packet reports ────────────────────
// A 64-bit ring of "received" flags covering [window_base_, window_base_+63]; bit i tracks seq
// window_base_+i. Only the kWindow (32) seqs past the delivery frontier are ever marked, so the
// ring is generously sized. Returns true if the seq was ALREADY marked (a duplicate).
bool ReliableChannel::mark_received(std::uint32_t seq) {
    if (seq < window_base_) {
        return true; // older than the ring: treat as seen (suppresses ancient duplicates)
    }
    if (seq - window_base_ >= 64) {
        // Slide the ring forward so `seq` becomes its newest entry, discarding the oldest bits.
        const std::uint32_t slide = seq - window_base_ - 63;
        received_bits_ = slide >= 64 ? 0 : (received_bits_ >> slide);
        window_base_ += slide;
    }
    const std::uint64_t mask = 1ull << (seq - window_base_);
    const bool already = (received_bits_ & mask) != 0;
    received_bits_ |= mask;
    return already;
}

// The ack anchor: the delivery frontier itself. 0 means "nothing delivered yet" — and clears
// nothing on the sender, which is exactly right (design point 2a at the top of this file).
std::uint32_t ReliableChannel::ack() const noexcept {
    return deliver_seq_;
}

// Bit i reports received(deliver_seq_ + i), for i in [0, kWindow).
std::uint32_t ReliableChannel::ack_bits() const noexcept {
    std::uint32_t bits = 0;
    for (std::uint32_t i = 0; i < kWindow; ++i) {
        const std::uint32_t s = deliver_seq_ + i;
        if (s >= window_base_ && s - window_base_ < 64 &&
            (received_bits_ >> (s - window_base_)) & 1ull) {
            bits |= 1u << i;
        }
    }
    return bits;
}

} // namespace rime::net
