// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

namespace rime::ecs {
class World;
}

// `rime::worldkit` — the answer to "what components does a Rime world have?" (m14.1, ADR-0037).
//
// WHY THIS EXISTS. There was no answer. Every consumer of a world hand-assembled its own
// registration list, and the lists drifted — silently, because an unregistered component is not an
// error until something asks for it. It cost two separate failures in one afternoon:
//
//   * The EDITOR could not open the block. `engine/app/editor_host_main.cpp` registered transform +
//     render + physics; `rime-blockgen` writes `blockkit::SlabRole`; the scene load failed on the
//     newest content in the repo. Its smoke test stayed green throughout, because it used a
//     synthetic scene.
//   * `99-the-block` registered the four modules the block's SCENE needs and none of the four its
//     SESSION needs. The block stood up and drew perfectly while the predictor never seeded (no
//     `RigidBodyHandle` meant no body to predict against), destruction replicated zero ops, the
//     peers' hashes disagreed, and the mixer heard nothing. Four symptoms, one missing list, and
//     not one of them pointing at the cause.
//
// A PROFILE, NOT A REGISTRY. The obvious fix — modules self-registering into a global table — was
// rejected in ADR-0037 on two guardrails: it needs mutable global state (guardrail 4), and it turns
// "the engine still builds without this module" into a runtime question rather than a link-time one
// (guardrail 2). A profile is an ordinary function in an ordinary module that depends on everything
// it names. Delete a feature module and this fails to LINK, loudly, at the one place that has to
// know.
//
// NOTHING IN THE ENGINE DEPENDS ON THIS. It sits at the top of the cake next to the apps: samples,
// tools and the editor host call it, and no engine module does. Deleting `engine/worldkit` leaves
// the engine building — the same promise `blockkit` and `destruction_render` make.
namespace rime::worldkit {

// Register the ENGINE's component set on `world`, in one fixed order.
//
// "Engine's" is the load-bearing word. This names the components the engine itself defines —
// transforms, physics, render, destruction and its network mirror, the character controller and its
// network mirror. It deliberately does NOT name any game's or any content module's: `blockkit` is
// the vision demo's content, and an engine profile that knew about it would be an engine that
// cannot be used for a second game. A game calls this and then registers its own on top:
//
//     worldkit::register_engine_components(world);
//     blockkit::register_blockkit_components(world);   // the game's own
//
// ORDER IS FIXED AND SHARED, which matters beyond tidiness: `ecs::component_schema_hash` goes into
// `net::NetDriver::Config`, and two peers that registered different sets refuse to connect. Both
// peers calling this cannot disagree.
//
// Idempotent — `World::register_component` on an already-registered type is a no-op returning the
// existing id — so calling it twice, or after a module already registered something, is safe.
//
// Returns the number of component types the world holds afterwards, so a caller can log or assert
// against it rather than trusting that the call did anything.
std::size_t register_engine_components(ecs::World& world);

} // namespace rime::worldkit
