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

namespace {

// FNV-1a continued over a fixed marker, then a version's four little-endian bytes -- the same
// construction `input_schema_hash` uses, so every fold is one platform-independent function.
void fold_marker(std::uint64_t& hash, std::string_view marker) {
    for (const char c : marker) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ull;
    }
}

void fold_u32(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= 0x100000001b3ull;
    }
}

} // namespace

std::uint64_t input_schema_identity(const GameDefinition::NetworkInputProfile& input,
                                    const GameDefinition::RemotePlayProfile* remote_play,
                                    std::uint16_t max_players) {
    std::uint64_t hash =
        remote_play != nullptr
            ? replication::input_schema_hash(
                  input.profile_id, input.profile_version, remote_play->fixed_dt)
            : replication::input_schema_hash(input.profile_id, input.profile_version);
    // Without a spawn profile the hash stays m20.4's identity, bit for bit: a game that does not
    // opt in is unchanged.
    if (remote_play != nullptr && remote_play->spawn_prediction) {
        // Speculative spawn changes what a checkpoint IS (it gains an extension an m20.4 decoder
        // refuses), so it is part of the input identity: a fixed marker and the profile version,
        // so bumping the spawn version is a schema refusal.
        fold_marker(hash, "rime.spawn_prediction");
        fold_u32(hash, remote_play->spawn_prediction->version);
        // An active interaction group (m20.6b) changes what a checkpoint's roster may hold -- a
        // physics object with 52 state bytes beside the player -- so it negotiates the same way.
        // Inactive or absent: m20.5's identity, bit for bit.
        if (const auto* interaction = remote_play->active_interaction()) {
            fold_marker(hash, "rime.interaction");
            fold_u32(hash, interaction->version);
        }
    }
    // m20.8b: a multi-seat game changes what the hello reply IS (codec 3 carries a seat). The
    // codec range already refuses a codec-2 client as `Version`; folding presence here as well
    // means two builds of one game that disagree about seating refuse as `Schema` even if a
    // future client offers both codecs. One-player: m20.7's identity, bit for bit.
    if (max_players > 1)
        fold_marker(hash, "rime.seats");
    return hash;
}

} // namespace rime::app
