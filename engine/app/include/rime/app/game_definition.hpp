// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/replication/input_v2.hpp"
#include "rime/replication/prediction.hpp"
#include "rime/replication/spawn_prediction.hpp"

// `GameDefinition` — what a GAME supplies to the engine, as opposed to what the engine owns (m20.1,
// ADR-0056-m20.1).
//
// Until this brick every sample owned its whole `main`: it parsed its own flags, built its own
// `Application`, decided for itself whether to create a device, and wired input, simulation and
// rendering together by hand. That works for a sample and cannot work for a SHIPPED game, because
// ADR-0046 §1 wants the end user of an exported game to choose at launch between playing locally,
// hosting, streaming and running a dedicated server — and the one thing all of those must agree on
// is the simulation. If each mode is a different hand-written loop, "the dedicated server computes
// what the player saw" is a hope; if every mode is the engine's loop around the same game object,
// it is a property.
//
// So the line is drawn like this, and it is the whole design:
//
//   THE GAME SUPPLIES                          THE ENGINE OWNS
//   ───────────────────────────────────────    ───────────────────────────────────────────────
//   its component set + an optional scene      the process: argument parsing and the mode
//   `setup`: build the simulated world          the loop: the fixed tick and its ordering
//   `fixed_tick`: one step, from an ActionState the device: whether one exists at all
//   an InputMap and remote input profile      input: events/commands → actions, and their tick
//   optional presentation hooks                 the renderer, the window, presentation
//   an optional autopilot and a state digest    the report: ticks run, what was skipped, why
//
// Three rules keep the line honest, each of which a reader can check in `launch.cpp`:
//
//   1. **The simulation never sees a device.** `Game::setup` and `Game::fixed_tick` are handed an
//      `ecs::World`, the job system and an `ActionState` — nothing from `rhi` or `render`. A game
//      that wants to be drawn says so in the separate `present_*` hooks, which the engine calls
//      only in a mode that has a device. That is what lets `dedicated` run the SAME object with no
//      Vulkan in the process.
//   2. **Input is actions, never keys.** The game declares `InputMap` bindings and receives an
//      `ActionState`; the engine maps the keyboard to it in `play`, and an admitted network peer
//      supplies it in `dedicated`. A game that read `platform::Event` directly
//      could not be driven by anything but a keyboard, and could not be tested without faking one.
//   3. **No scripting engine.** Everything here is plain C++ compiled into the game's own target,
//      as VISION and `docs/design/hosted-rime.md` require. A `GameDefinition` is a struct of values
//      and a factory, not a plugin: nothing is loaded at runtime and nothing in `engine/` names a
//      game.
namespace rime::render {
class MeshRegistry;
class MaterialRegistry;
class SceneRenderer;
} // namespace rime::render

namespace rime::app {

// ── Input: actions, never keys ────────────────────────────────────────────────────────────────
//
// A fixed-size snapshot rather than a map of named actions, deliberately: it is copied once per
// tick, it is also the target of the network command mapper, and a game names
// its actions with its own constants — `kMoveX = 0` — so a string lookup would buy nothing but a
// hash per tick. Eight axes and 32 buttons is more than any game in this tree uses; it is a v1
// bound, and widening it is additive.
struct ActionState {
    static constexpr std::size_t kAxes = 8;
    static constexpr std::size_t kButtons = 32;

    // Analogue values. A keyboard produces −1/0/+1 sums; an autopilot or a gamepad produces
    // anything. NOT normalised here: whether diagonal movement should be faster is a game rule, so
    // the game normalises, and a host cannot get it wrong on the game's behalf.
    std::array<float, kAxes> axes{};
    // Bit i set ⇔ button i is held for this tick.
    std::uint32_t held = 0;
    // Bit i set ⇔ button i went DOWN since the previous tick consumed input. Separate from `held`
    // because a tap that goes down and up inside one frame leaves nothing held by the time any tick
    // looks — and a quit key that can be pressed and ignored is a bug report waiting to be filed.
    std::uint32_t pressed = 0;

    [[nodiscard]] float axis(std::size_t i) const noexcept { return i < kAxes ? axes[i] : 0.0f; }

    [[nodiscard]] bool is_held(std::size_t i) const noexcept {
        return i < kButtons && (held & (1u << i)) != 0;
    }

    [[nodiscard]] bool was_pressed(std::size_t i) const noexcept {
        return i < kButtons && (pressed & (1u << i)) != 0;
    }
};

// A held key contributes `value` to axis `axis`. Two keys on one axis with opposite signs is how a
// W/S pair becomes one "forward" axis; the contributions SUM, so holding both is zero rather than
// whichever the event loop happened to see last.
struct AxisBinding {
    platform::Key key = platform::Key::Unknown;
    std::uint8_t axis = 0;
    float value = 1.0f;
};

struct ButtonBinding {
    platform::Key key = platform::Key::Unknown;
    std::uint8_t button = 0;
};

// What the game supplies about input: nothing but the mapping. Out-of-range axis/button indices are
// counted and ignored rather than asserted, because a binding table is data and the counter is how
// a mistake in it becomes visible (`InputMapper::rejected_bindings`).
struct InputMap {
    std::vector<AxisBinding> axes;
    std::vector<ButtonBinding> buttons;
};

// Events → ActionState, owned by the engine.
//
// HELD-KEY STATE, not per-event impulses: "W is down" is a fact about the whole tick, and a KeyDown
// is a fact about one instant. Auto-repeat is ignored for the same reason — a repeat is not a
// second press, and counting it would make a held key accelerate. This is `samples/hello-game`'s
// `KeyboardIntent` lifted into the engine and made table-driven, which is the M20 move in
// miniature.
//
// The two halves run at different rates on purpose: `update` once per FRAME (that is when events
// arrive), `take` once per TICK (that is when the game consumes them). A frame that runs zero ticks
// keeps its presses latched until one does; a frame that runs eight hands the press to the first
// and not to all eight.
class InputMapper {
public:
    explicit InputMapper(InputMap map);

    void update(std::span<const platform::Event> events);

    // The state for one tick. Clears the `pressed` edges — held state persists.
    [[nodiscard]] ActionState take() noexcept;

    // Bindings whose axis/button index was out of range at construction. Zero for a correct table.
    [[nodiscard]] std::size_t rejected_bindings() const noexcept { return rejected_; }

private:
    InputMap map_;
    std::array<bool, static_cast<std::size_t>(platform::Key::Count)> down_{};
    std::uint32_t pressed_ = 0;
    std::size_t rejected_ = 0;
};

// ── What the engine hands the game ────────────────────────────────────────────────────────────
//
// Contexts are structs of references rather than parameter lists so a later brick can add a field
// (a network session, an asset bridge) without touching every game's signature.

struct SetupContext {
    ecs::World& world;
    core::JobSystem& jobs;
    // The constant every tick advances by — the fixed-tick contract, available before the first
    // tick so a game can size anything tick-rate-dependent up front.
    double fixed_dt = 0.0;
    // Where this run's content lives (m20.2) — the root the entry scene was loaded from, for a game
    // that reads more files than its entry scene. Empty when the game declares no content.
    std::filesystem::path content_root{};
};

// COMMAND PROVENANCE (m20.5b, ADR-0105 Consequences): which client command a server tick executed.
// A game that predicts a spawn names it with a SpawnKey built from these numbers -- (incarnation,
// sequence, slot) -- and the client builds the same key from the command it predicted, which is
// what lets the server's verdict find the client's guess (spawn_prediction.hpp, technique 1).
struct CommandProvenance {
    // The admitted session's incarnation (`net::Session::incarnation()`): a reconnect is a new
    // incarnation, so its keys can never collide with the old session's. 0 with no client.
    std::uint32_t incarnation = 0;
    // The InputV2 sequence this tick retired. 0 means "no command" (InputCommand's convention):
    // before a client's schedule has started, nothing retires.
    std::uint32_t sequence = 0;
    // True only when the client's own command ran. False for a SYNTHESIZED stand-in (a late or
    // lost command, held keys repeated with `pressed` cleared) and for a tick with no client: a
    // stand-in is the server's guess at what the player meant, and a guess must never spend a
    // charge or launch anything the client did not ask for -- the client could not predict it.
    bool applied = false;
};

// What a game tells its host about one speculative spawn during a tick (m20.5b). The host --
// today the dedicated server's pump, when the game declares `SpawnPredictionProfile` -- turns each
// report into a durable outcome for the client, gives a launched entity a network identity, and
// retracts that identity when the entity ends. A game reports only keyed spawns (a command with
// provenance); local play has no host listening and no key to report.
struct SpawnReport {
    enum class Kind : std::uint8_t {
        Launched, // `entity` was created for `key` this tick
        Refused,  // the game said no to `key`; `reason` is the game's own code (never 0)
        Ended,    // `entity`, launched for `key`, ended this tick (the game has despawned it)
    };

    Kind kind = Kind::Launched;
    replication::SpawnKey key{};
    ecs::Entity entity{};
    std::uint8_t reason = 0;
};

struct TickContext {
    // A constructor, not aggregate initialisation, for the m20.1 fields: every game and proof
    // brace-initialises `{world, dt, tick, input}`, and with -Wextra a trailing field that brace
    // list omits is a -Wmissing-field-initializers warning (an error under RIME_WARNINGS_AS_ERRORS)
    // at every one of those sites. The fields after `input` are set by name by the hosts that have
    // them.
    TickContext(ecs::World& world_, double dt_, std::uint64_t tick_, ActionState input_ = {})
        : world(world_), dt(dt_), tick(tick_), input(input_) {}

    ecs::World& world;
    // ALWAYS the fixed dt — the same number every tick for the life of the run. A game that
    // integrates against anything else is not deterministic, and the dedicated/play digest proof
    // is what would catch it.
    double dt = 0.0;
    // 0-based index of THIS tick within the run. Ticks the engine skipped (see `RunReport`) do not
    // advance it, so it counts steps the game actually took.
    std::uint64_t tick = 0;
    // The player's, autopilot's, or admitted network client's wish for this tick.
    ActionState input{};
    // Set by a network server for every tick it drives (applied or not); `nullopt` for local play,
    // the autopilot and offline runs. A game that launches only on `applied` commands when this is
    // set, and freely when it is not, behaves the same alone and authoritative.
    std::optional<CommandProvenance> command;
    // Where to report keyed spawns this tick; null when nobody is listening (local play, or a game
    // without `SpawnPredictionProfile`). Non-owning; valid for the duration of `fixed_tick`.
    std::vector<SpawnReport>* spawns = nullptr;
};

// Presentation is a separate world from simulation, and these contexts exist only in a mode that
// has a device. A game that never overrides the `present_*` hooks is still a complete game — it
// simply draws nothing but what its components already describe.
struct PresentSetupContext {
    ecs::World& world;
    render::MeshRegistry& meshes;
    render::MaterialRegistry& materials;
    render::SceneRenderer& renderer;
};

struct PresentFrameContext {
    ecs::World& world;
    // Real seconds this frame covers — for presentation-only motion (a chase camera, a UI tween),
    // never for anything the simulation arbitrates. See `FrameContext::frame_dt`.
    double frame_dt = 0.0;
    // Where the render sits between the last two ticks (the ADR-0023 §3 interpolation seam).
    double alpha = 0.0;
};

// ── The game ──────────────────────────────────────────────────────────────────────────────────
//
// An interface rather than a bag of std::functions because a game has STATE — a score, a player
// entity, a physics world — and a set of lambdas capturing shared state is an object with its
// methods scattered across a file. The engine creates a fresh one per run through
// `GameDefinition::create`, so running a game twice in one process (the determinism self-check
// does) never shares state between the runs.
class Game {
public:
    virtual ~Game() = default;

    // Build the simulated world. Called once, after the engine has registered the definition's
    // components and loaded its entry scene (if any), before the first tick. Return false to refuse
    // to start — the run then reports `RunStatus::Failed` and ticks nothing.
    [[nodiscard]] virtual bool setup(SetupContext& ctx) = 0;

    // One fixed simulation step. Runs on the main thread in the `PostSim` position of the
    // canonical tick (after the Schedule and transform propagation —
    // docs/design/simulation-tick.md), so structural world changes are legal here.
    virtual void fixed_tick(TickContext& ctx) = 0;

    // The game asks to stop: it was won, lost, or quit. Checked after every tick; once true the
    // engine ends the run and ticks the game no further.
    [[nodiscard]] virtual bool finished() const { return false; }

    // An optional in-process player: a deterministic input source the engine can use when no human
    // or network peer is driving — a self-check, an attract mode, a dedicated-server soak run
    // (`--autopilot`). Asked once per tick, before `fixed_tick`, from the game's own state; return
    // false from `has_autopilot` if the game ships none.
    [[nodiscard]] virtual bool has_autopilot() const { return false; }

    [[nodiscard]] virtual ActionState autopilot() const { return {}; }

    // Remote play binds a newly replicated mirror after its initial world is complete. A game
    // keeps its own entity handles; binding them here lets its normal presentation and autopilot
    // read that mirror without simulating or duplicating it.
    [[nodiscard]] virtual bool bind_remote_world(ecs::World&) { return false; }

    // A hash of the game's SIMULATED state — what a dedicated server and a player must agree on.
    // Game-defined because only the game knows which state is authoritative (a camera pose is not,
    // a crate's pose is). 0 means "this game supplies none", and a proof that compares digests must
    // refuse a 0 rather than pass on it.
    [[nodiscard]] virtual std::uint64_t state_digest(const ecs::World& world) const {
        (void)world;
        return 0;
    }

    // ── Presentation (called only in a mode with a device) ──────────────────────────────────
    // Once, after `setup` and before the first frame: add meshes/materials and the render
    // components that point at them, tune the renderer. Must not change what the simulation
    // computes — the dedicated/play digest proof is what holds a game to that.
    virtual void present_setup(PresentSetupContext& ctx) { (void)ctx; }

    // Once per frame, before the scene is drawn: presentation-only state such as a camera.
    virtual void present_frame(PresentFrameContext& ctx) { (void)ctx; }
};

// Register a game's component set into a world. The same shape as `ComponentRegistrar` on the
// editor-host path (editor_host_app.hpp), so a game's registrar serves both its editor host and
// its shipped binary.
using GameComponentRegistrar = std::function<void(ecs::World&)>;

// Everything the engine needs to run a game in any mode. A value type: build it in a function the
// game's `main` calls, and hand it to `run_game`.
struct GameDefinition {
    // For the usage line, the window title and the report. Not an identifier the engine keys on.
    std::string name = "rime-game";

    // The fixed-tick rate. Every mode runs the game at this rate — that is the contract that makes
    // their digests comparable.
    double tick_hz = 60.0;

    // Registers the component types the entry scene names. Optional: a game that builds its world
    // in code registers what it needs inside `setup`.
    GameComponentRegistrar register_components;

    // A `.rscene` loaded into the world before `setup`, STRICTLY (an unknown component type is a
    // refusal, not a skip — a game loading its own content is the case LoadOptions' comment says
    // must fail at the door). Empty: the game builds its world in code.
    //
    // RELATIVE to the content root (m20.2, `rime/app/content_root.hpp`): `--content`, else
    // `<exe dir>/content`, else — only for a binary still in its build directory —
    // `dev_content_dir`. Never relative to the working directory, which is wherever the user
    // happened to double-click from. An ABSOLUTE path is loaded as given and resolves no root; that
    // is for tests and tools, and a game that ships one is pinned to the machine that built it.
    std::string entry_scene;

    // The dev fallback for the content root: the game's content directory in the SOURCE tree, and
    // the directory the build writes the game's executable into. Set both from the build
    // (`rime_game_content()` in CMake defines `RIME_GAME_CONTENT_DIR` / `RIME_GAME_BINARY_DIR`).
    // Declaring `dev_content_dir` also declares that the game HAS content: the runner then resolves
    // a root even with no entry scene, and a missing one is a refusal naming every path it tried.
    std::string dev_content_dir;
    std::string dev_binary_dir;

    // Keyboard bindings for `play`. Ignored by modes with no local keyboard.
    InputMap input;

    // A game opts into remote input by naming its command profile and mapping validated commands
    // to the same actions its local bindings produce. An absent mapper refuses a listening launch.
    struct NetworkInputProfile {
        std::string_view profile_id;
        std::uint32_t profile_version = 0;
        ActionState (*to_actions)(const replication::InputCommand&) = nullptr;
    };

    std::optional<NetworkInputProfile> network_input;

    // Both worlds register the same reflected component schema before constructing a replicator.
    // ServerReplicator currently publishes the full reflected registry. A per-profile filter can
    // follow when a larger game needs it; hello-game's small world does not need one yet.
    // The hooks bind to one world and one owned entity; the server uses capture for checkpoints,
    // while the client uses the same contract for prediction and reconciliation.
    struct RemotePlayProfile {
        GameComponentRegistrar register_replicated;
        ecs::Entity (*owned_player)(ecs::World&) = nullptr;
        replication::PredictionHooks (*prediction_hooks)(ecs::World&, ecs::Entity) = nullptr;
        double fixed_dt = 0.0;
        std::uint64_t (*digest)(const ecs::World&) = nullptr;
        replication::InputCommand (*to_command)(const ActionState&) = nullptr;
        ecs::Entity (*seed_prediction)(ecs::World&, ecs::World&, ecs::Entity) = nullptr;
        // The same local pose the renderer presents, queried without a GPU for response and snap
        // diagnostics. Games may keep controller position outside their transform component.
        core::Transform (*player_pose)(const ecs::World&, ecs::Entity) = nullptr;
        // A fixed command tape keeps the process proof independent of replicated-state timing.
        ActionState (*diagnostic_input_tape)(std::uint64_t) = nullptr;

        // m20.5b (ADR-0105): opt into SPECULATIVE SPAWN -- the game launches entities from
        // commands, reports them through `TickContext::spawns`, and the server answers each key
        // with a durable outcome in the owner checkpoint's spawn extension. Absent, nothing about
        // remote play changes: the same input identity, the m20.4 checkpoint bytes, the same
        // counters.
        //
        // NEGOTIATED THROUGH THE SCHEMA IDENTITY, not a new hello field: `input_schema_identity`
        // folds the profile's presence and `version` in, so a client and a server that disagree
        // about it are refused at the hello as `Schema` before either can send the other a
        // checkpoint it would refuse (an m20.4 decoder treats the extension as trailing bytes).
        struct SpawnPredictionProfile {
            std::uint32_t version = 1;
            // Server: the state bytes of one SPAWNED roster member, as the owner checkpoint
            // carries it (the player still goes through `prediction_hooks`' capture). Bound to
            // an entity rather than a PredictedEntityId because the server knows its own entity;
            // the id on the wire is that entity's NetId. Empty bytes refuse the checkpoint.
            std::vector<std::byte> (*capture_spawned)(const ecs::World&, ecs::Entity) = nullptr;
        };

        std::optional<SpawnPredictionProfile> spawn_prediction;
    };

    std::optional<RemotePlayProfile> remote_play;

    // Make a fresh game. Required.
    std::function<std::unique_ptr<Game>()> create;
};

// The input identity a client and a server must agree on before any command flows (ADR-0103 §1,
// ADR-0104). ONE function, used by the server pump and by every client, because the identity was
// once spelled out in three places and the proof client drifted from the other two the moment
// the rule changed. A remote-playable game's identity includes its fixed timestep: a client
// predicting at 1/30 s against a 1/60 s server would replay every command at the wrong dt and
// read as a stream of corrections, so it is refused at the hello as a schema mismatch instead.
// The same holds for speculative spawn (m20.5b): a declared `spawn_prediction` folds a marker and
// its version in after the m20.4 hash, and an absent one leaves the m20.4 value bit for bit.
[[nodiscard]] std::uint64_t
input_schema_identity(const GameDefinition::NetworkInputProfile& input,
                      const GameDefinition::RemotePlayProfile* remote_play);

} // namespace rime::app
