// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Authored player spawn (creator brick): a marker component a map author drops on any transformed
// entity to say where the local player starts and which way they face.
//
// The decisions, once, so the sample, the editor host and the tests cannot drift:
//
//   * THE POSE IS A CAPSULE CENTRE, in metres, in world space. Not the eye (the camera adds
//     `eye_height` on top of the character position) and not the feet. This is the same quantity
//     `CharacterState::position` already holds, so the server can put the avatar there with no
//     conversion and the same number serves out-of-bounds recovery.
//   * ORIENTATION gives the initial yaw and pitch of the view; roll is ignored because a first
//     person view has no roll. SCALE only takes part in composing a parent chain; it never sizes
//     the avatar (that is CharacterConfig's job).
//   * AN ABSENT MARKER IS NOT AN ERROR. Every existing and generated map has none and must run
//     exactly as before, so `None` means "use the old west-viewpoint default".
//   * AN ENABLED BUT UNUSABLE MARKER IS AN ERROR, never a fallback. If a creator placed a spawn and
//     it is wrong, quietly starting them at the old default hides the mistake and looks like the
//     feature being broken. Two enabled markers are refused for the same reason: choosing one by
//     archetype order would make the answer depend on storage layout, not on the map.
//
// AUTHORITY: the server resolves from its own world and places the avatar; the client resolves
// from the same file and uses the result only for its initial look (look never goes through
// CharacterState, see first_person.hpp). Both peers load the same scene, so they agree, and nothing
// here travels over the wire.
//
// The result is plain copied data. It holds no entity handle, so it stays valid when the world
// later changes structure (archetype moves, despawns), and it is never written back to the scene.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "rime/blockkit/role.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/first_person.hpp"
#include "rime/worldkit/profile.hpp"

namespace rime::block_play {

// The authored marker. Disabled by default so adding the component in the Inspector is harmless
// until the author ticks it on.
struct PlayerSpawn {
    bool enabled = false;
};

enum class SpawnStatus { None, Valid, Refused };

struct SpawnPlacement {
    core::Vec3 capsule_centre{}; // world metres; CharacterState::position convention
    float yaw = 0.0f;            // FirstPersonView convention: about +Y, 0 faces -Z
    float pitch = 0.0f;          // + looks up, within +-gameplay::kMaxPitch
};

struct SpawnResolution {
    SpawnStatus status = SpawnStatus::None;
    SpawnPlacement placement{}; // meaningful only when status == Valid
    std::size_t enabled_count = 0;
    std::string reason; // non-empty only when status == Refused
};

namespace detail {

[[nodiscard]] inline bool finite3(core::Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] inline bool usable(const core::Transform& t) noexcept {
    const core::Quat& q = t.rotation;
    // TRS composition assumes unit rotations. Normalising only after composing would still
    // rotate child translations with the malformed parent, and huge finite quaternions can
    // overflow float length() into a zero/identity fallback. Refuse those descriptions before
    // any arithmetic, with a small tolerance for serialized unit-quaternion roundoff.
    const double norm_squared =
        double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
    return finite3(t.translation) && finite3(t.scale) && std::isfinite(q.x) && std::isfinite(q.y) &&
           std::isfinite(q.z) && std::isfinite(q.w) && std::abs(norm_squared - 1.0) <= 0.002;
}

// Bound malformed/cyclic traversal; valid hierarchies deeper than this are explicitly refused.
inline constexpr int kMaxParentDepth = 64;

[[nodiscard]] inline SpawnResolution refuse(std::size_t enabled, std::string why) {
    SpawnResolution r;
    r.status = SpawnStatus::Refused;
    r.enabled_count = enabled;
    r.reason = std::move(why);
    return r;
}

} // namespace detail

// Find and validate the map's player spawn. Pure: it reads `world` and returns a value. It does
// NOT rely on WorldTransform (derived state that may not exist yet): it composes the authored
// LocalTransform chain itself, with the same `world = parent * local` rule as
// `ecs::propagate_transforms`, so a parented marker resolves identically whether or not a derive
// pass has run. (`blockkit::derive_world_transforms` is flat-only and ignores Parent.)
[[nodiscard]] inline SpawnResolution resolve_player_spawn(ecs::World& world) {
    ecs::Entity marker = ecs::kNullEntity;
    std::size_t enabled = 0;
    world.query<PlayerSpawn>().for_each([&](ecs::Entity e, const PlayerSpawn& s) {
        if (s.enabled) {
            ++enabled;
            marker = e;
        }
    });
    if (enabled == 0) {
        return {};
    }
    if (enabled > 1) {
        return detail::refuse(enabled,
                              std::to_string(enabled) +
                                  " enabled PlayerSpawn markers; exactly one is allowed");
    }

    const auto* local = world.get<ecs::LocalTransform>(marker);
    if (local == nullptr) {
        return detail::refuse(enabled, "the enabled PlayerSpawn has no LocalTransform");
    }
    if (!detail::usable(local->value)) {
        return detail::refuse(
            enabled,
            "the enabled PlayerSpawn pose is non-finite or its rotation is not unit length");
    }

    core::Transform composed = local->value;
    ecs::Entity cursor = marker;
    for (int depth = 0;; ++depth) {
        const auto* parent = world.get<ecs::Parent>(cursor);
        if (parent == nullptr || parent->value == ecs::kNullEntity) {
            break;
        }
        if (depth >= detail::kMaxParentDepth) {
            return detail::refuse(enabled, "the PlayerSpawn parent chain is cyclic or too deep");
        }
        const auto* up =
            world.is_alive(parent->value) ? world.get<ecs::LocalTransform>(parent->value) : nullptr;
        if (up == nullptr) {
            return detail::refuse(enabled,
                                  "a PlayerSpawn ancestor is missing or has no LocalTransform");
        }
        if (!detail::usable(up->value)) {
            return detail::refuse(
                enabled,
                "a PlayerSpawn ancestor pose is non-finite or its rotation is not unit length");
        }
        composed = up->value * composed;
        cursor = parent->value;
    }

    const core::Quat q = core::normalize(composed.rotation);
    if (!detail::usable(composed) || !std::isfinite(q.x) || !std::isfinite(q.y) ||
        !std::isfinite(q.z) || !std::isfinite(q.w)) {
        return detail::refuse(enabled, "the composed PlayerSpawn pose is not finite");
    }

    // Yaw/pitch from the forward axis (-Z rotated by the pose). Roll never enters. Looking within
    // a hair of straight up or down leaves yaw undefined, and guessing one would turn the player
    // somewhere the author did not choose, so that is refused too.
    const core::Vec3 f = core::rotate(q, core::Vec3{0.0f, 0.0f, -1.0f});
    if (std::hypot(f.x, f.z) < 1e-3f) {
        return detail::refuse(enabled,
                              "the PlayerSpawn looks straight up or down; yaw is undefined");
    }

    SpawnResolution r;
    r.status = SpawnStatus::Valid;
    r.enabled_count = 1;
    r.placement.capsule_centre = composed.translation;
    r.placement.yaw = std::atan2(-f.x, -f.z);
    r.placement.pitch = std::clamp(
        std::asin(std::clamp(f.y, -1.0f, 1.0f)), -gameplay::kMaxPitch, gameplay::kMaxPitch);
    return r;
}

// THE component list of this game's worlds: the engine's profile, blockkit's, then the spawn
// marker. The game (`register_all`) and the editor host both call this, never their own copies, so
// the Inspector offers PlayerSpawn exactly where Play consumes it, and the schema hash two peers
// compare cannot differ by who built the list.
inline void register_block_components(ecs::World& world) {
    (void)worldkit::register_engine_components(world);
    blockkit::register_blockkit_components(world);
    (void)world.register_component<PlayerSpawn>();
}

} // namespace rime::block_play

RIME_REFLECT_BEGIN(rime::block_play::PlayerSpawn)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
