// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// target-range's content module: one component, and the registrar that names it.
//
// A HEADER rather than a definition inside `main.cpp`, and the reason is the whole of m15.8. Two
// binaries need this type: the game (`target_range`) and the game's own editor host
// (`target-range-host`). Without the second one the editor cannot author this game at all — see
// `host_main.cpp` for why, and for the engine refusal that proves it.
//
// This is what `worldkit::register_engine_components`'s comment means by "a game adds its content
// module's on top" (`engine/worldkit/include/rime/worldkit/profile.hpp:45-48`). `blockkit` is the
// block's content module; this file is this game's, and it is four fields long.
#pragma once

#include <cstdint>

#include "rime/core/reflect.hpp"
#include "rime/ecs/world.hpp"

namespace targetrange {

// A crate that counts. Authored per entity in the editor: `points` is what it is worth and
// `required` marks a target the range cannot be completed without.
//
// `down` is state rather than authoring, and it is reflected anyway so a half-finished range can be
// saved and resumed, and so the editor's inspector can show it. A field the game writes and the
// editor can see is the normal case, not a special one.
struct Target {
    std::int32_t points = 10;
    bool required = true;
    bool down = false;
    // Impulse applied when hit, so a crate visibly leaves its stand rather than blinking out. In
    // the component rather than a constant because a heavy crate and a light one are an authoring
    // choice.
    float knock = 6.0f;
};

// Register this game's components. Called AFTER `worldkit::register_engine_components`, and the
// order is shared with every other Rime game on purpose: `ecs::component_schema_hash` goes into the
// net driver's config, and two peers that registered different sets refuse to connect.
inline std::size_t register_content_components(rime::ecs::World& world) {
    (void)world.register_component<Target>();
    return 1;
}

} // namespace targetrange

RIME_REFLECT_BEGIN(targetrange::Target)
RIME_REFLECT_FIELD(points)
RIME_REFLECT_FIELD(required)
RIME_REFLECT_FIELD(down)
RIME_REFLECT_FIELD(knock)
RIME_REFLECT_END()
