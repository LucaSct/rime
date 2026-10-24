// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "rime/platform/event.hpp"

namespace rime::ecs {
class World;
}

namespace rime::physics {
class PhysicsWorld;
}

namespace rime::app {

// Borrowed for one callback, never retained by a game. The host owns the world, collision
// queries and session clock; a local preview controller owns only its disposable player state.
// This keeps editor possession out of reflected scene data and out of network authority.
struct EditorPlayContext {
    ecs::World& world;
    physics::PhysicsWorld& physics;
    std::uint64_t session_id = 0;
    std::uint64_t tick = 0;
};

struct EditorPlayAdmission {
    bool ok = true;
    std::string reason;
};

enum class EditorPlayTransition { Pause, Resume, ReleaseInput, Stop };

// An optional local-player route, separate from the existing inspect/fly PlayTick. Begin runs
// after the pre-play snapshot and ground/collider reconciliation. A refusal rolls that session
// back. Stop is delivered BEFORE bodies/world state are discarded, including disconnect/error
// teardown, so a controller can remove any transient collision it owns without dangling borrows.
// Pause and input release arrive without needing a simulation tick: held keys must not survive
// a mouse release, lost window focus or a paused editor. Resume does not create another player.
struct EditorPlayHooks {
    std::function<EditorPlayAdmission(EditorPlayContext&)> begin;
    std::function<void(EditorPlayContext&, std::span<const platform::Event>, double)> tick;
    std::function<void(EditorPlayContext&, EditorPlayTransition)> transition;

    [[nodiscard]] bool supported() const noexcept { return bool(begin) && bool(tick); }
};

} // namespace rime::app
