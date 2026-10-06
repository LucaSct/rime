// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The engine-owned game runner (m20.1, ADR-0046 §1, ADR-0056-m20.1): the launch-mode grammar, the
// "not yet" stubs, the one decision that keeps `dedicated` off the GPU, and the InputMapper that
// turns keys into the actions a game consumes.
//
// The central proof is the device-factory COUNT. "Dedicated has no window" would pass on any
// headless CI box whether or not a device was made; "dedicated entered the factory zero times"
// cannot. `samples/hello-game --dedicated-proof` carries the same claim into a real game and adds
// the process-level half (no libvulkan mapped); this suite pins the engine's side with a game that
// has no rules at all, so a failure here is the runner's and not a game's.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rime/app/game_definition.hpp"
#include "rime/app/launch.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/rhi/device.hpp"
#include "rime/scene/scene_format.hpp"

using namespace rime;
using namespace rime::app;

namespace {

// What a probe game saw, kept outside the game because the runner destroys the game on return.
struct Observed {
    int created = 0;
    int setups = 0;
    std::uint64_t ticks = 0;
    int present_setups = 0;
    int present_frames = 0;
    std::size_t entities_at_setup = 0;
    std::size_t world_transforms_at_setup = 0;
    std::filesystem::path content_root_at_setup;
    std::vector<float> axis0_per_tick;
    double dt_seen = 0.0;
};

class ProbeGame final : public Game {
public:
    ProbeGame(Observed& seen, std::uint64_t finish_after, bool autopilot, bool refuse_setup)
        : seen_(seen), finish_after_(finish_after), autopilot_(autopilot),
          refuse_setup_(refuse_setup) {}

    bool setup(SetupContext& ctx) override {
        ++seen_.setups;
        seen_.entities_at_setup = ctx.world.entity_count();
        ctx.world.query<ecs::WorldTransform>().for_each(
            [this](ecs::Entity, ecs::WorldTransform&) { ++seen_.world_transforms_at_setup; });
        seen_.content_root_at_setup = ctx.content_root;
        return !refuse_setup_;
    }

    void fixed_tick(TickContext& ctx) override {
        CHECK(ctx.tick == seen_.ticks); // the tick index counts the game's own steps, from 0
        ++seen_.ticks;
        seen_.dt_seen = ctx.dt;
        seen_.axis0_per_tick.push_back(ctx.input.axis(0));
    }

    bool finished() const override { return finish_after_ != 0 && seen_.ticks >= finish_after_; }

    bool has_autopilot() const override { return autopilot_; }

    ActionState autopilot() const override {
        ActionState s{};
        s.axes[0] = static_cast<float>(seen_.ticks) + 0.5f; // a value only the autopilot produces
        return s;
    }

    std::uint64_t state_digest(const ecs::World&) const override {
        return 0xABCD0000 + seen_.ticks;
    }

    void present_setup(PresentSetupContext&) override { ++seen_.present_setups; }

    void present_frame(PresentFrameContext&) override { ++seen_.present_frames; }

private:
    Observed& seen_;
    std::uint64_t finish_after_;
    bool autopilot_;
    bool refuse_setup_;
};

GameDefinition probe_definition(Observed& seen,
                                std::uint64_t finish_after = 0,
                                bool autopilot = true,
                                bool refuse_setup = false) {
    GameDefinition def{};
    def.name = "probe";
    def.tick_hz = 30.0;
    def.create = [&seen, finish_after, autopilot, refuse_setup] {
        ++seen.created;
        return std::make_unique<ProbeGame>(seen, finish_after, autopilot, refuse_setup);
    };
    return def;
}

ParseResult parse(std::vector<std::string_view> args) {
    return parse_launch(args);
}

// A factory that counts and never makes a device — so these tests are GPU-free on every OS, and a
// dedicated run that wrongly asks for a device is caught by the count rather than by a crash.
struct CountingFactory {
    int calls = 0;

    RunHooks hooks() {
        RunHooks h{};
        h.device_factory = [this]() -> std::unique_ptr<rhi::Device> {
            ++calls;
            return nullptr;
        };
        return h;
    }
};

LaunchOptions bounded(LaunchMode mode, std::uint64_t ticks) {
    LaunchOptions o{};
    o.mode = mode;
    o.ticks = ticks;
    o.workers = 1;
    o.headless = true;
    return o;
}

platform::Event key(platform::Key k, bool down, bool repeat = false) {
    platform::Event e{};
    e.type = down ? platform::EventType::KeyDown : platform::EventType::KeyUp;
    e.key.key = k;
    e.key.repeat = repeat;
    return e;
}

} // namespace

// ── The grammar ───────────────────────────────────────────────────────────────────────────────

TEST_CASE("launch: no arguments is play, and every mode word parses") {
    const ParseResult none = parse({});
    REQUIRE(none.status == ParseStatus::Ok);
    CHECK(none.options.mode == LaunchMode::Play);

    const std::array<std::pair<std::string_view, LaunchMode>, 5> words{{
        {"play", LaunchMode::Play},
        {"dedicated", LaunchMode::Dedicated},
        {"browser", LaunchMode::Browser},
        {"stream", LaunchMode::Stream},
        {"host", LaunchMode::Host},
    }};
    for (const auto& [word, mode] : words) {
        const ParseResult r = parse({word});
        REQUIRE(r.status == ParseStatus::Ok);
        CHECK(r.options.mode == mode);
        CHECK(to_string(mode) == word); // the report names the mode the user typed
    }
}

TEST_CASE("launch: options parse, and malformed numbers are refused rather than read as zero") {
    const ParseResult ok =
        parse({"dedicated", "--port", "7777", "--ticks", "600", "--autopilot", "--workers", "3"});
    REQUIRE(ok.status == ParseStatus::Ok);
    CHECK(ok.options.mode == LaunchMode::Dedicated);
    CHECK(ok.options.port == 7777);
    CHECK(ok.options.ticks == 600);
    CHECK(ok.options.autopilot);
    CHECK(ok.options.workers == 3);

    // atoi("abc") == 0 and atoi("12x") == 12: exactly the silent misreads this parser exists to
    // refuse. Each is its own case so one accepted spelling cannot hide behind another's refusal.
    CHECK(parse({"--ticks", "abc"}).status == ParseStatus::Error);
    CHECK(parse({"--ticks", "12x"}).status == ParseStatus::Error);
    CHECK(parse({"--ticks", "0"}).status == ParseStatus::Error);
    CHECK(parse({"--ticks"}).status == ParseStatus::Error);
    CHECK(parse({"--port", "0"}).status == ParseStatus::Error);
    CHECK(parse({"--port", "65536"}).status == ParseStatus::Error);
    CHECK(parse({"--frames", "-1"}).status == ParseStatus::Error);
    CHECK(parse({"--bogus"}).status == ParseStatus::Error);
    CHECK(parse({"warp"}).status == ParseStatus::Error);
    CHECK(parse({"--help"}).status == ParseStatus::Help);
}

TEST_CASE("launch: the mode word must come first, and nonsense combinations are refused") {
    // Positional-first: a second grammar in which the mode may float is refused, not tolerated.
    const ParseResult late = parse({"--ticks", "5", "dedicated"});
    CHECK(late.status == ParseStatus::Error);
    CHECK(late.error.find("dedicated") != std::string::npos);

    // Dedicated renders nothing, so a frame bound is a request it cannot honour.
    CHECK(parse({"dedicated", "--frames", "8"}).status == ParseStatus::Error);
    CHECK(parse({"play", "--frames", "8"}).status == ParseStatus::Ok);
}

TEST_CASE("launch: only dedicated is device-free by definition") {
    CHECK_FALSE(mode_uses_device(LaunchMode::Dedicated));
    CHECK(mode_uses_device(LaunchMode::Play));
    CHECK(mode_uses_device(LaunchMode::Browser));
    CHECK(mode_uses_device(LaunchMode::Stream));
    CHECK(mode_uses_device(LaunchMode::Host));
}

// ── The stubs ─────────────────────────────────────────────────────────────────────────────────

TEST_CASE("launch: browser, stream and host are refused by name before anything is built") {
    for (const LaunchMode mode : {LaunchMode::Browser, LaunchMode::Stream, LaunchMode::Host}) {
        Observed seen;
        CountingFactory factory;
        const RunReport r =
            run_game_mode(bounded(mode, 10), probe_definition(seen), factory.hooks());
        CHECK(r.status == RunStatus::NotYetImplemented);
        CHECK(r.message.find(std::string(to_string(mode))) != std::string::npos);
        // Refused BEFORE construction: no device asked for, no game created, nothing ticked. A stub
        // that quietly ran `play` would show up here as a created game and ticks.
        CHECK(factory.calls == 0);
        CHECK(seen.created == 0);
        CHECK(seen.ticks == 0);
    }
}

TEST_CASE("launch: run_game maps outcomes to distinct exit codes") {
    Observed seen;
    const GameDefinition def = probe_definition(seen, 0, true);
    auto run = [&](std::vector<std::string> words) {
        std::vector<char*> argv;
        std::string program = "probe";
        argv.push_back(program.data());
        for (std::string& w : words) {
            argv.push_back(w.data());
        }
        return run_game(static_cast<int>(argv.size()), argv.data(), def);
    };
    CHECK(run({"browser", "--port", "8443"}) == kExitNotYet);
    CHECK(run({"stream"}) == kExitNotYet);
    CHECK(run({"host"}) == kExitNotYet);
    CHECK(run({"warp"}) == kExitUsage);
    CHECK(run({"dedicated", "--ticks", "3", "--autopilot", "--workers", "1"}) == kExitOk);
    CHECK(seen.ticks == 3);
}

// ── The decision that keeps dedicated off the GPU ────────────────────────────────────────────

TEST_CASE("launch: dedicated never enters the device factory and never presents") {
    Observed seen;
    CountingFactory factory;
    const RunReport r =
        run_game_mode(bounded(LaunchMode::Dedicated, 25), probe_definition(seen), factory.hooks());
    REQUIRE(r.status == RunStatus::Ok);
    CHECK(factory.calls == 0);
    CHECK(r.device_factory_calls == 0);
    CHECK_FALSE(r.had_device);
    CHECK_FALSE(r.windowed);
    CHECK(seen.present_setups == 0);
    CHECK(seen.present_frames == 0);
    CHECK(r.presentation_skipped == 0); // nothing was SKIPPED: dedicated never wanted it
    // The bounded run is exact: 25 frames, 25 ticks, each at the definition's fixed dt.
    CHECK(r.ticks == 25);
    CHECK(r.frames == 25);
    CHECK(seen.dt_seen == doctest::Approx(1.0 / 30.0));
    CHECK(r.state_digest == 0xABCD0000 + 25);
    // No --autopilot and no keyboard: a server with no clients hands the game empty input.
    for (const float a : seen.axis0_per_tick) {
        CHECK(a == 0.0f);
    }
}

TEST_CASE("launch: play asks for a device exactly once, and degrades to the sim without one") {
    Observed seen;
    CountingFactory factory;
    const RunReport r =
        run_game_mode(bounded(LaunchMode::Play, 12), probe_definition(seen), factory.hooks());
    REQUIRE(r.status == RunStatus::Ok);
    CHECK(factory.calls == 1);
    CHECK(r.device_factory_calls == 1);
    CHECK_FALSE(r.had_device);
    CHECK(r.ticks == 12);
    // Presentation was wanted and impossible: never called, and every skip is counted (setup + one
    // per frame) rather than silent.
    CHECK(seen.present_setups == 0);
    CHECK(seen.present_frames == 0);
    CHECK(r.presentation_skipped == 1 + 12);
}

TEST_CASE("launch: the autopilot drives every tick, and a game without one is refused") {
    Observed seen;
    LaunchOptions o = bounded(LaunchMode::Dedicated, 4);
    o.autopilot = true;
    const RunReport r = run_game_mode(o, probe_definition(seen));
    REQUIRE(r.status == RunStatus::Ok);
    REQUIRE(seen.axis0_per_tick.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        // asked BEFORE the tick, from the state the previous ticks left
        CHECK(seen.axis0_per_tick[i] == static_cast<float>(i) + 0.5f);
    }

    Observed none;
    const RunReport refused = run_game_mode(o, probe_definition(none, 0, /*autopilot=*/false));
    CHECK(refused.status == RunStatus::Failed);
    CHECK(none.ticks == 0);
}

TEST_CASE("launch: a finished game stops the run, and a refused setup ticks nothing") {
    Observed seen;
    const RunReport r =
        run_game_mode(bounded(LaunchMode::Dedicated, 100), probe_definition(seen, /*after=*/7));
    CHECK(r.status == RunStatus::Ok);
    CHECK(r.finished);
    CHECK(r.ticks == 7);
    CHECK(seen.ticks == 7);

    Observed refused;
    const RunReport f = run_game_mode(bounded(LaunchMode::Dedicated, 100),
                                      probe_definition(refused, 0, true, /*refuse_setup=*/true));
    CHECK(f.status == RunStatus::Failed);
    CHECK(refused.setups == 1);
    CHECK(refused.ticks == 0);

    GameDefinition empty{};
    CHECK(run_game_mode(bounded(LaunchMode::Dedicated, 1), empty).status == RunStatus::Failed);
}

TEST_CASE("launch: an entry scene is loaded strictly before setup") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rime_launch_test_entry.rscene";
    {
        ecs::World authored;
        ecs::register_transform_components(authored);
        core::Transform t{};
        t.translation = {3.0f, 0.0f, 0.0f};
        (void)authored.spawn_with(ecs::LocalTransform{t});
        (void)authored.spawn_with(ecs::LocalTransform{core::Transform{}});
        REQUIRE(scene::save_scene_file(authored, path));
    }

    Observed seen;
    GameDefinition def = probe_definition(seen);
    def.entry_scene = path.string();
    def.register_components = [](ecs::World& w) { ecs::register_transform_components(w); };
    const RunReport r = run_game_mode(bounded(LaunchMode::Dedicated, 1), def);
    CHECK(r.status == RunStatus::Ok);
    CHECK(seen.entities_at_setup == 2); // the scene was in the world when the game's setup ran

    // Strict: without the registrar the scene names types this world does not know, and a game
    // loading its OWN content must fail at the door rather than run on a half-loaded world.
    Observed strict;
    GameDefinition unregistered = probe_definition(strict);
    unregistered.entry_scene = path.string();
    const RunReport refused = run_game_mode(bounded(LaunchMode::Dedicated, 1), unregistered);
    CHECK(refused.status == RunStatus::Failed);
    CHECK(strict.setups == 0);

    Observed missing;
    GameDefinition nowhere = probe_definition(missing);
    nowhere.entry_scene = (path.parent_path() / "rime_launch_test_missing.rscene").string();
    CHECK(run_game_mode(bounded(LaunchMode::Dedicated, 1), nowhere).status == RunStatus::Failed);

    std::filesystem::remove(path);
}

TEST_CASE("launch: a relative entry scene resolves against the content root, never the cwd") {
    // m20.2. A directory holding the scene, named with --content: the run finds it, hands the root
    // to `setup`, and composes WorldTransforms for what it loaded — m20.1 only re-propagated
    // existing ones, so a loaded entity reached `setup` (and PhysicsSync) with no WorldTransform.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "rime_launch_test_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        ecs::World authored;
        ecs::register_transform_components(authored);
        (void)authored.spawn_with(ecs::LocalTransform{core::Transform{}});
        REQUIRE(scene::save_scene_file(authored, root / "level.rscene"));
    }

    Observed seen;
    GameDefinition def = probe_definition(seen);
    def.entry_scene = "level.rscene";
    def.register_components = [](ecs::World& w) { ecs::register_transform_components(w); };
    LaunchOptions options = bounded(LaunchMode::Dedicated, 1);
    options.content_dir = root.string();
    RunHooks quiet{};
    quiet.log_content_root = false;
    const RunReport r = run_game_mode(options, def, quiet);
    CHECK(r.status == RunStatus::Ok);
    CHECK(r.content.ok);
    CHECK(r.content.source == ContentSource::Explicit);
    CHECK(std::filesystem::equivalent(seen.content_root_at_setup, root));
    CHECK(seen.entities_at_setup == 1);
    CHECK(seen.world_transforms_at_setup == 1);

    // The same game with its content gone: refused BEFORE a game is created, and the report names
    // where it looked rather than running on an empty world.
    std::filesystem::remove_all(root);
    Observed gone;
    GameDefinition missing = probe_definition(gone);
    missing.entry_scene = "level.rscene";
    missing.register_components = def.register_components;
    const RunReport refused = run_game_mode(options, missing, quiet);
    CHECK(refused.status == RunStatus::Failed);
    CHECK(gone.created == 0);
    CHECK_FALSE(refused.content.ok);
    CHECK(refused.message.find("no content root found") != std::string::npos);
    CHECK(refused.message.find("rime_launch_test_content: missing") != std::string::npos);
}

TEST_CASE("launch: --content parses, and needs a value") {
    const ParseResult ok = parse({"dedicated", "--content", "some/dir"});
    REQUIRE(ok.status == ParseStatus::Ok);
    CHECK(ok.options.content_dir == "some/dir");
    CHECK(parse({"dedicated", "--content"}).status == ParseStatus::Error);
}

// ── Input: actions, never keys ────────────────────────────────────────────────────────────────

TEST_CASE("input: held keys sum onto axes, and auto-repeat is not a second press") {
    InputMap map;
    map.axes = {{platform::Key::W, 1, -1.0f}, {platform::Key::S, 1, +1.0f}};
    map.buttons = {{platform::Key::Escape, 0}};
    InputMapper m(map);

    const std::array down_w{key(platform::Key::W, true)};
    m.update(down_w);
    ActionState s = m.take();
    CHECK(s.axis(1) == -1.0f);

    // Both held: they cancel, rather than the last event winning.
    const std::array down_s{key(platform::Key::S, true)};
    m.update(down_s);
    CHECK(m.take().axis(1) == 0.0f);

    const std::array up_both{key(platform::Key::W, false), key(platform::Key::S, false)};
    m.update(up_both);
    CHECK(m.take().axis(1) == 0.0f);

    // A press edge is delivered to exactly ONE tick; the repeat that follows is not a new press.
    const std::array esc{key(platform::Key::Escape, true), key(platform::Key::Escape, true, true)};
    m.update(esc);
    s = m.take();
    CHECK(s.was_pressed(0));
    CHECK(s.is_held(0));
    s = m.take();
    CHECK_FALSE(s.was_pressed(0));
    CHECK(s.is_held(0));
}

TEST_CASE("input: a tap inside one frame still reaches a tick") {
    // Down and up in the same frame leaves nothing HELD by the time a tick looks — the case that
    // makes a quit key sometimes ignored, and the reason `pressed` is latched separately.
    InputMap map;
    map.buttons = {{platform::Key::Escape, 3}};
    InputMapper m(map);
    const std::array tap{key(platform::Key::Escape, true), key(platform::Key::Escape, false)};
    m.update(tap);
    const ActionState s = m.take();
    CHECK(s.was_pressed(3));
    CHECK_FALSE(s.is_held(3));
}

TEST_CASE("input: out-of-range bindings are counted and dropped, not honoured") {
    InputMap map;
    map.axes = {{platform::Key::A, 0, 1.0f}, {platform::Key::B, ActionState::kAxes, 1.0f}};
    map.buttons = {{platform::Key::C, ActionState::kButtons}, {platform::Key::Count, 0}};
    InputMapper m(map);
    CHECK(m.rejected_bindings() == 3);

    // And a run reports what its definition's map lost.
    Observed seen;
    GameDefinition def = probe_definition(seen);
    def.input = map;
    const RunReport r = run_game_mode(bounded(LaunchMode::Dedicated, 1), def);
    CHECK(r.rejected_bindings == 3);
}
