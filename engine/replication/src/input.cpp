// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/input.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "rime/core/byte_cursor.hpp"

namespace rime::replication {
namespace {

// Pitch is clamped just inside the poles rather than at them. Exactly ±π/2 makes the view's forward
// vector parallel to the world up axis, and every look-at basis built from those two degenerates —
// the classic gimbal snap where the camera rolls violently at the zenith. A hair short costs
// nothing a player can perceive and keeps the cross product well-conditioned.
constexpr float kMaxPitch = std::numbers::pi_v<float> / 2.0f - 0.001f;

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

[[nodiscard]] bool finite(float v) noexcept {
    return std::isfinite(v);
}

// Fold an angle into (-π, π]. Done on the client when sampling and again on the server when
// validating: the client so the number it sends stays small however long the player spins, the
// server because it cannot assume the client did.
[[nodiscard]] float wrap_angle(float radians) noexcept {
    const float wrapped = std::remainder(radians, kTwoPi);
    return wrapped;
}

} // namespace

bool sanitize(InputCommand& command) noexcept {
    bool changed = false;

    // Non-finite first, and by replacement rather than rejection. A NaN compares false against
    // every bound, so a clamp written as std::clamp would pass it straight through — the trap that
    // makes "we validate our inputs" untrue in a lot of code that looks like it does.
    for (float* field : {&command.move_x, &command.move_y, &command.yaw, &command.pitch}) {
        if (!finite(*field)) {
            *field = 0.0f;
            changed = true;
        }
    }

    // Movement intent lives in the unit disc. Clamping each axis separately would leave a diagonal
    // at length √2 — the "strafe-running is faster" bug that has shipped in more games than it
    // should have, and here it would be a client-chosen speed multiplier rather than a quirk.
    const float length_sq = command.move_x * command.move_x + command.move_y * command.move_y;
    if (length_sq > 1.0f) {
        const float scale = 1.0f / std::sqrt(length_sq);
        command.move_x *= scale;
        command.move_y *= scale;
        changed = true;
    }

    const float wrapped_yaw = wrap_angle(command.yaw);
    if (wrapped_yaw != command.yaw) {
        command.yaw = wrapped_yaw;
        changed = true;
    }

    const float clamped_pitch = std::clamp(command.pitch, -kMaxPitch, kMaxPitch);
    if (clamped_pitch != command.pitch) {
        command.pitch = clamped_pitch;
        changed = true;
    }

    return changed;
}

// ── InputSampler ────────────────────────────────────────────────────────────────────────────────

void InputSampler::set_bindings(std::span<const ActionBinding> actions,
                                std::span<const AxisBinding> axes) {
    actions_.assign(actions.begin(), actions.end());
    axes_.assign(axes.begin(), axes.end());
}

bool InputSampler::is_held(platform::Key key) const noexcept {
    const auto index = static_cast<std::size_t>(key);
    if (index >= static_cast<std::size_t>(platform::Key::Count)) {
        return false;
    }
    return (held_keys_[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
}

void InputSampler::clear_held() noexcept {
    held_keys_.fill(0);
}

void InputSampler::accumulate(std::span<const platform::Event> events) {
    for (const platform::Event& event : events) {
        switch (event.type) {
            case platform::EventType::KeyDown: {
                const auto index = static_cast<std::size_t>(event.key.key);
                if (index >= static_cast<std::size_t>(platform::Key::Count)) {
                    break;
                }
                const std::uint64_t mask = std::uint64_t{1} << (index % 64);
                const bool was_held = (held_keys_[index / 64] & mask) != 0;
                held_keys_[index / 64] |= mask;

                // An edge is a TRANSITION, so it is recorded only on the down that actually changed
                // the state. The OS auto-repeat delivers a stream of KeyDowns while a key is held
                // (event.key.repeat marks them), and counting those as presses would turn one
                // trigger pull into thirty.
                if (!was_held && !event.key.repeat) {
                    for (const ActionBinding& binding : actions_) {
                        if (binding.key == event.key.key) {
                            pending_pressed_ |= binding.bit;
                        }
                    }
                }
                break;
            }
            case platform::EventType::KeyUp: {
                const auto index = static_cast<std::size_t>(event.key.key);
                if (index < static_cast<std::size_t>(platform::Key::Count)) {
                    held_keys_[index / 64] &= ~(std::uint64_t{1} << (index % 64));
                }
                break;
            }
            case platform::EventType::MouseMove:
                // Relative motion, not absolute position: a view angle integrates the deltas, and
                // the absolute cursor position is meaningless once the pointer is captured.
                yaw_ = wrap_angle(yaw_ - event.mouse_move.dx * mouse_sensitivity_);
                pitch_ = std::clamp(
                    pitch_ + event.mouse_move.dy * mouse_sensitivity_, -kMaxPitch, kMaxPitch);
                break;
            case platform::EventType::WindowFocus:
                if (!event.focus.focused) {
                    clear_held();
                }
                break;
            default:
                break;
        }
    }
}

InputCommand InputSampler::build(std::uint32_t sequence) {
    InputCommand command{};
    command.sequence = sequence;
    command.yaw = yaw_;
    command.pitch = pitch_;

    for (const AxisBinding& binding : axes_) {
        if (!is_held(binding.key)) {
            continue;
        }
        if (binding.axis == 0) {
            command.move_x += binding.scale;
        } else {
            command.move_y += binding.scale;
        }
    }

    for (const ActionBinding& binding : actions_) {
        if (is_held(binding.key)) {
            command.held |= binding.bit;
        }
    }

    command.pressed = pending_pressed_;

    // Opposite keys held together sum to zero and a diagonal sums to √2, so the sample needs the
    // same disc normalization the server will apply. Doing it here as well is not redundant: it
    // means an honest client's commands never register as sanitized, which is what makes that
    // counter a signal about the PEER rather than about our own binding arithmetic.
    (void)sanitize(command);

    // Edges are consumed by the command that reports them; levels are not. Held keys and view
    // angles deliberately survive — they are the state this class exists to integrate.
    pending_pressed_ = 0;
    return command;
}

std::span<const InputSampler::ActionBinding> default_action_bindings() noexcept {
    static constexpr std::array<InputSampler::ActionBinding, 2> kBindings{{
        {platform::Key::Space, 1u << 0},    // primary action
        {platform::Key::LeftCtrl, 1u << 1}, // secondary action
    }};
    return kBindings;
}

std::span<const InputSampler::AxisBinding> default_axis_bindings() noexcept {
    static constexpr std::array<InputSampler::AxisBinding, 4> kBindings{{
        {platform::Key::W, 1, 1.0f},
        {platform::Key::S, 1, -1.0f},
        {platform::Key::D, 0, 1.0f},
        {platform::Key::A, 0, -1.0f},
    }};
    return kBindings;
}

// ── ClientInputSender ───────────────────────────────────────────────────────────────────────────

const InputCommand& ClientInputSender::record(InputCommand command) {
    command.sequence = next_sequence_++;
    ++commands_sent_;

    if (unacked_.size() >= kMaxUnackedCommands) {
        // Drop the OLDEST. The bound is reached only when the server has stopped acknowledging for
        // two seconds, at which point the oldest commands are the least likely to still matter and
        // the session's own timeout is already the real answer to what is happening.
        unacked_.erase(unacked_.begin());
        ++commands_evicted_;
    }
    unacked_.push_back(command);
    return unacked_.back();
}

void ClientInputSender::set_redundancy(std::size_t commands_per_packet) noexcept {
    redundancy_ = std::clamp(commands_per_packet, std::size_t{1}, kMaxCommandsPerPacket);
}

void ClientInputSender::send(net::NetDriver& driver, std::uint64_t now_ms) {
    if (unacked_.empty()) {
        return; // nothing recorded yet this session; an empty window is not worth a datagram
    }

    // The window is the newest `redundancy_` UN-ACKED commands, and drawing it from that list
    // rather than from a separate history gives the mechanism a property worth naming: it widens
    // exactly when it is needed. Acknowledgements advance only as far as the server has consumed,
    // so a stretch of loss leaves the un-acked list long and every packet repeats more; a clean
    // link keeps it one or two deep and the repetition costs almost nothing. Nothing tunes that —
    // it falls out of what "un-acked" means.
    //
    // Retiring on the ack is also what stops the window repeating input the server is already done
    // with, which on a long session would otherwise be the whole match.
    const std::size_t count = std::min(redundancy_, unacked_.size());
    const std::span<const InputCommand> window{unacked_.end() - static_cast<std::ptrdiff_t>(count),
                                               count};

    scratch_.clear();
    core::ByteWriter writer{scratch_};
    writer.u8(static_cast<std::uint8_t>(MessageTag::InputCommands));
    writer.u8(static_cast<std::uint8_t>(count));
    for (const InputCommand& command : window) {
        writer.u32(command.sequence);
        writer.f32(command.move_x);
        writer.f32(command.move_y);
        writer.f32(command.yaw);
        writer.f32(command.pitch);
        writer.u32(command.held);
        writer.u32(command.pressed);
    }

    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session != nullptr && session->state() == net::SessionState::Connected) {
            (void)session->send_unreliable(scratch_, now_ms, kStreamInputCommands);
            ++packets_sent_;
        }
    }
}

void ClientInputSender::apply_messages(std::span<const net::Received> messages) {
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        if (!reader.u8(tag) || tag != static_cast<std::uint8_t>(MessageTag::InputAck)) {
            continue; // not ours to read — left in place for the other readers of this span
        }
        std::uint32_t consumed = 0;
        if (!reader.u32(consumed)) {
            ++malformed_;
            continue;
        }
        ++acks_received_;

        // Monotonic: acks ride the unreliable channel, so a stale one can arrive behind a fresher
        // one, and letting the frontier go backwards would resurrect commands already retired.
        acked_through_ = std::max(acked_through_, consumed);

        const auto first_live =
            std::find_if(unacked_.begin(), unacked_.end(), [this](const InputCommand& command) {
                return command.sequence > acked_through_;
            });
        unacked_.erase(unacked_.begin(), first_live);
    }
}

// ── ServerInputReceiver ─────────────────────────────────────────────────────────────────────────

ServerInputReceiver::ClientInput& ServerInputReceiver::client_for(net::SessionId id) {
    for (ClientInput& state : clients_) {
        if (state.in_use && state.id == id) {
            return state;
        }
    }
    // Reuse a freed slot before growing. A SessionId is generational, so a recycled slot cannot be
    // confused with the incarnation that used to hold it — but the buffered commands must still be
    // cleared, or the new client's first drain would hand the game its predecessor's intent.
    for (ClientInput& state : clients_) {
        if (!state.in_use) {
            state = ClientInput{};
            state.id = id;
            state.in_use = true;
            return state;
        }
    }
    ClientInput fresh{};
    fresh.id = id;
    fresh.in_use = true;
    clients_.push_back(std::move(fresh));
    return clients_.back();
}

std::size_t ServerInputReceiver::apply_messages(net::SessionId id,
                                                std::span<const net::Received> messages) {
    std::size_t accepted = 0;
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        if (!reader.u8(tag) || tag != static_cast<std::uint8_t>(MessageTag::InputCommands)) {
            continue; // another module's tag, or the baseline ack — not ours, not an error
        }
        std::uint8_t count = 0;
        if (!reader.u8(count) || count == 0 || count > kMaxCommandsPerPacket) {
            ++malformed_; // a hostile count field must not become a loop bound
            continue;
        }

        ClientInput& state = client_for(id);
        for (std::uint8_t i = 0; i < count; ++i) {
            InputCommand command{};
            const bool ok = reader.u32(command.sequence) && reader.f32(command.move_x) &&
                            reader.f32(command.move_y) && reader.f32(command.yaw) &&
                            reader.f32(command.pitch) && reader.u32(command.held) &&
                            reader.u32(command.pressed);
            if (!ok) {
                ++malformed_;
                break; // the rest of this packet is unparseable; the reader is bounds-checked, so
                       // nothing was read out of range to get here
            }

            // Deduplicate against the receive frontier, NOT against the consumption one. The
            // redundancy window means every command arrives up to kInputRedundancy times by design,
            // so this is the common path, not an anomaly — it is counted rather than silent so a
            // test can prove the window was actually exercised.
            if (command.sequence <= state.received_through) {
                ++commands_duplicate_;
                continue;
            }

            // A jump in the sequence is a command whose every redundant copy was lost. Count the
            // commands missed, not the gaps: one gap of three is three inputs the player made and
            // the server will never see, and that is the number worth watching.
            if (state.received_through != 0 && command.sequence > state.received_through + 1) {
                gaps_observed_ += command.sequence - state.received_through - 1;
            }
            state.received_through = command.sequence;

            if (sanitize(command)) {
                ++commands_sanitized_;
            }

            if (state.buffered.size() >= kMaxBufferedCommands) {
                // Drop the oldest un-drained command. Newest-wins is right for input: if the game
                // has stalled, the intent worth keeping when it resumes is the recent one. The
                // consumption frontier will step over what was dropped, which is why this is
                // counted — the client retires those commands believing they were acted on.
                state.buffered.erase(state.buffered.begin());
                ++commands_dropped_overflow_;
            }
            state.buffered.push_back(command);
            ++commands_accepted_;
            ++accepted;
        }
    }
    return accepted;
}

std::size_t ServerInputReceiver::apply_inbound(net::NetDriver& driver) {
    std::size_t accepted = 0;
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        accepted += apply_messages(id, inbox_);
    }
    return accepted;
}

std::size_t ServerInputReceiver::drain(net::SessionId id, std::vector<InputCommand>& out) {
    for (ClientInput& state : clients_) {
        if (!state.in_use || !(state.id == id)) {
            continue;
        }
        const std::size_t count = state.buffered.size();
        if (count == 0) {
            return 0;
        }
        // The frontier advances HERE and nowhere else: the game now has these commands, which is
        // the only evidence that justifies telling the client so.
        state.consumed_through = state.buffered.back().sequence;
        out.insert(out.end(), state.buffered.begin(), state.buffered.end());
        state.buffered.clear();
        return count;
    }
    return 0;
}

void ServerInputReceiver::send_acks(net::NetDriver& driver, std::uint64_t now_ms) {
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr || session->state() != net::SessionState::Connected) {
            continue;
        }
        const std::uint32_t consumed = consumed_through(id);
        if (consumed == 0) {
            continue; // nothing consumed yet: an ack of 0 carries no information the client lacks
        }
        scratch_.clear();
        core::ByteWriter writer{scratch_};
        writer.u8(static_cast<std::uint8_t>(MessageTag::InputAck));
        writer.u32(consumed);
        (void)session->send_unreliable(scratch_, now_ms, kStreamInputAck);
    }
}

void ServerInputReceiver::on_session_events(std::span<const net::SessionEvent> events) noexcept {
    for (const net::SessionEvent& event : events) {
        // Only the endings. A Connected event needs no answer here: a client's state is created
        // lazily by its first command, and pre-creating it would allocate a buffer for every peer
        // that connects and never sends input — which is exactly what an observer does.
        if (event.kind == net::SessionEvent::Kind::Disconnected ||
            event.kind == net::SessionEvent::Kind::ConnectFailed) {
            forget(event.id);
        }
    }
}

void ServerInputReceiver::forget(net::SessionId id) noexcept {
    for (ClientInput& state : clients_) {
        if (state.in_use && state.id == id) {
            state.in_use = false;
            state.buffered.clear();
            state.buffered.shrink_to_fit(); // clear() keeps the capacity; a dead client keeps none
            state.received_through = 0;
            state.consumed_through = 0;
        }
    }
}

std::uint32_t ServerInputReceiver::consumed_through(net::SessionId id) const noexcept {
    for (const ClientInput& state : clients_) {
        if (state.in_use && state.id == id) {
            return state.consumed_through;
        }
    }
    return 0;
}

} // namespace rime::replication
