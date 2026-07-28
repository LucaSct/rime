// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The session driver (m11.2, ADR-0033 §7): one Link, N peers, one handshake.
//
// The shape of update() is the thing to read first. It is deliberately two passes:
//
//   1. ROUTE — drain the link once and give every datagram to whoever owns it, after three gates
//      (frame, salt, identity) that are ordered cheapest-first so hostile input is refused before
//      it can cost anything.
//   2. TICK — run every session's timers, reap whatever died, and rebuild the id list.
//
// Splitting them means event order is slot order rather than packet-arrival order, which is what
// makes a test's expectations stable under a reordering network.

#include "rime/net/net_driver.hpp"

#include <memory>
#include <string>
#include <utility>

#include "rime/core/byte_cursor.hpp"

namespace rime::net {

namespace {

// Combine the two halves of an incarnation. FNV-1a rather than xor, for one specific reason: xor
// maps client == server to 0, and 0 is kNoSalt — the value reserved for "no session". A one-in-four
// -billion coincidence that silently disables the incarnation check is not a risk worth taking to
// save four lines.
[[nodiscard]] std::uint32_t fold_salts(std::uint32_t client, std::uint32_t server) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const std::uint64_t combined = (static_cast<std::uint64_t>(client) << 32) | server;
    for (int shift = 56; shift >= 0; shift -= 8) {
        hash ^= (combined >> shift) & 0xFFull;
        hash *= 0x100000001b3ull;
    }
    const auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return folded == kNoSalt ? 1u : folded;
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "0x";
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<std::size_t>((value >> shift) & 0xFull);
        if (nibble == 0 && leading && shift != 0) {
            continue; // trim leading zeros; a 16-digit hash is harder to eyeball than a short one
        }
        leading = false;
        out.push_back(kDigits[nibble]);
    }
    return out;
}

[[nodiscard]] std::string endpoint_string(const Endpoint& endpoint) {
    return endpoint.address_string() + ":" + std::to_string(endpoint.port);
}

} // namespace

std::string format(const SessionEvent& event) {
    const std::string who = endpoint_string(event.endpoint);
    switch (event.kind) {
        case SessionEvent::Kind::Connected:
            return "connected to " + who;
        case SessionEvent::Kind::Disconnected:
            return "disconnected from " + who + ": " + std::string(to_string(event.reason));
        case SessionEvent::Kind::ConnectFailed:
            break;
    }

    // A rejection is the one event a human is expected to ACT on, so it carries both numbers and
    // says what to do about them — the same discipline as the asset layer's "schema hash mismatch
    // (re-cook needed)".
    const std::string head =
        "connection to " + who + " failed: " + std::string(to_string(event.reason));
    switch (event.reason) {
        case DisconnectReason::SchemaMismatch:
            return head + " (server schema " + hex64(event.expected) + ", client schema " +
                   hex64(event.actual) +
                   ") — the client is a different build; rebuild/re-cook against the server's "
                   "component set";
        case DisconnectReason::ProtocolMismatch:
            return head + " (server protocol " + std::to_string(event.expected) +
                   ", client protocol " + std::to_string(event.actual) +
                   ") — one side is a stale binary";
        case DisconnectReason::AppMismatch:
            return head + " (server app id " + hex64(event.expected) + ", client app id " +
                   hex64(event.actual) + ") — these are two different games";
        default:
            return head;
    }
}

NetDriver::NetDriver(Link& link, const Config& config) noexcept
    : link_(&link), config_(config), rng_(config.salt_seed) {
    if (rng_ == 0) {
        rng_ = 0x9E3779B97F4A7C15ull; // xorshift is a fixed point at zero — it would never advance
    }
}

std::uint32_t NetDriver::next_salt() noexcept {
    // xorshift64*, the same generator ScriptedNetwork uses and for the same reason: identical
    // output on every standard library, so a seeded test reproduces its incarnations exactly.
    for (;;) {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const auto salt = static_cast<std::uint32_t>((rng_ * 0x2545F4914F6CDD1Dull) >> 32);
        if (salt != kNoSalt) {
            return salt;
        }
    }
}

NetDriver::Slot* NetDriver::find_by_endpoint(const Endpoint& endpoint) noexcept {
    for (Slot& slot : slots_) {
        if (slot.session && slot.session->peer() == endpoint) {
            return &slot;
        }
    }
    return nullptr;
}

Session* NetDriver::session(SessionId id) noexcept {
    if (id.index >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.index];
    // The generation check is the whole point of the handle: a slot recycled by a reincarnation
    // must not answer to the dead session's id.
    if (!slot.session || slot.generation != id.generation) {
        return nullptr;
    }
    return slot.session.get();
}

void NetDriver::send_framed(const Endpoint& to,
                            std::uint32_t salt,
                            std::span<const std::byte> payload) {
    std::vector<std::byte> datagram;
    datagram.reserve(kSaltFrameSize + payload.size());
    core::ByteWriter writer(datagram);
    writer.u32(salt);
    writer.bytes(payload);
    (void)link_->send(to, datagram);
}

void NetDriver::send_reject(const Endpoint& to, const ConnectReject& reject) {
    // No session exists and none will: this is the "validate before allocate" path, and the reply
    // is the only resource the request is allowed to cost.
    send_framed(to, reject.client_salt, encode_connect_reject(reject));
}

std::optional<SessionId> NetDriver::connect(const Endpoint& server, std::uint64_t now_ms) {
    const std::uint32_t client_salt = next_salt();
    ConnectRequest request;
    request.protocol = kProtocolVersion;
    request.app_id = config_.app_id;
    request.schema_hash = config_.schema_hash;
    request.client_salt = client_salt;

    // While Connecting the session's salt IS its client_salt: it is the only value the far side
    // can recognize until the accept establishes the folded incarnation.
    Slot* slot = allocate_session(server,
                                  client_salt,
                                  client_salt,
                                  kNoSalt,
                                  SessionState::Connecting,
                                  encode_connect_request(request),
                                  now_ms);
    if (slot == nullptr) {
        return std::nullopt;
    }
    const SessionId id = slot->session->id();
    rebuild_live_ids();
    return id;
}

void NetDriver::update(std::uint64_t now_ms, std::vector<SessionEvent>& events_out) {
    // ── Pass 1: route ───────────────────────────────────────────────────────────────────
    datagrams_.clear();
    link_->receive(datagrams_);
    for (const Datagram& datagram : datagrams_) {
        core::ByteReader reader(datagram.bytes);
        std::uint32_t salt = 0;
        std::span<const std::byte> payload;
        if (!reader.u32(salt) || reader.remaining() == 0 ||
            !reader.bytes(payload, reader.remaining())) {
            ++dropped_; // shorter than a frame, or a frame with no payload: not ours
            continue;
        }

        if (is_control_tag(payload[0])) {
            on_control(datagram.from, salt, payload, now_ms, events_out);
            continue;
        }

        // Channel traffic. It is only meaningful for an established session whose incarnation
        // matches — that check is what stops a previous incarnation's in-flight packets from being
        // buffered into the fresh channel's sequence space as legitimate early traffic.
        Slot* slot = find_by_endpoint(datagram.from);
        if (slot == nullptr || slot->session->state() == SessionState::Connecting ||
            slot->session->salt() != salt) {
            ++dropped_;
            continue;
        }
        slot->session->on_channel_packet(payload, now_ms);
    }

    // ── Pass 2: tick ────────────────────────────────────────────────────────────────────
    for (Slot& slot : slots_) {
        if (!slot.session) {
            continue;
        }
        const bool connecting = slot.session->state() == SessionState::Connecting;
        if (const std::optional<DisconnectReason> reason = slot.session->update(now_ms)) {
            // A session that never completed its handshake "failed to connect"; one that did and
            // then died "disconnected". The game shows those to a player very differently.
            kill(slot,
                 *reason,
                 connecting ? SessionEvent::Kind::ConnectFailed : SessionEvent::Kind::Disconnected,
                 events_out);
        }
    }
    rebuild_live_ids();
}

void NetDriver::on_control(const Endpoint& from,
                           std::uint32_t salt,
                           std::span<const std::byte> payload,
                           std::uint64_t now_ms,
                           std::vector<SessionEvent>& out) {
    switch (static_cast<ControlTag>(payload[0])) {
        case ControlTag::ConnectRequest: {
            ++requests_seen_;
            const std::optional<ConnectRequest> request = decode_connect_request(payload);
            if (!request) {
                ++dropped_;
                return;
            }
            on_connect_request(from, *request, now_ms, out);
            return;
        }
        case ControlTag::ConnectAccept: {
            const std::optional<ConnectAccept> accept = decode_connect_accept(payload);
            Slot* slot = find_by_endpoint(from);
            // The accept is framed with our client_salt — the only value we knew when we asked.
            if (!accept || slot == nullptr || slot->session->salt() != salt) {
                ++dropped_;
                return;
            }
            on_connect_accept(*slot, *accept, now_ms, out);
            return;
        }
        case ControlTag::ConnectReject: {
            const std::optional<ConnectReject> reject = decode_connect_reject(payload);
            Slot* slot = find_by_endpoint(from);
            if (!reject || slot == nullptr || slot->session->salt() != salt ||
                slot->session->state() != SessionState::Connecting ||
                slot->session->client_salt() != reject->client_salt) {
                ++dropped_; // a rejection of somebody else's attempt, or of one we are already past
                return;
            }
            kill(*slot,
                 reject->reason,
                 SessionEvent::Kind::ConnectFailed,
                 out,
                 reject->expected,
                 reject->actual);
            return;
        }
        case ControlTag::Disconnect: {
            const std::optional<DisconnectReason> reason = decode_disconnect(payload);
            Slot* slot = find_by_endpoint(from);
            // Salt-guarded: a stale incarnation's goodbye must not murder the fresh session that
            // now occupies the same endpoint.
            if (!reason || slot == nullptr || slot->session->salt() != salt) {
                ++dropped_;
                return;
            }
            kill(*slot, DisconnectReason::Graceful, SessionEvent::Kind::Disconnected, out);
            return;
        }
        case ControlTag::Heartbeat: {
            Slot* slot = find_by_endpoint(from);
            if (!decode_heartbeat(payload) || slot == nullptr || slot->session->salt() != salt) {
                ++dropped_;
                return;
            }
            slot->session->on_heartbeat(now_ms);
            return;
        }
    }
    ++dropped_; // an unknown control tag: a newer peer, or noise. Either way, not ours to guess at.
}

void NetDriver::on_connect_request(const Endpoint& from,
                                   const ConnectRequest& request,
                                   std::uint64_t now_ms,
                                   std::vector<SessionEvent>& out) {
    if (!accepting_) {
        ++dropped_; // a client has no business accepting peers
        return;
    }

    // Identity, cheapest first, each mismatch naming itself so the rejection is actionable. All of
    // this happens BEFORE any allocation: a wrong-build or wrong-game peer costs us one datagram.
    ConnectReject reject;
    reject.client_salt = request.client_salt;
    if (request.protocol != kProtocolVersion) {
        reject.reason = DisconnectReason::ProtocolMismatch;
        reject.expected = kProtocolVersion;
        reject.actual = request.protocol;
        send_reject(from, reject);
        return;
    }
    if (request.app_id != config_.app_id) {
        reject.reason = DisconnectReason::AppMismatch;
        reject.expected = config_.app_id;
        reject.actual = request.app_id;
        send_reject(from, reject);
        return;
    }
    if (request.schema_hash != config_.schema_hash) {
        reject.reason = DisconnectReason::SchemaMismatch;
        reject.expected = config_.schema_hash;
        reject.actual = request.schema_hash;
        send_reject(from, reject);
        return;
    }

    if (Slot* existing = find_by_endpoint(from)) {
        if (existing->session->client_salt() == request.client_salt) {
            // The SAME attempt arriving again: our accept was lost (or the link duplicated the
            // request). Re-send the identical accept and allocate nothing — idempotent acceptance
            // is what makes a lost accept self-healing without a third handshake phase.
            const ConnectAccept accept{request.client_salt, existing->session->server_salt()};
            send_framed(from, request.client_salt, encode_connect_accept(accept));
            return;
        }
        // A different client_salt from the same endpoint means the peer re-rolled it, and the only
        // way that happens is a fresh connect() — a restarted process. It has no memory of the old
        // session, so keeping ours would leave a zombie talking to someone who moved on.
        kill(*existing, DisconnectReason::Replaced, SessionEvent::Kind::Disconnected, out);
        // `existing` may dangle after the allocation below — do not touch it again.
    }

    const std::uint32_t server_salt = next_salt();
    const std::uint32_t incarnation = fold_salts(request.client_salt, server_salt);
    Slot* slot = allocate_session(
        from, incarnation, request.client_salt, server_salt, SessionState::Connected, {}, now_ms);
    if (slot == nullptr) {
        reject.reason = DisconnectReason::ServerFull;
        reject.expected = static_cast<std::uint64_t>(config_.max_sessions);
        send_reject(from, reject);
        return;
    }

    // Framed with the client's salt: the incarnation is exactly what this packet is establishing,
    // so the client cannot recognize it yet.
    const ConnectAccept accept{request.client_salt, server_salt};
    send_framed(from, request.client_salt, encode_connect_accept(accept));

    SessionEvent event;
    event.kind = SessionEvent::Kind::Connected;
    event.id = slot->session->id();
    event.endpoint = from;
    out.push_back(event);
}

void NetDriver::on_connect_accept(Slot& slot,
                                  const ConnectAccept& accept,
                                  std::uint64_t now_ms,
                                  std::vector<SessionEvent>& out) {
    Session& session = *slot.session;
    if (session.state() != SessionState::Connecting) {
        // A duplicate or late accept for a session we already promoted. Harmless — count it as
        // liveness rather than noise, since it provably came from our peer.
        if (session.state() == SessionState::Connected &&
            accept.client_salt == session.client_salt()) {
            session.on_heartbeat(now_ms);
        } else {
            ++dropped_;
        }
        return;
    }
    if (accept.client_salt != session.client_salt()) {
        ++dropped_; // an answer to an attempt that is not ours
        return;
    }

    session.server_salt_ = accept.server_salt;
    session.promote(fold_salts(accept.client_salt, accept.server_salt), now_ms);

    SessionEvent event;
    event.kind = SessionEvent::Kind::Connected;
    event.id = session.id();
    event.endpoint = session.peer();
    out.push_back(event);
}

NetDriver::Slot* NetDriver::allocate_session(const Endpoint& peer,
                                             std::uint32_t salt,
                                             std::uint32_t client_salt,
                                             std::uint32_t server_salt,
                                             SessionState initial_state,
                                             std::vector<std::byte> connect_request,
                                             std::uint64_t now_ms) {
    std::size_t live = 0;
    std::size_t free_index = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].session) {
            ++live;
        } else if (free_index == slots_.size()) {
            free_index = i; // first reusable slot; recycling keeps the table from growing forever
        }
    }
    if (live >= config_.max_sessions) {
        return nullptr; // the bound that makes the accept path safe to reach at all
    }
    if (free_index == slots_.size()) {
        slots_.emplace_back();
    }

    Slot& slot = slots_[free_index];
    const SessionTiming timing{config_.heartbeat_interval_ms,
                               config_.timeout_ms,
                               config_.connect_retry_ms,
                               config_.connect_attempts,
                               config_.linger_sends};
    slot.session = std::make_unique<Session>(
        SessionId{static_cast<std::uint32_t>(free_index), slot.generation},
        peer,
        *link_,
        salt,
        client_salt,
        server_salt,
        config_.resend_ms,
        timing,
        initial_state,
        std::move(connect_request),
        now_ms);
    return &slot;
}

void NetDriver::kill(Slot& slot,
                     DisconnectReason reason,
                     SessionEvent::Kind kind,
                     std::vector<SessionEvent>& out,
                     std::uint64_t expected,
                     std::uint64_t actual) {
    if (!slot.session) {
        return;
    }
    SessionEvent event;
    event.kind = kind;
    event.id = slot.session->id();
    event.endpoint = slot.session->peer();
    event.reason = reason;
    event.expected = expected;
    event.actual = actual;
    out.push_back(event);

    slot.session.reset();
    // Bumping on the way OUT means every SessionId handed out for this slot is now stale, which is
    // what lets a game hold an id across frames and simply get null back when its peer is gone.
    ++slot.generation;
}

void NetDriver::rebuild_live_ids() {
    live_ids_.clear();
    for (const Slot& slot : slots_) {
        if (slot.session) {
            live_ids_.push_back(slot.session->id());
        }
    }
}

} // namespace rime::net
