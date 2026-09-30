// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// hello-game — the GAME half: everything the game IS, and nothing about running it.
//
// Until m20.1 this class lived at the top of `main.cpp` with its host code below it, and the line
// between the two was a promise in a comment. It is now a `rime::app::Game`, handed to the engine
// through a `rime::app::GameDefinition`, and the line is a type boundary: this header includes no
// window, no loop and no launch mode, and `main.cpp` includes no rule. ADR-0046's riskiest
// assumption was that a game's play loop could be lifted into the engine WITHOUT REWRITING THE
// GAME. What changed here, measured honestly:
//
//   * the three methods became the Game interface's — `setup(SetupContext&)`,
//     `fixed_tick(TickContext&)` — and `apply_intent` is now fed from the tick's ActionState;
//   * the physics world moved INTO the game (it was the host's), because the engine deliberately
//     does not own physics (guardrail 2: rime::app stays physics-agnostic);
//   * the render-only work moved out of `setup` into `present_setup`, so the SIMULATED world is the
//     same entity-for-entity whether or not anything draws it — which is what lets `dedicated` and
//     `play` produce one digest;
//   * the rules — movement, the arena clamp, the trigger collection — did not change at all.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/app/game_definition.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"

namespace hello_game {

// The game's action layout — ITS names for the ActionState slots. The engine knows only "axis 0";
// what axis 0 means is the game's business, declared once here and used by both the key bindings
// (`definition()`) and the rules (`fixed_tick`), so the two cannot drift apart.
inline constexpr std::uint8_t kAxisMoveX = 0; // −1 left … +1 right
inline constexpr std::uint8_t kAxisMoveZ = 1; // −1 forward (−z, the camera's look) … +1 back
inline constexpr std::uint8_t kButtonQuit = 0;

// ── What the game is told, per tick ───────────────────────────────────────────────────────────
//
// A direction on the floor plane and a wish to stop — the game's own reading of an ActionState.
// Kept as a separate type (rather than reading axes inside the rules) because it is the vocabulary
// the rules are written in: `move` and `quit` mean something to this game; `axes[1]` does not.
//
// `move` is not normalised here. Normalising is a game rule (it is what stops diagonal movement
// being 1.41x faster), so the game does it, and a host cannot get it wrong on the game's behalf.
struct Intent {
    rime::core::Vec3 move{0.0f, 0.0f, 0.0f};
    bool quit = false;
};

class HelloGame final : public rime::app::Game {
public:
    // Tuning, all in one place so a reader can change the feel without reading the logic.
    static constexpr float kPlayerSpeed = 4.0f;      // m/s on the floor
    static constexpr float kPlayerRadius = 0.35f;    // capsule radius
    static constexpr float kPlayerHalfHeight = 0.5f; // capsule half-height (excluding caps)
    static constexpr float kPickupRadius = 0.6f;     // the trigger sphere the player must touch
    static constexpr int kPickupCount = 5;
    static constexpr float kRingRadius = 5.0f; // the markers sit on this circle about the origin

    // Where marker `index` is. Public because the autopilot steers at it, and steering at a
    // hard-coded coordinate instead would mean moving a marker silently turns the self-check into
    // one that walks past it — a test that no longer tests what its name says.
    [[nodiscard]] static rime::core::Vec3 marker_position(int index);

    // ── rime::app::Game ───────────────────────────────────────────────────────────────────────
    [[nodiscard]] bool setup(rime::app::SetupContext& ctx) override;
    void fixed_tick(rime::app::TickContext& ctx) override;

    [[nodiscard]] bool finished() const override { return won() || quit_; }

    [[nodiscard]] bool has_autopilot() const override { return true; }

    [[nodiscard]] rime::app::ActionState autopilot() const override;
    [[nodiscard]] std::uint64_t state_digest(const rime::ecs::World& world) const override;
    void present_setup(rime::app::PresentSetupContext& ctx) override;
    void present_frame(rime::app::PresentFrameContext& ctx) override;

    // ── What a host may ask about the game ────────────────────────────────────────────────────
    [[nodiscard]] int score() const noexcept { return score_; }

    [[nodiscard]] bool won() const noexcept { return score_ == kPickupCount; }

    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

    [[nodiscard]] rime::core::Vec3 player_position() const noexcept { return player_pos_; }

    // The tick the Nth marker was collected on, or 0 for one still out there. The self-check
    // compares these between two runs: a scalar score would also match if the pickups were
    // collected in a different order on a different thread count, and that is exactly the failure
    // a determinism proof is for.
    [[nodiscard]] std::uint64_t collected_on(std::size_t index) const {
        return pickups_[index].collected_tick;
    }

    // How many times the engine called a presentation hook. The dedicated proof asserts ZERO: a
    // server that set up meshes nobody will see is a server that needed a device to start.
    [[nodiscard]] std::uint64_t presentation_calls() const noexcept { return presentation_calls_; }

private:
    struct Pickup {
        rime::ecs::Entity entity{};
        rime::physics::BodyId body{};
        bool collected = false;
        std::uint64_t collected_tick = 0;
    };

    void apply_intent(const Intent& intent);
    void move_player(rime::ecs::World& world, float dt);
    void bind_bodies(rime::ecs::World& world);
    void collect(rime::ecs::World& world);

    // The physics world is the GAME's (it was the host's before m20.1): the engine owns the loop
    // and hands the game the tick, and what the game simulates with is its own affair.
    rime::physics::PhysicsWorld physics_;
    rime::physics::PhysicsSync sync_;

    Intent intent_{};
    rime::ecs::Entity player_{};
    rime::ecs::Entity crate_{};
    rime::ecs::Entity camera_{}; // presentation only: spawned by present_setup, absent otherwise
    rime::physics::BodyId player_body_{};
    rime::core::Vec3 player_pos_{};
    std::vector<Pickup> pickups_;
    bool bound_ = false;
    bool quit_ = false;
    int score_ = 0;
    std::uint64_t ticks_ = 0;
    std::uint64_t presentation_calls_ = 0;
};

// The GameDefinition: the only thing `main` hands the engine.
[[nodiscard]] rime::app::GameDefinition definition();

} // namespace hello_game
