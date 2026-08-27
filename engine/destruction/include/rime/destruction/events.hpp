// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/destruction/ids.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/body.hpp"

// The destruction event stream (M8.4, ADR-0029 §7): what a destructible reports each update() as
// plain data, published through a core::EventChannel and read AFTER the tick — never callbacks
// fired mid-solve. One stream fans out to every interested system without destruction knowing any
// of them: the VFX dust stub, the engine/audio null seam, and gameplay each read the same span
// (guardrail 2 — remove any one and the others are byte-identical, because they share an immutable
// view). Emitted in a canonical order (settle events, then damage per part in ascending id, then
// detachments per island in creation order), so the stream is a pure function of the tick's inputs
// exactly like the fracture it narrates — the M11 replay contract, extended to the fan-out.
//
// Every payload carries a world-space AABB (the M10-C2 hook): a lighting/culling consumer keys off
// where a break happened without re-deriving it from part ids and placements it cannot see.
namespace rime::destruction {

enum class DestructionEventKind : std::uint8_t {
    // A part took damage this tick and still stands. `magnitude` is the health removed.
    PartDamaged = 0,
    // A part's health reached zero this tick; it leaves the wall as its own debris chunk carrying
    // the killing impulse (ADR-0029 §2). `magnitude` is the health removed on the fatal tick.
    PartDied = 1,
    // A group of still-standing parts lost their support and broke free as ONE new debris body (a
    // hull for a lone part, a compound for several). `body` is that body; `magnitude` is the
    // world-space impulse magnitude it was kicked off with. NOT emitted for a killed part's own
    // chunk — that death is already a PartDied.
    IslandDetached = 2,
    // A debris body came to rest — a physics `Slept` event (M7.9) on a body fracture created. The
    // basis for m8.5's lifecycle (settle ⇒ linger ⇒ freeze). `body` is the settled debris.
    DebrisSettled = 3,
    // A frozen debris crossed the VISUAL budget and was retired: its render leaf, its SDF stamp and
    // its shadow-caster entry should all go (m13.2b — ADR-0032 C6, ruled to M12/M13 by ADR-0035
    // §6).
    //
    // THE ONE THIS CHANNEL WAS EXTENDED FOR, and the reason is a leak rather than a bug. m8.5's
    // lifecycle bounds debris in the PHYSICS world — settle ⇒ linger ⇒ freeze destroys the body —
    // but it deliberately keeps the roster record so ids never shift, and ADR-0029 §8 says in as
    // many words that "a render leaf can outlive the physics body at its last pose". Correct for
    // v1 rendering; it also means debris never despawns VISUALLY, so a long session accumulates
    // render leaves, SDF bricks and shadow casters for rubble nobody can see. Nothing ever told
    // those caches a piece was gone, because for rendering nothing was.
    //
    // `world_bounds` is the leaf's last known extent, captured when the body was destroyed at
    // FREEZE time — the last moment anyone could ask the physics world where it was. That is what
    // makes this event usable as a cache invalidation exactly like `PartDied` and `IslandDetached`:
    // same payload shape, same channel, no new plumbing. `body` is null (it was already destroyed).
    DebrisEvicted = 4,
};

// One destruction event. Which fields are meaningful depends on `kind` (documented above): part
// events name a `part`, body events name a `body`, and `world_bounds` is always the world-space
// AABB of whatever the event concerns (the struck part, the detached island, the settled body).
struct DestructionEvent {
    DestructionEventKind kind = DestructionEventKind::PartDamaged;
    InstanceId instance{}; // the destructible concerned (the debris's source, too)
    std::uint32_t part =
        kInvalidPartIndex;        // the part id for PartDamaged/PartDied; invalid otherwise
    physics::BodyId body{};       // the debris body for IslandDetached/DebrisSettled
    physics::Aabb world_bounds{}; // world-space AABB of the affected part/island (M10-C2)
    float magnitude = 0.0f;       // damage removed, or |impulse| for a detachment — a VFX/
                                  // audio intensity; 0 for a settle
};

} // namespace rime::destruction
