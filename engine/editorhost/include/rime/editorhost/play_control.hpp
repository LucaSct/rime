// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rime::editorhost {

// Optional play-control handshake (PlayControl 0x021E editor->engine, PlayControlState 0x020B
// engine->editor). Why it is a negotiation and not a button: an editor cannot know whether the
// host it launched has a player controller at all, and a captured pointer is a state the editor
// must never *assume*. So the host advertises `supported_modes` (bit0 Inspect is mandatory, bit1
// Player is optional) and the editor only offers Player when it sees bit1. Every state carries a
// `session_id` so a command or a reply that outlived its play session can be recognised as stale
// by the runtime; this codec only carries the id faithfully and does not judge staleness. A host
// that predates this message simply drops the unknown types (forward-compat rule, protocol.hpp),
// which the editor reads as "Inspect only".
enum class PlayControlAction : std::uint8_t {
    SelectInspect = 0,
    SelectPlayer = 1,
    Capture = 2,
    Release = 3
};

// editor -> engine. Wire: action:u8, session_id:u64 (little-endian) — 9 bytes.
struct PlayControlMsg {
    PlayControlAction action = PlayControlAction::SelectInspect;
    std::uint64_t session_id = 0;
};

// engine -> editor. mode: 0 Inspect, 1 Player. phase: 0 Edit, 1 Playing, 2 Paused.
// status: 0 Ready, 1 Active, 2 Refused.
struct PlayControlStateMsg {
    static constexpr std::size_t max_reason_bytes = 1024;
    std::uint8_t supported_modes = 1;
    std::uint8_t mode = 0;
    std::uint8_t phase = 0;
    std::uint8_t status = 0;
    bool captured = false;
    std::uint64_t session_id = 0;
    std::uint64_t tick_count = 0;
    std::string reason; // UTF-8, at most max_reason_bytes
};

// Serializers return an empty vector for an invalid message (never a half-valid packet); parsers
// validate into a candidate and commit to `out` only on success (transactional).
[[nodiscard]] std::vector<std::byte> serialize_play_control(const PlayControlMsg& msg);
[[nodiscard]] bool parse_play_control(std::span<const std::byte> payload, PlayControlMsg& out);

// Wire: supported_modes:u8, mode:u8, phase:u8, status:u8, captured:u8, session_id:u64,
// tick_count:u64, reason_length:u32, reason bytes.
[[nodiscard]] std::vector<std::byte> serialize_play_control_state(const PlayControlStateMsg& msg);
[[nodiscard]] bool parse_play_control_state(std::span<const std::byte> payload,
                                            PlayControlStateMsg& out);

} // namespace rime::editorhost
