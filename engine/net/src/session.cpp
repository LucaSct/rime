// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// One peer relationship (m11.2, ADR-0033 §7). The interesting content here is the timer state
// machine at the bottom — everything above it is the plumbing that lets ReliableChannel run,
// unmodified, over a wire that now carries an incarnation salt.

#include "rime/net/session.hpp"

#include <utility>

#include "rime/core/byte_cursor.hpp"

namespace rime::net {

// ── The salt-stamping adapter ───────────────────────────────────────────────────────────
bool Session::SessionLink::send(const Endpoint& to, std::span<const std::byte> data) {
    // Prepend this incarnation's salt and hand the framed datagram to the real link. The channel
    // above is oblivious: it wrote its 13-byte header into `data` and has no idea four more bytes
    // precede it on the wire.
    scratch_.clear();
    core::ByteWriter writer(scratch_);
    writer.u32(salt_);
    if (!data.empty()) { // memcpy(dst, nullptr, 0) is UB by the letter of the standard
        writer.bytes(data);
    }
    return inner_->send(to, scratch_);
}

std::size_t Session::SessionLink::receive(std::vector<Datagram>&) {
    // Deliberately a dead end: the NetDriver polls the ONE real link and routes by endpoint. If a
    // session drained the link itself it would eat every other session's packets — the exact bug
    // ReliableChannel's header warned about before sessions existed.
    return 0;
}

// ── Construction ────────────────────────────────────────────────────────────────────────
Session::Session(SessionId id,
                 const Endpoint& peer,
                 Link& link,
                 std::uint32_t salt,
                 std::uint32_t client_salt,
                 std::uint32_t server_salt,
                 std::uint64_t resend_ms,
                 const SessionTiming& timing,
                 SessionState initial_state,
                 std::vector<std::byte> connect_request,
                 std::uint64_t now_ms)
    : id_(id), peer_(peer), timing_(timing), state_(initial_state), client_salt_(client_salt),
      server_salt_(server_salt), link_(link, salt), channel_(link_, peer, resend_ms),
      connect_request_(std::move(connect_request)), last_recv_ms_(now_ms), last_sent_ms_(now_ms) {
    if (state_ == SessionState::Connecting) {
        attempts_left_ = timing_.connect_attempts;
        // Deadline in the past (== now) so the FIRST update() transmits the request. Keeping every
        // send in update() means there is exactly one place that touches the wire on a timer.
        next_action_ms_ = now_ms;
    }
}

// ── The game-facing surface ─────────────────────────────────────────────────────────────
bool Session::send_reliable(std::span<const std::byte> message, std::uint64_t now_ms) {
    if (state_ != SessionState::Connected) {
        return false; // half-open: these bytes would land in a stranger's sequence space
    }
    if (!channel_.send_reliable(message, now_ms)) {
        return false; // over kMaxPayload, or the peer stopped acking (kMaxPending backpressure)
    }
    last_sent_ms_ = now_ms;
    return true;
}

bool Session::send_unreliable(std::span<const std::byte> message, std::uint64_t now_ms) {
    if (state_ != SessionState::Connected) {
        return false;
    }
    if (!channel_.send_unreliable(message)) {
        return false;
    }
    last_sent_ms_ = now_ms;
    return true;
}

std::size_t Session::drain_received(std::vector<Received>& out) {
    const std::size_t count = inbox_.size();
    for (Received& message : inbox_) {
        out.push_back(std::move(message));
    }
    inbox_.clear();
    return count;
}

void Session::disconnect() noexcept {
    if (state_ == SessionState::Closing) {
        return; // already leaving; asking twice does not make it more graceful
    }
    state_ = SessionState::Closing;
    attempts_left_ = timing_.linger_sends;
    next_action_ms_ = 0; // announce on the next update()
}

// ── Driver entry points ─────────────────────────────────────────────────────────────────
void Session::on_channel_packet(std::span<const std::byte> payload, std::uint64_t now_ms) {
    // The driver already checked the salt, so these bytes provably belong to THIS incarnation.
    last_recv_ms_ = now_ms;

    // An empty vector allocates nothing, so the common "packet carried only an ack" case costs no
    // heap traffic; a packet that really delivers messages was always going to allocate for their
    // bytes anyway.
    std::vector<Received> delivered;
    channel_.process_packet(payload, delivered);
    for (Received& message : delivered) {
        inbox_.push_back(std::move(message));
    }
}

void Session::promote(std::uint32_t incarnation_salt, std::uint64_t now_ms) {
    // The accept arrived: swap the provisional client_salt for the folded incarnation salt that
    // both sides will stamp from here on, and open the channel for traffic.
    link_.set_salt(incarnation_salt);
    state_ = SessionState::Connected;
    last_recv_ms_ = now_ms;
    attempts_left_ = 0;
    connect_request_.clear();
    connect_request_.shrink_to_fit(); // the retry buffer is dead weight once connected
}

void Session::send_control(std::span<const std::byte> payload, std::uint64_t now_ms) {
    (void)link_.send(peer_, payload); // the adapter stamps the salt; loss is UDP's business
    last_sent_ms_ = now_ms;
}

// ── The timer state machine ─────────────────────────────────────────────────────────────
std::optional<DisconnectReason> Session::update(std::uint64_t now_ms) {
    switch (state_) {
        case SessionState::Connecting:
            // Re-send the byte-identical request on a cadence until someone answers. Because the
            // bytes are identical (same client_salt), a server that already accepted recognizes a
            // retry as the SAME attempt and simply re-sends its accept — which is what makes a lost
            // accept self-healing without either side keeping extra state.
            if (now_ms >= next_action_ms_) {
                if (attempts_left_ <= 0) {
                    return DisconnectReason::ConnectTimeout;
                }
                --attempts_left_;
                send_control(connect_request_, now_ms);
                next_action_ms_ = now_ms + timing_.connect_retry_ms;
            }
            return std::nullopt;

        case SessionState::Connected:
            // Peer death first: if the peer has gone silent there is no point resending to it.
            if (now_ms - last_recv_ms_ >= timing_.timeout_ms) {
                return DisconnectReason::Timeout;
            }
            channel_.update(now_ms); // resends, queued sends, and the channel's own AckOnly
            // Keepalive exists only for an IDLE connection: any traffic we sent this interval
            // already told the peer we are alive, so it suppresses the heartbeat. (A resend issued
            // by channel_.update above does not stamp last_sent_ms_, so a busy-but-stalled link may
            // still emit one — harmless, and cheaper than threading the channel's send state out.)
            if (now_ms - last_sent_ms_ >= timing_.heartbeat_interval_ms) {
                send_control(encode_heartbeat(), now_ms);
                ++heartbeats_sent_;
            }
            return std::nullopt;

        case SessionState::Closing:
            // Announce the departure once per tick, a few times, because a single datagram is a
            // coin-flip on a lossy link. Then go, without waiting for an answer: removal is
            // unilateral, and if every copy was lost the peer's own timeout collects us.
            if (attempts_left_ <= 0) {
                return DisconnectReason::LocalClose;
            }
            --attempts_left_;
            send_control(encode_disconnect(DisconnectReason::Graceful), now_ms);
            return std::nullopt;
    }
    return std::nullopt; // unreachable; a total switch keeps a new state from silently doing
                         // nothing
}

} // namespace rime::net
