// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/gameplay/character.hpp"
#include "rime/replication/input.hpp"

// The wire ↔ mover conversion (m12.3/m12.4, ADR-0035 §3).
//
// `gameplay::CharacterInput` and `replication::InputCommand` are the same fields with different
// owners: the mover's types are mirrored rather than shared so a single-player build can delete the
// networking stack (character.hpp). ADR-0035 §3 says "`gameplay_net` converts", which makes this
// module the owner of the conversion — and this header its one home.
//
// IT HAS ONE HOME BECAUSE RECONCILIATION DEPENDS ON IT. The server runs `step_character` over the
// commands it consumed; the client replays `step_character` over the same commands after a
// correction. Those two runs are only comparable if the bytes on the wire become the SAME
// `CharacterInput` on both sides. A second copy of these six assignments, drifting by one field,
// would present as a predictor that corrects forever on a link with no loss at all — a divergence
// with no divergent input behind it, which is about the worst-shaped bug in this area.
namespace rime::gameplay_net {

// `sequence` deliberately does NOT cross. It is the network's bookkeeping, not the mover's, and
// leaving it out is what keeps a replay tape a statement about intent rather than about a
// particular session.
[[nodiscard]] inline gameplay::CharacterInput
to_character_input(const replication::InputCommand& command) noexcept {
    gameplay::CharacterInput input;
    input.move_x = command.move_x;
    input.move_y = command.move_y;
    input.yaw = command.yaw;
    input.pitch = command.pitch;
    input.held = command.held;
    input.pressed = command.pressed;
    return input;
}

} // namespace rime::gameplay_net
