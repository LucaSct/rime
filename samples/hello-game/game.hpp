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
#include <string_view>
#include <vector>

#include "rime/app/game_definition.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/reflect.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"

namespace hello_game {

// The game's action layout — ITS names for the ActionState slots. The engine knows only "axis 0";
// what axis 0 means is the game's business, declared once here and used by both the key bindings
// (`definition()`) and the rules (`fixed_tick`), so the two cannot drift apart.
inline constexpr std::uint8_t kAxisMoveX = 0; // −1 left … +1 right
inline constexpr std::uint8_t kAxisMoveZ = 1; // −1 forward (−z, the camera's look) … +1 back
inline constexpr std::uint8_t kButtonQuit = 0;
inline constexpr std::uint8_t kButtonThrow = 1;

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

// One authoritative, reflected record keeps every byte the digest reads on the replicated side.
// The physics bridge still owns its bodies; this records its crate pose after each fixed step.
struct ReplicatedGameState {
    std::uint64_t ticks = 0;
    std::uint32_t score = 0;
    std::uint64_t collected0 = 0, collected1 = 0, collected2 = 0, collected3 = 0, collected4 = 0;
    rime::core::Vec3 player_position{};
    rime::core::Vec3 player_velocity{};
    rime::core::Vec3 crate_position{};
    rime::core::Quat crate_rotation{};
    // Resource state belongs to the owner checkpoint's group. The score is authoritative only:
    // prediction never runs the target test or grants a point.
    std::uint32_t throw_charges = 3;
    std::uint32_t throw_cooldown = 0;
    std::uint32_t target_score = 0;
    rime::core::Vec3 player_facing{0.0f, 0.0f, -1.0f};
};

// One entity per accepted flight. WorldTransform lets the normal world stream give it a NetId;
// the complete motion state is also captured in the owner checkpoint for prediction.
struct ThrownProjectile {
    rime::core::Vec3 position{};
    rime::core::Vec3 velocity{};
    std::uint32_t age = 0;
};

// The crate's full rigid-body state, copied out of the physics world after every authoritative
// step (m20.6b, ADR-0106 §2). The pose alone is already in ReplicatedGameState; this adds what a
// physics body cannot be restored without -- linear and angular velocity -- plus whether the
// player touched it this tick, the fact the server's membership policy needs and only the physics
// contact stream knows. It sits on the crate entity because it is per-object state: the server's
// owner checkpoint captures it from here when the crate is in the interaction group. It is not in
// the digest, which is unchanged (the crate pose it already hashes is the same body's).
struct CrateBody {
    rime::core::Vec3 position{};
    rime::core::Quat orientation{};
    rime::core::Vec3 linear_velocity{};
    rime::core::Vec3 angular_velocity{};
    std::uint32_t touching_player = 0;
};

// The crate's roster state bytes: position, orientation, linear and angular velocity as 13
// little-endian float bit patterns (52 bytes).
inline constexpr std::size_t kCrateStateBytes = 52;

[[nodiscard]] std::uint64_t replicated_digest(const rime::ecs::World& world);
[[nodiscard]] rime::ecs::Entity owned_player(rime::ecs::World& world);
[[nodiscard]] rime::replication::PredictionHooks prediction_hooks(rime::ecs::World& world,
                                                                  rime::ecs::Entity player);

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
    [[nodiscard]] bool bind_remote_world(rime::ecs::World& world) override;
    [[nodiscard]] std::uint64_t state_digest(const rime::ecs::World& world) const override;
    void present_setup(rime::app::PresentSetupContext& ctx) override;
    void present_frame(rime::app::PresentFrameContext& ctx) override;
    void present_prediction(rime::app::PresentFrameContext& ctx,
                            rime::ecs::World& prediction_world) override;

    // ── What a host may ask about the game ────────────────────────────────────────────────────
    [[nodiscard]] int score() const noexcept;

    [[nodiscard]] bool won() const noexcept { return score() == kPickupCount; }

    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    [[nodiscard]] std::uint64_t ticks() const noexcept;

    [[nodiscard]] rime::core::Vec3 player_position() const noexcept;

    // The tick the Nth marker was collected on, or 0 for one still out there. The self-check
    // compares these between two runs: a scalar score would also match if the pickups were
    // collected in a different order on a different thread count, and that is exactly the failure
    // a determinism proof is for.
    [[nodiscard]] std::uint64_t collected_on(std::size_t index) const;

    // How many times the engine called a presentation hook. The dedicated proof asserts ZERO: a
    // server that set up meshes nobody will see is a server that needed a device to start.
    [[nodiscard]] std::uint64_t presentation_calls() const noexcept { return presentation_calls_; }

    // A test moves the target off the known flight path as a sensitivity control for the sweep.
    void set_throw_target(rime::core::Vec3 target) noexcept { throw_target_ = target; }

private:
    struct Pickup {
        rime::ecs::Entity entity{};
        rime::physics::BodyId body{};
    };

    void apply_intent(const Intent& intent);
    void move_player(rime::ecs::World& world, float dt);
    [[nodiscard]] ReplicatedGameState* state() const noexcept;
    void bind_bodies(rime::ecs::World& world);
    void collect(rime::ecs::World& world);
    void throw_step(rime::app::TickContext& ctx, ReplicatedGameState& state);
    void record_crate(rime::ecs::World& world);
    void present_projectile(rime::ecs::World& render_world, rime::ecs::World& source_world);

    // The physics world is the GAME's (it was the host's before m20.1): the engine owns the loop
    // and hands the game the tick, and what the game simulates with is its own affair.
    rime::physics::PhysicsWorld physics_;
    rime::physics::PhysicsSync sync_;

    Intent intent_{};
    rime::ecs::Entity player_{};
    rime::ecs::Entity crate_{};
    rime::ecs::Entity camera_{}; // presentation only: spawned by present_setup, absent otherwise
    rime::ecs::Entity projectile_visual_{}; // one visible identity across guess and adoption
    rime::ecs::Entity state_entity_{};
    rime::ecs::Entity projectile_{};
    rime::replication::SpawnKey projectile_key_{};
    rime::core::Vec3 throw_target_{0.0f, 0.9f, -2.55f};
    rime::physics::BodyId player_body_{};
    rime::physics::BodyId crate_body_{};
    rime::ecs::World* world_ = nullptr;
    std::vector<Pickup> pickups_;
    bool bound_ = false;
    bool quit_ = false;
    std::uint64_t presentation_calls_ = 0;
};

// The entry scene, relative to the content root (m20.2): the arena floor.
inline constexpr std::string_view kEntryScene = "hello-game.rscene";

// The arena — the content `content/hello-game.rscene` holds. `hello_game --emit-content <dir>`
// writes it from this function; the committed file is that output, and the editor is how it would
// change from here (the target-range shape: a generator writes the first draft).
void build_arena(rime::ecs::World& world);

// The component types the entry scene names, registered before it loads (strict load).
void register_components(rime::ecs::World& world);

// The GameDefinition: the only thing `main` hands the engine.
[[nodiscard]] rime::app::GameDefinition definition();

} // namespace hello_game

// Beside the declaration because World::spawn_with registers components in whichever TU sees it.
RIME_REFLECT_BEGIN(hello_game::ReplicatedGameState)
RIME_REFLECT_FIELD(ticks)
RIME_REFLECT_FIELD(score)
RIME_REFLECT_FIELD(collected0)
RIME_REFLECT_FIELD(collected1)
RIME_REFLECT_FIELD(collected2)
RIME_REFLECT_FIELD(collected3)
RIME_REFLECT_FIELD(collected4)
RIME_REFLECT_FIELD(player_position)
RIME_REFLECT_FIELD(player_velocity)
RIME_REFLECT_FIELD(crate_position)
RIME_REFLECT_FIELD(crate_rotation)
RIME_REFLECT_FIELD(throw_charges)
RIME_REFLECT_FIELD(throw_cooldown)
RIME_REFLECT_FIELD(target_score)
RIME_REFLECT_FIELD(player_facing)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(hello_game::CrateBody)
RIME_REFLECT_FIELD(position)
RIME_REFLECT_FIELD(orientation)
RIME_REFLECT_FIELD(linear_velocity)
RIME_REFLECT_FIELD(angular_velocity)
RIME_REFLECT_FIELD(touching_player)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(hello_game::ThrownProjectile)
RIME_REFLECT_FIELD(position)
RIME_REFLECT_FIELD(velocity)
RIME_REFLECT_FIELD(age)
RIME_REFLECT_END()
