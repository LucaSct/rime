// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine's half of "input is actions, never keys" (m20.1): the table-driven InputMapper that
// turns a frame's platform events into the ActionState a game's fixed tick consumes. See
// game_definition.hpp for why the two halves run at different rates.

#include "rime/app/game_definition.hpp"

#include <string_view>
#include <utility>

#include "rime/replication/input_v2.hpp"

namespace rime::app {
namespace {

[[nodiscard]] constexpr std::size_t key_index(platform::Key key) noexcept {
    return static_cast<std::size_t>(key);
}

[[nodiscard]] constexpr bool key_in_range(platform::Key key) noexcept {
    return key_index(key) < key_index(platform::Key::Count);
}

} // namespace

InputMapper::InputMapper(InputMap map) : map_(std::move(map)) {
    // Validate the table ONCE, here, and drop what cannot be honoured — counted, so a game whose
    // binding table names axis 9 of 8 finds out from a number rather than from a key that silently
    // does nothing. Checking on every event instead would pay per keystroke for a mistake that can
    // only be made at build time.
    auto bad_axis = [](const AxisBinding& b) {
        return b.axis >= ActionState::kAxes || !key_in_range(b.key);
    };
    auto bad_button = [](const ButtonBinding& b) {
        return b.button >= ActionState::kButtons || !key_in_range(b.key);
    };
    rejected_ += std::erase_if(map_.axes, bad_axis);
    rejected_ += std::erase_if(map_.buttons, bad_button);
}

void InputMapper::update(std::span<const platform::Event> events) {
    for (const platform::Event& e : events) {
        const bool down = e.type == platform::EventType::KeyDown;
        if (!down && e.type != platform::EventType::KeyUp) {
            continue; // not a key: window/mouse events are the engine's business, not the map's
        }
        if (!key_in_range(e.key.key)) {
            continue;
        }
        // A press is an EDGE: up→down, and never an auto-repeat. Recorded before the held state
        // changes so the comparison sees the state the key was in before this event.
        const bool was_down = down_[key_index(e.key.key)];
        if (down && !was_down && !e.key.repeat) {
            for (const ButtonBinding& b : map_.buttons) {
                if (b.key == e.key.key) {
                    pressed_ |= 1u << b.button;
                }
            }
        }
        down_[key_index(e.key.key)] = down;
    }
}

ActionState InputMapper::take() noexcept {
    // Recomputed from the held keys each time rather than accumulated per event, so the answer is
    // a pure function of "which keys are down now" — a missed KeyUp cannot leave an axis stuck at a
    // value no key is producing.
    ActionState s{};
    for (const AxisBinding& b : map_.axes) {
        if (down_[key_index(b.key)]) {
            s.axes[b.axis] += b.value;
        }
    }
    for (const ButtonBinding& b : map_.buttons) {
        if (down_[key_index(b.key)]) {
            s.held |= 1u << b.button;
        }
    }
    s.pressed = pressed_;
    pressed_ = 0; // an edge is delivered to exactly one tick
    return s;
}

std::uint64_t input_schema_identity(const GameDefinition::NetworkInputProfile& input,
                                    const GameDefinition::RemotePlayProfile* remote_play) {
    const std::uint64_t base =
        remote_play != nullptr
            ? replication::input_schema_hash(
                  input.profile_id, input.profile_version, remote_play->fixed_dt)
            : replication::input_schema_hash(input.profile_id, input.profile_version);
    if (remote_play == nullptr || !remote_play->spawn_prediction)
        return base; // m20.4's identity, bit for bit: a game that does not opt in is unchanged
    // Speculative spawn changes what a checkpoint IS (it gains an extension an m20.4 decoder
    // refuses), so it is part of the input identity. FNV-1a continued from the m20.4 hash over a
    // fixed marker and the profile version's four little-endian bytes -- the same construction
    // `input_schema_hash` uses, so the result is one platform-independent function of
    // (profile, version, dt, spawn version), and bumping the spawn version is a schema refusal.
    std::uint64_t hash = base;
    const auto mix = [&hash](std::uint8_t byte) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    };
    for (const char c : std::string_view{"rime.spawn_prediction"})
        mix(static_cast<std::uint8_t>(c));
    const std::uint32_t version = remote_play->spawn_prediction->version;
    for (unsigned shift = 0; shift < 32; shift += 8)
        mix(static_cast<std::uint8_t>(version >> shift));
    const auto* interaction = remote_play->active_interaction();
    if (interaction == nullptr)
        return hash; // m20.5's identity, bit for bit
    // An active interaction group (m20.6b) changes what a checkpoint's roster may hold -- a
    // physics object with 52 state bytes beside the player -- so it negotiates the same way:
    // continue the same FNV-1a over a second marker and its version.
    for (const char c : std::string_view{"rime.interaction"})
        mix(static_cast<std::uint8_t>(c));
    for (unsigned shift = 0; shift < 32; shift += 8)
        mix(static_cast<std::uint8_t>(interaction->version >> shift));
    return hash;
}

} // namespace rime::app
