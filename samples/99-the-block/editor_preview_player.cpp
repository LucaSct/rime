// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "editor_preview_player.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "play_city.hpp"
#include "player_spawn.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ground/surface.hpp"
#include "rime/physics/components.hpp"
#include "rime/render/components.hpp"

namespace block_editor {
namespace {
using namespace rime;

bool positive(float v) {
    return std::isfinite(v) && v >= 1e-4f && v <= 8192.0f;
}

bool finite(core::Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool valid_pose(const core::Transform& pose) {
    return block_play::detail::usable(pose) && positive(pose.scale.x) &&
           positive(pose.scale.y) && positive(pose.scale.z) &&
           std::abs(pose.translation.x) <= 8192.0f && std::abs(pose.translation.y) <= 8192.0f &&
           std::abs(pose.translation.z) <= 8192.0f;
}

bool parented(ecs::World& world, ecs::Entity entity) {
    const auto* parent = world.get<ecs::Parent>(entity);
    return parent && parent->value != ecs::kNullEntity;
}

core::Vec3 scaled(core::Vec3 a, core::Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

bool same(core::Vec3 a, core::Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool already_bound(ecs::World& world, physics::PhysicsWorld& physics, ecs::Entity entity) {
    if (const auto* body = world.get<physics::RigidBodyHandle>(entity))
        return physics.is_alive(body->body);
    if (const auto* body = world.get<ground::GroundBody>(entity))
        return physics.is_alive(body->body);
    return false;
}

bool cube_kind(std::uint32_t kind) {
    using namespace blockkit::slab_kind;
    return kind == kKerb || kind == kBarrier || kind == kLampMast || kind == kLampHead ||
           kind == kTimber;
}
} // namespace

rime::app::EditorPlayHooks PreviewPlayer::hooks() {
    return {[this](rime::app::EditorPlayContext& c) { return begin(c); },
            [this](rime::app::EditorPlayContext& c,
                   std::span<const rime::platform::Event> events,
                   double dt) { tick(c, events, dt); },
            [this](rime::app::EditorPlayContext& c, rime::app::EditorPlayTransition t) {
                transition(c, t);
            }};
}

bool PreviewPlayer::add_body(rime::physics::PhysicsWorld& physics,
                             const rime::physics::BodyDesc& description) {
    if (bodies_.size() >= max_bodies) {
        failure_ = "Preview collision exceeds the 2048-body limit";
        return false;
    }
    const auto body = physics.create_body(description);
    if (!body.is_valid()) {
        failure_ = "Physics refused a preview collision body";
        return false;
    }
    bodies_.push_back(body);
    return true;
}

bool PreviewPlayer::bind_pattern(rime::physics::PhysicsWorld& physics,
                                 std::uint64_t asset,
                                 const rime::core::Transform& pose) {
    using namespace rime;
    auto found = std::find_if(patterns_.begin(), patterns_.end(), [&](const Pattern& p) {
        return p.asset == asset && same(p.scale, pose.scale);
    });
    if (found == patterns_.end()) {
        if (patterns_.size() >= max_patterns) {
            failure_ = "Preview collision exceeds the 32 scaled-pattern limit";
            return false;
        }
        // Register the very convex parts drawn by intact_city_mesh. Scale the source vertices
        // before registration, then compensate for the hull registrar's COM recentering. A
        // bounding box would fill the gaps beneath a lamp arm and beside a barrier's feet.
        const auto source = asset == block_play::city::kLampAsset
                                ? block_play::city::make_lamp_asset()
                                : block_play::city::make_barrier_asset();
        Pattern pattern{asset, pose.scale, {}};
        for (const auto& part : source.parts) {
            if (hulls_.size() >= max_hulls) {
                failure_ = "Preview collision exceeds the 128-hull limit";
                return false;
            }
            std::vector<core::Vec3> vertices;
            vertices.reserve(part.vertices.size());
            for (const auto& vertex : part.vertices)
                vertices.push_back(scaled(vertex, pose.scale));
            const auto hull = physics.register_hull({vertices, part.face_counts, part.face_indices});
            if (!hull.is_valid()) {
                failure_ = "Physics refused a scaled city source hull";
                return false;
            }
            hulls_.push_back(hull);
            physics::HullInfo info;
            if (!physics.hull_info(hull, info)) {
                failure_ = "Preview source hull could not be inspected";
                return false;
            }
            pattern.parts.push_back({hull, scaled(part.com, pose.scale) + info.centroid});
        }
        patterns_.push_back(std::move(pattern));
        found = patterns_.end() - 1;
    }
    for (const auto& part : found->parts) {
        physics::BodyDesc description;
        description.motion = physics::MotionType::Static;
        description.shape.type = physics::ShapeType::ConvexHull;
        description.shape.hull = part.hull;
        description.orientation = core::normalize(pose.rotation);
        description.position = pose.translation + core::rotate(description.orientation, part.offset);
        if (!finite(description.position) || !add_body(physics, description))
            return false;
    }
    return true;
}

bool PreviewPlayer::bind_collision(rime::app::EditorPlayContext& context) {
    using namespace rime;
    auto& world = context.world;
    bool ok = true;
    world.query<physics::Collider>().for_each([&](ecs::Entity, const physics::Collider& collider) {
        if (collider.shape_type == static_cast<std::uint32_t>(physics::ShapeType::Heightfield)) {
            ok = false;
            failure_ = "Walking Preview does not support heightfield shape queries";
        }
    });
    if (!ok)
        return false;
    world.query<blockkit::SlabRole>().for_each([&](ecs::Entity entity, const blockkit::SlabRole& role) {
        if (!ok || role.kind == blockkit::slab_kind::kLight)
            return;
        if (already_bound(world, context.physics, entity))
            return;
        const auto* local = world.get<ecs::LocalTransform>(entity);
        const auto* derived = world.get<ecs::WorldTransform>(entity);
        if (!local || !derived || parented(world, entity) || !valid_pose(local->value) ||
            !valid_pose(derived->value)) {
            ok = false;
            failure_ = "Preview geometry needs a finite root transform with positive scale";
            return;
        }
        const auto& pose = derived->value;
        physics::BodyDesc description;
        description.motion = physics::MotionType::Static;
        description.position = pose.translation;
        description.orientation = core::normalize(pose.rotation);
        description.shape.type = physics::ShapeType::Box;
        if (const auto* destructible = world.get<destruction::Destructible>(entity)) {
            if (const auto* cook = blockkit::find_cook_by_asset(destructible->asset)) {
                description.shape.half_extents =
                    scaled({cook->size_x * 0.5f, cook->size_y * 0.5f, cook->size_z * 0.5f},
                           pose.scale);
            } else if (destructible->asset == block_play::city::kLampAsset ||
                       destructible->asset == block_play::city::kBarrierAsset) {
                ok = bind_pattern(context.physics, destructible->asset, pose);
                return;
            } else {
                ok = false;
                failure_ = "A destructible asset has no known preview source collision";
                return;
            }
        } else if (cube_kind(role.kind)) {
            description.shape.half_extents = scaled({0.5f, 0.5f, 0.5f}, pose.scale);
        } else {
            ok = false;
            failure_ = "A slab or street is missing its authored collision source";
            return;
        }
        const auto half = description.shape.half_extents;
        if (!positive(half.x) || !positive(half.y) || !positive(half.z)) {
            ok = false;
            failure_ = "Preview source dimensions are outside supported positive bounds";
            return;
        }
        ok = add_body(context.physics, description);
    });
    if (!ok)
        return false;
    // Refuse an unrecognised rendered object rather than call a scene collision-ready while a
    // newly imported wall is walk-through. Explicit light/decorative roles carry no solid volume.
    auto inspect_mesh = [&](ecs::Entity entity) {
        if (!already_bound(world, context.physics, entity) &&
            !world.get<blockkit::SlabRole>(entity)) {
            ok = false;
            failure_ = "Rendered geometry has no supported preview collision source";
        }
    };
    world.query<render::MeshRef>().for_each([&](ecs::Entity e, const render::MeshRef&) { inspect_mesh(e); });
    world.query<render::MeshAsset>().for_each([&](ecs::Entity e, const render::MeshAsset&) { inspect_mesh(e); });
    return ok;
}

void PreviewPlayer::cleanup(rime::physics::PhysicsWorld& physics) {
    // Bodies reference hulls, so destroy every body first; unregistering a live hull is refused.
    for (auto body : bodies_)
        physics.destroy_body(body);
    for (auto hull : hulls_)
        (void)physics.unregister_hull(hull);
    bodies_.clear();
    hulls_.clear();
    patterns_.clear();
    active_ = false;
    paused_ = false;
    camera_ = rime::ecs::kNullEntity;
    session_ = 0;
    release_input();
}

rime::app::EditorPlayAdmission PreviewPlayer::begin(rime::app::EditorPlayContext& context) {
    using namespace rime;
    if (active_)
        return {false, "A Preview player session is already active"};
    failure_.clear();
    state_ = {};
    view_ = {};
    stats_ = {};
    release_input();
    const auto spawn = block_play::resolve_player_spawn(context.world);
    if (spawn.status != block_play::SpawnStatus::Valid)
        return {false, spawn.reason.empty() ? "Player Preview needs one enabled PlayerSpawn" : spawn.reason};
    if (!finite(spawn.placement.capsule_centre) ||
        std::abs(spawn.placement.capsule_centre.x) > 8192.0f ||
        std::abs(spawn.placement.capsule_centre.y) > 8192.0f ||
        std::abs(spawn.placement.capsule_centre.z) > 8192.0f)
        return {false, "PlayerSpawn is outside Preview's 8192-metre bounds"};
    std::size_t cameras = 0;
    context.world.query<render::Camera>().for_each([&](ecs::Entity e, const render::Camera& camera) {
        if (camera.active) { ++cameras; camera_ = e; }
    });
    if (cameras != 1)
        return {false, "Player Preview needs exactly one active camera"};
    const auto* local = context.world.get<ecs::LocalTransform>(camera_);
    if (!local || !context.world.get<ecs::WorldTransform>(camera_) ||
        parented(context.world, camera_) || !valid_pose(local->value)) {
        camera_ = ecs::kNullEntity;
        return {false, "Player Preview camera needs a finite unparented Local/WorldTransform"};
    }
    if (!bind_collision(context)) {
        const auto reason = failure_;
        cleanup(context.physics);
        return {false, reason};
    }
    state_.position = spawn.placement.capsule_centre;
    physics::PenetrationHit overlap;
    if (context.physics.penetration(gameplay::character_shape(config_), state_.position,
                                    core::quat_identity(), overlap) && overlap.depth > 0.001f) {
        cleanup(context.physics);
        return {false, "PlayerSpawn overlaps solid preview collision; move the capsule centre clear"};
    }
    view_.yaw = spawn.placement.yaw;
    view_.pitch = spawn.placement.pitch;
    session_ = context.session_id;
    active_ = true;
    paused_ = false;
    (void)write_camera(context.world);
    RIME_INFO("player-preview: admitted session {} at ({}, {}, {}), {} static bodies, {} source hulls; query-only, no weapons",
              session_, state_.position.x, state_.position.y, state_.position.z, bodies_.size(), hulls_.size());
    return {};
}

void PreviewPlayer::release_input() noexcept {
    forward_ = backward_ = left_ = right_ = jump_ = false;
    // A released capture has no residual horizontal command. Gravity/jump motion remains real,
    // but acceleration's old lateral momentum must not coast through a later Resume.
    state_.velocity.x = state_.velocity.z = 0.0f;
}

bool PreviewPlayer::write_camera(rime::ecs::World& world) {
    const auto* camera = world.get<rime::render::Camera>(camera_);
    auto* local = world.get<rime::ecs::LocalTransform>(camera_);
    auto* derived = world.get<rime::ecs::WorldTransform>(camera_);
    if (!camera || !camera->active || !local || !derived || parented(world, camera_)) {
        failure_ = "The possessed Preview camera was removed, disabled or parented";
        release_input();
        return false;
    }
    const auto eye = rime::gameplay::eye_transform(view_, state_.position);
    local->value.translation = derived->value.translation = eye.translation;
    local->value.rotation = derived->value.rotation = eye.rotation;
    return true;
}

void PreviewPlayer::tick(rime::app::EditorPlayContext& context,
                          std::span<const rime::platform::Event> events,
                          double dt) {
    using namespace rime;
    if (!active_ || paused_ || context.session_id != session_ || !failure_.empty() ||
        !std::isfinite(dt) || dt <= 0.0 || dt > 0.1)
        return;
    std::uint32_t pressed = 0;
    for (const auto& event : events) {
        if ((event.type == platform::EventType::WindowFocus && !event.focus.focused) ||
            event.type == platform::EventType::WindowClose) {
            release_input();
            pressed = 0;
            break;
        }
        if (event.type == platform::EventType::MouseMove) {
            gameplay::apply_look(view_, event.mouse_move.dx, event.mouse_move.dy);
        } else if (event.type == platform::EventType::KeyDown || event.type == platform::EventType::KeyUp) {
            const bool down = event.type == platform::EventType::KeyDown;
            switch (event.key.key) {
                case platform::Key::W: forward_ = down; break;
                case platform::Key::S: backward_ = down; break;
                case platform::Key::A: left_ = down; break;
                case platform::Key::D: right_ = down; break;
                case platform::Key::Space:
                    if (down && !jump_ && !event.key.repeat)
                        pressed |= gameplay::kActionJump;
                    jump_ = down;
                    break;
                default: break;
            }
        }
    }
    gameplay::CharacterInput intent;
    intent.move_x = float(right_) - float(left_);
    intent.move_y = float(forward_) - float(backward_);
    intent.yaw = view_.yaw;
    intent.pitch = view_.pitch;
    intent.held = jump_ ? gameplay::kActionJump : 0;
    intent.pressed = pressed;
    state_ = gameplay::step_character(state_, intent, config_, context.physics, {}, float(dt), &stats_);
    if (!finite(state_.position) || !finite(state_.velocity) || context.physics.heightfield_query_skips() != 0) {
        failure_ = "Player Preview stopped on non-finite motion or unsupported heightfield queries";
        release_input();
        RIME_WARN("player-preview: {}", failure_);
        return;
    }
    (void)write_camera(context.world);
}

void PreviewPlayer::transition(rime::app::EditorPlayContext& context,
                                rime::app::EditorPlayTransition transition) {
    using rime::app::EditorPlayTransition;
    if (!active_ || context.session_id != session_)
        return;
    switch (transition) {
        case EditorPlayTransition::Pause: release_input(); paused_ = true; break;
        case EditorPlayTransition::Resume: release_input(); paused_ = false; break;
        case EditorPlayTransition::ReleaseInput: release_input(); break;
        case EditorPlayTransition::Stop: cleanup(context.physics); state_ = {}; view_ = {}; stats_ = {}; break;
    }
}

} // namespace block_editor
