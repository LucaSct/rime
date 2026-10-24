// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rime/app/editor_play.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/first_person.hpp"

namespace block_editor {

// A local, query-only walking preview. No network actor, authored collider or reflected player
// entity is created. The host owns the borrowed worlds and must deliver Stop before destroying
// them; the adapter owns only ids for its disposable static city collision snapshot.
class PreviewPlayer {
public:
    static constexpr std::size_t max_bodies = 2048;
    static constexpr std::size_t max_hulls = 128;
    static constexpr std::size_t max_patterns = 32;

    PreviewPlayer() = default;
    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;

    [[nodiscard]] rime::app::EditorPlayHooks hooks();
    [[nodiscard]] rime::app::EditorPlayAdmission begin(rime::app::EditorPlayContext& context);
    void tick(rime::app::EditorPlayContext& context,
              std::span<const rime::platform::Event> events,
              double dt);
    void transition(rime::app::EditorPlayContext& context,
                    rime::app::EditorPlayTransition transition);

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] const rime::gameplay::CharacterState& state() const noexcept { return state_; }

    [[nodiscard]] const rime::gameplay::FirstPersonView& view() const noexcept { return view_; }

    [[nodiscard]] const rime::gameplay::StepStats& step_stats() const noexcept { return stats_; }

    [[nodiscard]] std::size_t body_count() const noexcept { return bodies_.size(); }

    [[nodiscard]] std::size_t hull_count() const noexcept { return hulls_.size(); }

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

private:
    struct Part {
        rime::physics::HullId hull;
        rime::core::Vec3 offset;
    };

    struct Pattern {
        std::uint64_t asset = 0;
        rime::core::Vec3 scale;
        std::vector<Part> parts;
    };

    [[nodiscard]] bool bind_collision(rime::app::EditorPlayContext& context);
    [[nodiscard]] bool add_body(rime::physics::PhysicsWorld& physics,
                                const rime::physics::BodyDesc& description);
    [[nodiscard]] bool bind_pattern(rime::physics::PhysicsWorld& physics,
                                    std::uint64_t asset,
                                    const rime::core::Transform& pose);
    void cleanup(rime::physics::PhysicsWorld& physics);
    void release_input() noexcept;
    [[nodiscard]] bool write_camera(rime::ecs::World& world);

    rime::ecs::Entity camera_ = rime::ecs::kNullEntity;
    rime::gameplay::CharacterConfig config_{};
    rime::gameplay::CharacterState state_{};
    rime::gameplay::FirstPersonView view_{};
    rime::gameplay::StepStats stats_{};
    std::vector<rime::physics::BodyId> bodies_;
    std::vector<rime::physics::HullId> hulls_;
    std::vector<Pattern> patterns_;
    std::uint64_t session_ = 0;
    bool active_ = false;
    bool paused_ = false;
    bool forward_ = false, backward_ = false, left_ = false, right_ = false, jump_ = false;
    std::string failure_;
};

} // namespace block_editor
