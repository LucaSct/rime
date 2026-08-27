// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 13-networked-player — Milestone 12's "done when" (see README.md for the design).
//
// A dedicated headless server owns the simulation. Two clients connect, drive the SAME scripted
// input tape over the SAME lossy link, and differ in exactly one thing: **client 0 predicts;
// client 1 does not.** That is the whole shape of this proof. ADR-0035 §1 asks for own-input
// response "≤ 1 tick, against a prediction-off control showing ≥ RTT ticks, so prediction is
// provably THE REASON" — and the cheapest way to be wrong about that is to measure two runs on two
// links and compare numbers that were never comparable. Here the control is a PEER IN THE SAME
// MATCH: same server, same tick, same scripted losses, same tape. One boolean apart.
//
// WHAT THIS PROVES, and it is deliberately narrower than "the block works":
//
//   * own-input response ≤ 1 tick for the predicting client, ≥ RTT ticks for the control;
//   * both clients converge on the server's authoritative state BIT FOR BIT at quiescence;
//   * a remote player is drawn moving CONTINUOUSLY — no frame-to-frame lurch, which is the m12.5
//     interpolation clause and the one that is invisible in state;
//   * the whole thing survives 20% packet loss, with corrections actually happening.
//
// WHAT IT DELIBERATELY DOES NOT PROVE. Frame rate, GI, scale, and "feels right" are M13's, and this
// binary never opens a window or touches a GPU. Networked *destruction* is M11's and is proven by
// `12-networked-destruction`; here the wall exists server-side so that "shoot" means something and
// so that the m12.3 weapon→destruction glue is exercised end to end in a real program. The clients
// do not mirror the destruction: their claim is about the PLAYER.
//
// A NOTE ON THE LEVEL. The clients carry the same floor the server does, because a client that
// cannot see the ground cannot predict standing on it. They do NOT carry the wall's collision, so
// the tape keeps both players well clear of it — walking into geometry only one side knows about
// would produce a correction storm that says nothing about prediction and everything about the
// level being half-loaded. Stated rather than hidden, because it is a real limit of this sample.
//
// Run it:  build/dev/bin/networked_player --headless [--cooked <dir>]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/components.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/gameplay_net/components.hpp"
#include "rime/gameplay_net/gameplay_client.hpp"
#include "rime/gameplay_net/gameplay_server.hpp"
#include "rime/gameplay_net/predictor.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/interpolation.hpp"
#include "rime/replication/server_replicator.hpp"

#ifndef RIME_WALL_COOKED_DIR
#define RIME_WALL_COOKED_DIR "cooked"
#endif

using namespace rime;

namespace {

// ── The scenario ─────────────────────────────────────────────────────────────────────────────
// Every number is chosen so a piece of M12 is EXERCISED rather than trivially satisfied, and each
// is asserted on at the end. Too easy is the quiet failure: a run over a perfect link with a
// stationary player is green for reasons that have nothing to do with prediction.

constexpr std::uint64_t kTickMs = 16;
constexpr float kDt = 1.0f / 60.0f;

// 48 ms each way at a 16 ms tick: 3 ticks out, 3 back. Fixed rather than jittered, because the
// control's claim is "≥ the round trip" and a distribution would make that an argument about
// percentiles instead of about mechanism.
constexpr std::uint64_t kOneWayMs = 48;
constexpr int kRoundTripTicks = static_cast<int>(2 * kOneWayMs / kTickMs);

// Real loss, so corrections really happen. A run with zero corrections would prove the comparison
// is dead rather than that the network was kind (ADR-0035 §4).
constexpr float kLossRate = 0.20f;

constexpr std::uint64_t kMatchTicks = 420;

// The tape's phases. Both players stand still until kStepOffTick so that "how long until my own
// input moves me" has a clean, scripted zero to measure from.
constexpr std::uint64_t kStepOffTick = 60;
constexpr std::uint64_t kFireFromTick = 120;
constexpr std::uint64_t kStopTick = 330; // …then quiesce, which is where exactness is demanded

constexpr std::uint64_t kAssetId = 0x5741'4C4Cull; // 'WALL'
constexpr float kWallZ = -14.0f;                   // well clear of where the tape walks

// The floor is TILED: 10 m per box is a measured GJK limit (a larger box swallows shallow overlaps
// and reports nothing), not a limit on level size.
constexpr float kTileHalf = 10.0f;
constexpr int kTilesPerAxis = 3;

// ── Small helpers ────────────────────────────────────────────────────────────────────────────

physics::BodyId add_static_box(physics::PhysicsWorld& w, core::Vec3 half, core::Vec3 pos) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape.type = physics::ShapeType::Box;
    d.shape.half_extents = half;
    d.position = pos;
    return w.create_body(d);
}

void stand_level(physics::PhysicsWorld& w) {
    const float pitch = 2.0f * kTileHalf - 0.2f; // overlap, so there is no seam to fall through
    const int half = kTilesPerAxis / 2;
    for (int ix = -half; ix <= half; ++ix) {
        for (int iz = -half; iz <= half; ++iz) {
            (void)add_static_box(
                w,
                {kTileHalf, 0.5f, kTileHalf},
                {static_cast<float>(ix) * pitch, -0.5f, static_cast<float>(iz) * pitch});
        }
    }
}

float rest_y(const gameplay::CharacterConfig& c) {
    return c.half_height + c.radius + 1.5f * c.skin;
}

void register_all(ecs::World& world) {
    ecs::register_transform_components(world);
    physics::register_physics_components(world);
    destruction::register_destruction_components(world);
    gameplay::register_gameplay_components(world);
    gameplay_net::register_gameplay_net_components(world);
}

// ── The scripted tape (ADR-0033 A6) ──────────────────────────────────────────────────────────
//
// Input is DATA — a pure function of (player, tick) — never anything derived from the simulation.
// A tape that aimed by querying the world would re-introduce exactly the float dependence that
// makes a proof unreproducible, and would make the two clients' tapes differ the moment their
// predictions did.
[[nodiscard]] replication::InputCommand tape(int player, std::uint64_t tick) {
    replication::InputCommand c;
    if (tick < kStepOffTick || tick >= kStopTick) {
        // Standing still — and still SENDING. A player at rest still transmits, and the frontier
        // only steps over a permanently-lost command when a later one arrives, so silence at the
        // end would leave the peers a tick apart forever.
        return c;
    }

    // STRAFING IN A TIGHT ELLIPSE WHILE FACING THE WALL, in opposite directions per player.
    //
    // Facing the wall is what lets a tape that cannot look at the world still aim: the engine basis
    // puts forward at -Z for yaw 0, and the wall is straight down -Z, so a level shot at yaw ≈ 0
    // lands on it from anywhere this ellipse reaches. Yaw sweeps a little so the shots spread
    // across the wall's width instead of drilling one part.
    //
    // TIGHT is load-bearing, and the first draft was not. A circle traced at 0.03 rad/tick has
    // radius max_speed/omega ≈ 3.3 m, which walks the players far enough that (a) the aim leaves
    // the wall and (b) they can reach geometry the CLIENTS do not have — the wall's collision is
    // server-side only here, and walking into it would produce a correction storm that says
    // nothing about prediction. At 0.15 rad/tick the radius is ~0.67 m and both problems go away.
    const float phase = static_cast<float>(tick - kStepOffTick) * 0.15f;
    const float dir = player == 0 ? 1.0f : -1.0f;
    c.move_x = std::sin(phase * dir);
    c.move_y = 0.3f * std::cos(phase * dir); // shallow in Z, so they hold their distance
    c.yaw = 0.05f * std::sin(phase * 0.5f * dir);

    if (tick >= kFireFromTick) {
        const std::uint64_t period = player == 0 ? 9u : 13u;
        if ((tick - kFireFromTick) % period == 0) {
            c.pressed |= gameplay::kActionFire;
            c.held |= gameplay::kActionFire;
        }
    }
    return c;
}

// ── The peers ────────────────────────────────────────────────────────────────────────────────

struct ClientPeer {
    ecs::World world;
    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    core::JobSystem jobs{0};

    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::unique_ptr<replication::ClientReplicator> replicator;
    replication::ClientInputSender sender;
    gameplay_net::GameplayClient gameplay;
    gameplay_net::Predictor predictor;
    gameplay::CharacterConfig config{};
    bool predict = false;

    // What this client last drew its own avatar at, and the largest single-tick step it ever drew
    // for the OTHER player. The second is m12.5's clause, and it is measured on FRAMES rather than
    // ticks because motion between ticks is exactly what a tick-sampled proof cannot see.
    core::Vec3 own_drawn{};
    bool own_drawn_valid = false;
    float worst_remote_step = 0.0f;
    std::uint64_t remote_frames = 0;
    std::uint64_t remote_frames_over_bound = 0;
    core::Vec3 last_remote_frame{};
    bool last_remote_valid = false;

    [[nodiscard]] ecs::Entity local_player() const {
        return gameplay.local_player(replicator->map());
    }

    [[nodiscard]] physics::BodyId local_body() const {
        const ecs::Entity e = local_player();
        if (!e.is_valid()) {
            return physics::BodyId{};
        }
        const auto* h = world.get<physics::RigidBodyHandle>(e);
        return h != nullptr ? h->body : physics::BodyId{};
    }

    // The SIMULATION's answer for this client's own avatar.
    [[nodiscard]] gameplay::CharacterState own_state() const {
        if (predict && predictor.seeded()) {
            return predictor.state();
        }
        const ecs::Entity e = local_player();
        const auto* s = e.is_valid() ? world.get<gameplay::CharacterState>(e) : nullptr;
        return s != nullptr ? *s : gameplay::CharacterState{};
    }

    // What a renderer would DRAW for it — the smoothed pose when predicting.
    [[nodiscard]] core::Vec3 own_draw_position() const {
        if (predict && predictor.seeded()) {
            return predictor.visual_position();
        }
        return own_state().position;
    }
};

struct Match {
    net::ScriptedNetwork network;
    net::Endpoint server_endpoint{0x7F000001u, 7801};

    ecs::World world;
    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    core::JobSystem jobs{0};
    destruction::DestructionWorld destruction;
    destruction::PatternId pattern{};
    ecs::Entity wall = ecs::kNullEntity;

    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::unique_ptr<replication::ServerReplicator> replicator;
    replication::ServerInputReceiver input;
    gameplay_net::GameplayServer gameplay;

    std::vector<std::unique_ptr<ClientPeer>> clients;
    std::vector<ecs::Entity> avatars;

    gameplay::CharacterConfig character{};
    gameplay::WeaponConfig weapon{};

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    // The tick the TAPE counts from. Set once the handshake has settled, so that "the tape steps
    // off at tick 60" and "the measurement starts counting at tick 60" are the same tick 60. They
    // were not in the first draft — the tape ran on absolute ticks and the measurement on
    // post-settle ticks, so by the time the measurement looked, the players had been walking for
    // three hundred ticks and BOTH clients answered "1 tick" for a reason that had nothing to do
    // with prediction.
    std::uint64_t tape_origin = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    explicit Match(std::uint64_t seed) : network(seed, {kLossRate, 0.0f, kOneWayMs, kOneWayMs}) {
        register_all(world);
        stand_level(physics);
        link = &network.add_node(server_endpoint);

        net::NetDriver::Config config;
        config.app_id = 0x52494D45u;
        config.schema_hash = ecs::component_schema_hash(world);
        config.salt_seed = 0x1111ull;
        driver = std::make_unique<net::NetDriver>(*link, config);
        driver->listen();
        replicator = std::make_unique<replication::ServerReplicator>(world);

        // A rifle, not a demolition charge: cooked parts stand at 1.0 health, so the damage number
        // is expressed against that scale and the wall takes sustained fire rather than one shot.
        weapon.damage = 0.34f;
        weapon.damage_radius = 0.16f;
        weapon.cooldown_ticks = 0;
    }

    ClientPeer& add_client(bool predict, std::uint16_t port, std::uint64_t salt) {
        auto peer = std::make_unique<ClientPeer>();
        register_all(peer->world);
        stand_level(peer->physics); // the same level, loaded independently — as a game would
        peer->predict = predict;

        const net::Endpoint endpoint{0x7F000001u, port};
        peer->link = &network.add_node(endpoint);
        net::NetDriver::Config config;
        config.app_id = 0x52494D45u;
        config.schema_hash = ecs::component_schema_hash(peer->world);
        config.salt_seed = salt;
        peer->driver = std::make_unique<net::NetDriver>(*peer->link, config);
        peer->replicator = std::make_unique<replication::ClientReplicator>(peer->world);
        (void)peer->world.register_component<ecs::RenderTransform>();

        clients.push_back(std::move(peer));
        ClientPeer& added = *clients.back();
        (void)added.driver->connect(server_endpoint, now_ms);
        return added;
    }

    // The game's spawn policy — kept in the sample, not in the engine, exactly as ADR-0035 §3 says.
    ecs::Entity spawn_avatar(net::SessionId) {
        core::Transform placement;
        placement.translation = {
            static_cast<float>(avatars.size()) * 3.0f, rest_y(character), 4.0f};

        physics::RigidBody body;
        body.motion = static_cast<std::uint32_t>(physics::MotionType::Kinematic);
        physics::Collider collider;
        collider.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Capsule);
        collider.radius = character.radius;
        collider.half_height = character.half_height;

        gameplay::CharacterState state;
        state.position = placement.translation;

        const ecs::Entity e = world.spawn_with(ecs::LocalTransform{placement},
                                               ecs::WorldTransform{placement},
                                               body,
                                               collider,
                                               character,
                                               state,
                                               weapon,
                                               gameplay::WeaponState{});
        (void)replicator->replicate(e);
        avatars.push_back(e);
        return e;
    }

    void stand_wall(const assets::DestructibleAsset& asset) {
        pattern = destruction.register_pattern(asset, physics);
        core::Transform placement;
        placement.translation = {0.0f, 1.5f, kWallZ};
        wall = world.spawn_with(ecs::LocalTransform{placement},
                                ecs::WorldTransform{placement},
                                destruction::Destructible{kAssetId});
        (void)destruction::bind_destructibles(
            world,
            destruction,
            physics,
            [this](std::uint64_t id) {
                return id == kAssetId ? pattern : destruction::PatternId{};
            },
            destruction::Authority::Local);
    }

    // ── One tick of the whole system ─────────────────────────────────────────────────────────
    void tick() {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        // ── PreSim: server ──
        events.clear();
        driver->update(now_ms, events);
        replicator->on_session_events(events);
        input.on_session_events(events);
        gameplay.on_session_events(
            events,
            [this](net::SessionId id) { return spawn_avatar(id); },
            [this](net::SessionId, ecs::Entity p) { replicator->despawn(p); });
        for (const net::SessionId id : driver->session_ids()) {
            net::Session* s = driver->session(id);
            if (s == nullptr) {
                continue;
            }
            inbox.clear();
            (void)s->drain_received(inbox);
            (void)replicator->apply_messages(id, inbox);
            (void)input.apply_messages(id, inbox);
        }

        // ── PreSim: clients — send this tick's tape, then apply mail, then predict ──
        for (std::size_t i = 0; i < clients.size(); ++i) {
            ClientPeer& c = *clients[i];
            const std::uint64_t tape_tick =
                tick_index >= tape_origin ? tick_index - tape_origin : 0;
            const replication::InputCommand command =
                c.sender.record(tape(static_cast<int>(i), tape_tick));
            c.sender.send(*c.driver, now_ms);

            events.clear();
            c.driver->update(now_ms, events);
            for (const net::SessionId id : c.driver->session_ids()) {
                net::Session* s = c.driver->session(id);
                if (s == nullptr) {
                    continue;
                }
                inbox.clear();
                (void)s->drain_received(inbox);
                (void)c.replicator->apply_messages(inbox);
                c.sender.apply_messages(inbox);
                (void)c.gameplay.apply_messages(inbox);
            }

            ecs::propagate_transforms(c.world, c.jobs);
            c.sync.reconcile(c.world, c.physics);

            const ecs::Entity avatar = c.local_player();
            const physics::BodyId self = c.local_body();
            if (c.predict && avatar.is_valid() && self.is_valid()) {
                if (const auto* cfg = c.world.get<gameplay::CharacterConfig>(avatar)) {
                    c.config = *cfg;
                }
                if (const auto* auth = c.world.get<gameplay::CharacterState>(avatar)) {
                    (void)c.predictor.reconcile(
                        *auth,
                        c.gameplay.last_processed_input(c.world, c.replicator->map()),
                        c.config,
                        c.physics,
                        self,
                        kDt);
                }
                (void)c.predictor.predict(command, c.config, c.physics, self, kDt, nullptr);
                if (c.predictor.seeded()) {
                    // The SMOOTHED pose goes to the transforms; `state()` stays the simulation's.
                    gameplay::write_character_pose(c.world, avatar, c.predictor.visual_position());
                }
            }
            c.sync.push_in(c.world, c.physics, kDt);
            c.physics.step(kDt);
            c.sync.write_back(c.world, c.physics);
        }

        // ── The server's simulation ──
        world.advance_version();
        gameplay.consume(world, physics, input, kDt);
        ecs::propagate_transforms(world, jobs);
        sync.reconcile(world, physics);
        sync.push_in(world, physics, kDt);
        physics.step(kDt);
        sync.write_back(world, physics);

        // The weapon → destruction glue: the consumer's job, twenty lines, kept out of the engine
        // so `gameplay_net` never links `destruction` (ADR-0035 §3).
        for (const gameplay_net::ShotEvent& shot : gameplay.shots()) {
            if (!shot.did_hit) {
                continue; // a miss is still an event — a tracer and a report, no damage
            }
            destruction::InstanceId instance{};
            for (std::size_t i = 0; i < destruction.instance_count(); ++i) {
                const destruction::InstanceId candidate{static_cast<std::uint32_t>(i), 0};
                if (destruction.body_of(candidate) == shot.body) {
                    instance = candidate;
                    break;
                }
            }
            if (!instance.is_valid()) {
                continue; // the floor, or rubble — not our business
            }
            if (destruction.part_from_child(instance, shot.child) ==
                destruction::kInvalidPartIndex) {
                continue;
            }
            destruction.apply_damage(
                instance, shot.point, shot.damage_radius, shot.damage, shot.impulse);
        }
        destruction.update(physics);

        // ── Publish ──
        replicator->publish(*driver, now_ms);
        gameplay.publish(*driver, replicator->map(), now_ms);
        input.send_acks(*driver, now_ms);
        for (auto& c : clients) {
            c->replicator->send_ack(*c->driver, now_ms);
            (void)c->replicator->settle_transform_history();
        }
    }

    // Sample what each client would DRAW this tick, at several sub-tick alphas — the renderer's
    // view. Continuity lives between ticks, so a tick-sampled proof cannot see it.
    void sample_frames() {
        // A remote player walking at max_speed covers max_speed*dt per tick; three ticks of that is
        // generous room for the interval a value covers. A drawn step above it is a lurch.
        const float bound = character.max_speed * kDt * 3.0f;
        for (std::size_t i = 0; i < clients.size(); ++i) {
            ClientPeer& c = *clients[i];
            const core::Vec3 own = c.own_draw_position();
            c.own_drawn = own;
            c.own_drawn_valid = true;

            // The OTHER player's avatar, as this client mirrors it.
            const ecs::Entity mine = c.local_player();
            ecs::Entity remote = ecs::kNullEntity;
            c.replicator->map().for_each([&](replication::NetId, ecs::Entity e) {
                if (e != mine && c.world.get<gameplay::CharacterState>(e) != nullptr) {
                    remote = e;
                }
            });
            if (!remote.is_valid()) {
                return;
            }
            for (int f = 0; f < 4; ++f) {
                const float alpha = static_cast<float>(f) * 0.25f;
                const core::Vec3 p =
                    replication::interpolated_transform(c.world, remote, alpha).translation;
                if (c.last_remote_valid) {
                    const float step = core::length(p - c.last_remote_frame);
                    c.worst_remote_step = std::max(c.worst_remote_step, step);
                    ++c.remote_frames;
                    if (step > bound) {
                        ++c.remote_frames_over_bound;
                    }
                }
                c.last_remote_frame = p;
                c.last_remote_valid = true;
            }
        }
    }
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return {};
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    std::string cooked_dir = RIME_WALL_COOKED_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cooked" && i + 1 < argc) {
            cooked_dir = argv[++i];
        } else if (a == "--headless") {
            // The CI entry point and the only shape this sample has. `--play` is M13's windowed
            // client (m13.3); accepted here so the invocation does not change when it arrives.
        } else {
            std::fprintf(stderr, "usage: networked_player --headless [--cooked <dir>]\n");
            return 2;
        }
    }

    const std::vector<std::byte> file = read_file(std::filesystem::path(cooked_dir) / "wall.rdest");
    if (file.empty()) {
        std::fprintf(
            stderr, "cannot read %s/wall.rdest — run the cook fixture first\n", cooked_dir.c_str());
        return 1;
    }
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    if (!asset.has_value()) {
        const std::string_view reason = assets::to_string(err);
        std::fprintf(stderr,
                     "wall.rdest failed to decode: %.*s\n",
                     static_cast<int>(reason.size()),
                     reason.data());
        return 1;
    }

    Match match{0x12345678ull};
    match.stand_wall(*asset);
    ClientPeer& predicting = match.add_client(/*predict=*/true, 7802, 0x2222ull);
    ClientPeer& following = match.add_client(/*predict=*/false, 7803, 0x3333ull);

    // Settle the handshake, the spawns and the assignments before anything is measured.
    for (int i = 0;
         i < 300 && (match.driver->session_count() < 2 || !predicting.local_player().is_valid() ||
                     !following.local_player().is_valid());
         ++i) {
        match.tick();
    }

    // From here the tape's clock starts. Everything before this was handshake.
    match.tape_origin = match.tick_index;

    // ── The measurement: how long until my own input moves ME? ──
    // Both clients stand still until kStepOffTick, so the first tick on which each one's DRAWN
    // pose moves is the answer, in that client's own ticks. No clock synchronisation anywhere.
    int response[2] = {-1, -1};
    core::Vec3 at_rest[2];
    bool have_rest[2] = {false, false};

    const std::uint64_t start = match.tick_index;
    while (match.tick_index - start < kMatchTicks) {
        const std::uint64_t local = match.tick_index - start + 1;
        if (local == kStepOffTick) {
            for (int i = 0; i < 2; ++i) {
                at_rest[i] = match.clients[static_cast<std::size_t>(i)]->own_draw_position();
                have_rest[i] = true;
            }
        }
        match.tick();
        match.sample_frames();

        if (local >= kStepOffTick) {
            for (int i = 0; i < 2; ++i) {
                if (response[i] < 0 && have_rest[i]) {
                    const core::Vec3 now =
                        match.clients[static_cast<std::size_t>(i)]->own_draw_position();
                    if (core::length(now - at_rest[i]) > 1e-4f) {
                        response[i] = static_cast<int>(local - kStepOffTick + 1);
                    }
                }
            }
        }
    }

    // Quiesce: the tape stopped moving at kStopTick and is still SENDING, so the frontier can step
    // over anything permanently lost and both sides can come to rest on the same value.
    for (int i = 0; i < 200; ++i) {
        match.tick();
    }

    // ── The report ───────────────────────────────────────────────────────────────────────────
    int failures = 0;
    const auto check = [&failures](bool ok, const char* what) {
        std::fprintf(stderr, "  %s  %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };

    std::fprintf(stderr,
                 "\n13-networked-player — M12 \"The Player\"\n"
                 "  link: %.0f%% loss, %llu ms one-way (%d round-trip ticks), %llu ticks\n",
                 static_cast<double>(kLossRate * 100.0f),
                 static_cast<unsigned long long>(kOneWayMs),
                 kRoundTripTicks,
                 static_cast<unsigned long long>(match.tick_index));

    std::fprintf(stderr, "\nown-input response (the clause, and its control)\n");
    std::fprintf(stderr,
                 "        prediction ON  : %d tick(s)\n        prediction OFF : %d tick(s)\n",
                 response[0],
                 response[1]);
    check(response[0] > 0 && response[0] <= 1, "predicting client responds within 1 tick");
    check(response[1] >= kRoundTripTicks, "the control waits at least a round trip");
    check(response[0] < response[1], "prediction is provably the reason");

    std::fprintf(stderr, "\nagreement at quiescence (bit-exact, no epsilon)\n");
    for (std::size_t i = 0; i < match.clients.size(); ++i) {
        // WHICH server entity is this client's avatar is asked through the NetId, never through
        // spawn order. The two handshakes race on a lossy link, so `avatars[i]` is not reliably
        // client i's — and pairing by index compares one client's prediction against the OTHER
        // player's authoritative state, which fails for a reason that has nothing to do with
        // prediction. (It did, on the first run: both clients "failed to converge".)
        const replication::NetId id = match.clients[i]->gameplay.local_player_id();
        const ecs::Entity server_avatar = match.replicator->map().resolve(id);
        const gameplay::CharacterState mine = match.clients[i]->own_state();
        const auto* truth = server_avatar.is_valid()
                                ? match.world.get<gameplay::CharacterState>(server_avatar)
                                : nullptr;
        const bool same =
            truth != nullptr && mine.position.x == truth->position.x &&
            mine.position.y == truth->position.y && mine.position.z == truth->position.z &&
            mine.velocity.x == truth->velocity.x && mine.velocity.z == truth->velocity.z;
        std::fprintf(stderr,
                     "  %s  client %zu's own avatar (NetId %u) matches the server bit for bit\n",
                     same ? "ok  " : "FAIL",
                     i,
                     id.index);
        failures += same ? 0 : 1;
    }

    std::fprintf(stderr, "\nremote motion is continuous (m12.5)\n");
    // Asserted as a FRACTION of frames rather than as a hard maximum, and the reason is a real
    // mechanism rather than a tolerance for flakiness: a gap longer than `kMaxInterpolationSpan`
    // deliberately SNAPS (a blend stretched over a two-second stall would crawl the entity across
    // the level while the authority already has it elsewhere), and under 20% loss a run this long
    // will contain a few. So the claim is "motion is continuous", not "nothing ever snaps" — and
    // the snap count is printed beside it so a regression that starts snapping constantly is
    // visible rather than absorbed.
    const float bound = match.character.max_speed * kDt * 3.0f;
    for (std::size_t i = 0; i < match.clients.size(); ++i) {
        const ClientPeer& c = *match.clients[i];
        const bool ok = c.remote_frames > 0 && c.remote_frames_over_bound * 100 <= c.remote_frames;
        std::fprintf(stderr,
                     "  %s  client %zu: %llu of %llu drawn frames over %.3f m (worst %.4f, "
                     "far-gap snaps %llu)\n",
                     ok ? "ok  " : "FAIL",
                     i,
                     static_cast<unsigned long long>(c.remote_frames_over_bound),
                     static_cast<unsigned long long>(c.remote_frames),
                     static_cast<double>(bound),
                     static_cast<double>(c.worst_remote_step),
                     static_cast<unsigned long long>(c.replicator->histories_snapped_far()));
        failures += ok ? 0 : 1;
    }

    std::fprintf(stderr, "\nnon-vacuousness\n");
    check(match.network.packets_dropped() > 0, "the link really dropped packets");
    check(match.input.gaps_observed() > 0, "commands were really lost outright");
    check(predicting.predictor.corrections() > 0,
          "the predictor really corrected (zero would mean the comparison is dead)");
    check(predicting.predictor.corrections_skipped() > predicting.predictor.corrections(),
          "…and was usually right rather than usually replaced");
    check(match.gameplay.commands_consumed() > kMatchTicks,
          "the server consumed input from both players");
    check(match.gameplay.shots_fired() > 0 && match.gameplay.shots_hit() > 0,
          "shots were fired and connected");

    std::uint32_t dead = 0;
    const destruction::InstanceId instance{0, 0};
    const std::uint32_t parts = match.destruction.instance_part_count(instance);
    for (std::uint32_t p = 0; p < parts; ++p) {
        if (!match.destruction.part_alive(instance, p)) {
            ++dead;
        }
    }
    std::fprintf(stderr,
                 "  %s  the wall lost %u of %u parts to gunfire\n",
                 dead > 0 ? "ok  " : "FAIL",
                 dead,
                 parts);
    failures += dead > 0 ? 0 : 1;

    check(match.replicator->net_ids_orphaned() == 0, "no NetId was orphaned");
    check(predicting.replicator->malformed_messages() == 0 &&
              following.replicator->malformed_messages() == 0,
          "no client silently discarded a message as malformed");

    std::fprintf(
        stderr,
        "\ninstruments\n"
        "  corrections %llu (skipped %llu, replayed %llu, worst error %.3f m)\n"
        "  shots %llu fired / %llu hit · commands consumed %llu\n"
        "  starved player-ticks %llu — a lost packet costs one tick's command and the next\n"
        "    packet's redundancy window delivers two, so this tracks the loss rate\n",
        static_cast<unsigned long long>(predicting.predictor.corrections()),
        static_cast<unsigned long long>(predicting.predictor.corrections_skipped()),
        static_cast<unsigned long long>(predicting.predictor.commands_replayed()),
        static_cast<double>(predicting.predictor.max_correction_distance()),
        static_cast<unsigned long long>(match.gameplay.shots_fired()),
        static_cast<unsigned long long>(match.gameplay.shots_hit()),
        static_cast<unsigned long long>(match.gameplay.commands_consumed()),
        static_cast<unsigned long long>(match.gameplay.ticks_starved()));

    std::fprintf(stderr, "\n%s\n", failures == 0 ? "M12 green." : "M12 NOT green.");
    return failures == 0 ? 0 : 1;
}
