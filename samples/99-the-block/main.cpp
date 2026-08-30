// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 99-the-block — Milestone 13's "done when", and the vision demo itself:
//
//     a destructible urban block (M8 + M10 + M11 + M12) runs at a playable frame rate
//     and *feels* right.
//
// Every piece of that sentence has been built and proved on its own. NOT ONE OF THEM HAS EVER RUN
// WITH THE OTHERS. That is this sample's entire job: it is the first process in the repo's history
// to hold a cooked destructible city block, the full M10 lighting stack, a replicated server, a
// predicted client, a first-person camera, a window and a mixer at the same time. The interesting
// failures in a milestone like this are never in the parts — they are in the seams, and a seam is
// only visible when both sides of it are running.
//
// ── What this asserts, and how "feels right" is decomposed ──────────────────────────────────────
//
// "Feels right" is not testable and this sample does not pretend otherwise. It is split into the
// things that ARE:
//
//   1. IT COMPOSES. Eight subsystems in one frame, each still doing real work — asserted with each
//      one's own counter, not with "it didn't crash". A subsystem that silently switched itself off
//      is the failure this catches, and it is the likeliest one.
//   2. IT IS DESTRUCTIBLE AT SCALE. A demolition charge on a hero building drops live parts, raises
//      live debris past ADR-0035 §1's floor, and the pile then settles — and the peers converge.
//   3. THE PEERS AGREE. The client's destruction state hash converges on the server's, bit for bit,
//      with the composition checks matching and nothing left unresolved.
//   4. THE PLAYER IS IN IT. The camera is mounted on the m12.4 PREDICTED player — the wiring
//      m13.3a named and deferred to exactly here — and walks on the block's own collision.
//   5. IT DRAWS. The frustum cull does real work in both directions at block scale, every part has
//   a
//      render leaf that found its mesh, and the frame comes back lit. (That the block's pixels beat
//      an empty frame's is `block_render_test`'s comparative claim and is not repeated here.)
//
//   6. A PLAYABLE FRAME RATE is the one clause CI cannot judge, and it is NOT measured here.
//      Lavapipe is a CPU rasteriser, so a millisecond there is a statement about the runner's mood.
//      Counts stay in --headless where CI can fail on them forever; the clock belongs in a --perf
//      mode on hardware whose fingerprint goes into the report — the 11-lit-rooms split, ADR-0035
//      §2b — and that is m13.p, the next brick but one.
//
//   7. THE COLLAPSE STAYS LOCAL. Levelling one building must not level the block. This shipped as a
//      reported KNOWN DEFECT in m13.5 and is an assertion as of m13.6 — see the comment on it in
//      run_headless for what was wrong and why nothing else could have seen it.
//
// ── What it deliberately does NOT prove ─────────────────────────────────────────────────────────
//
// ONE CLIENT, not two. `13-networked-player` proves prediction with its prediction-off control as a
// peer in the same match, and `12-networked-destruction` proves two clients agreeing on a broken
// wall. Re-running those here would double the cost to re-answer settled questions. What is new
// here is that they compose AT BLOCK SCALE, so that is what is measured.
//
// GI MECHANISM is `11-lit-rooms`' and `tests/render/gi_thesis_test.cpp`'s. This sample asserts the
// lighting stack is on and responding — the caches invalidate when the block breaks — not that
// sphere tracing is correct.
//
// Run it:
//   build/dev/bin/the_block --headless [--ticks N]   the CI-gated proof
//   build/dev/bin/the_block --play                   walk around it and shoot
//   build/dev/bin/the_block --headless --idle        the control: nobody touches the block
//   [--cooked <dir>]                                 where the nine .rdest files are
//
// NO --perf YET. ADR-0035 asks for one and it belongs here; it is m13.p's, deliberately, because
// the scope of a perf pass should come from a demo that exists rather than from a guess about one.
// This sample is that demo.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/audio/mixer.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_net/composition.hpp"
#include "rime/destruction_net/destruction_client.hpp"
#include "rime/destruction_net/destruction_server.hpp"
#include "rime/destruction_render/part_leaves.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/components.hpp"
#include "rime/gameplay/first_person.hpp"
#include "rime/gameplay/input_map.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/gameplay_net/convert.hpp"
#include "rime/gameplay_net/gameplay_client.hpp"
#include "rime/gameplay_net/gameplay_server.hpp"
#include "rime/gameplay_net/predictor.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/render/text/hud.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/server_replicator.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/worldkit/profile.hpp"

#ifndef RIME_BLOCK_COOKED_DIR
#define RIME_BLOCK_COOKED_DIR "cooked"
#endif

namespace {

using namespace rime;

// ── The scenario ────────────────────────────────────────────────────────────────────────────────

constexpr float kDt = 1.0f / 60.0f;
constexpr std::uint64_t kTickMs = 16;
constexpr std::uint32_t kOneWayMs = 40; // ~80 ms RTT — a real internet match, not a LAN
constexpr float kLossRate = 0.05f;

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720;

// The tape's beats, in ticks after the handshake settles. The player walks up the street, turns to
// face the south hero building, and opens fire; the last stretch is silence so the pile can settle
// and the peers can converge with nothing still in flight.
// The tape is deliberately TIGHT, and the cost is why. Every tick after the collapse simulates
// ~1,500 debris bodies, so the tail dominates the run: the first draft's 1,100 ticks took 226 s in
// a Debug build on a real GPU, which under ASan and on lavapipe is a CI timeout rather than a test.
// These beats keep every claim reachable — the charge fires, the debris peaks past ADR-0035 §1's
// floor, and the pile gets long enough to settle and the peers to converge — in a fraction of the
// work. `--ticks N` runs it longer by hand.
constexpr std::uint64_t kWalkFrom = 30;
constexpr std::uint64_t kAimAt = 90;
constexpr std::uint64_t kFireFrom = 100;
constexpr std::uint64_t kChargeAt = 210;
constexpr std::uint64_t kCeaseFire = 200;
constexpr std::uint64_t kDefaultTicks = 430;

// ADR-0035 §1's floors, restated here as the numbers this proof fails on.
constexpr std::size_t kMinParts = 1500;
constexpr std::size_t kMinLocalLights = 32;
constexpr std::size_t kMinPeakDebris = 400;

// How many queued destruction batches a client may apply in one tick. Each costs a full physics
// step, so this is a frame-time bound, not a fidelity knob — see the note at the drain loop. Two
// lets a client that fell one behind recover immediately without ever paying for ten at once.
constexpr std::size_t kMaxCatchUpBatchesPerTick = 2;

// Set by --idle. A file-scope flag rather than a parameter because the tape is a pure function of
// tick by contract and threading a mode through it would invite reading anything else in too.
bool g_idle = false;

// How many ticks the script runs. A variable rather than a constant so a human can run the demo
// past the point CI stops caring; the default is chosen for CI's clock (see the beats above).
std::uint64_t g_ticks = kDefaultTicks;

// --ppm <file>: write the intact frame the render claims are made about, so a human can look at the
// thing the counters are describing.
std::string g_ppm;

// ── Small helpers ───────────────────────────────────────────────────────────────────────────────

// Every component this demo's worlds hold: the engine's profile (m14.1) plus the game's own.
//
// It used to be nine hand-written lines here, and the first draft got them wrong in a way that took
// a while to see: it named the four modules the block's SCENE needs and none of the four its
// SESSION needs, so the block stood up and drew perfectly while the predictor never seeded (no
// `RigidBodyHandle` meant no body to predict against), destruction replicated zero ops, the peers'
// hashes disagreed, and the mixer heard nothing. Four symptoms, one missing list.
//
// `worldkit::register_engine_components` is that list, once, shared with the editor host and
// anything else that opens a `.rscene`. The order inside it is fixed, which matters beyond
// tidiness: `ecs::component_schema_hash` goes into `NetDriver::Config`, and two peers that
// registered different sets refuse to connect. Both peers calling one function cannot disagree.
void register_all(ecs::World& world) {
    (void)worldkit::register_engine_components(world);
    blockkit::register_blockkit_components(world); // the game's own, layered on the engine's
}

// How many debris bodies are still LIVE (not frozen by the C6 budget). The roster is append-only,
// so a frozen debris keeps its slot and reads a dead body — block_standup_test's helper.
[[nodiscard]] std::size_t live_debris(const destruction::DestructionWorld& dw,
                                      physics::PhysicsWorld& pw) {
    std::size_t live = 0;
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        if (pw.is_alive(dw.debris_body(i))) {
            ++live;
        }
    }
    return live;
}

// What is ACTUALLY standing in a world, counted from the world.
//
// `blockkit::BlockStats` describes what `assemble` intended to build, and for a generated block the
// two agree. With `--scene` they need not: the whole point of an editor is that the file differs
// from what the generator would have produced, so every number the proof gates on is measured from
// the world that came off disk. The same discipline m13.2c applied to the block's own scale floors.
struct MeasuredScene {
    std::size_t entities = 0;
    std::size_t destructibles = 0;
    std::size_t parts = 0; // only knowable after binding — the patterns carry the part counts
    std::size_t local_lights = 0;

    // A fingerprint of the AUTHORED placements — every entity's LocalTransform translation, FNV-1a
    // over the raw float bits in load order.
    //
    // It exists so the round trip can be OBSERVED rather than assumed (m14.4). "The game ran the
    // file the editor saved" is worth very little on its own: it is equally true of a game that
    // silently regenerated its own level and ignored the file. Two runs whose digests DIFFER are
    // running different scenes, and that is the claim M14's "done when" actually makes.
    std::uint64_t placement_digest = 0;
};

[[nodiscard]] MeasuredScene measure_scene(ecs::World& world) {
    MeasuredScene m;
    m.entities = world.entity_count();
    world.query<destruction::Destructible>().for_each(
        [&](ecs::Entity, destruction::Destructible&) { ++m.destructibles; });
    world.query<render::PointLight>().for_each(
        [&](ecs::Entity, render::PointLight&) { ++m.local_lights; });
    world.query<render::SpotLight>().for_each(
        [&](ecs::Entity, render::SpotLight&) { ++m.local_lights; });

    // FNV-1a over the translation bits. Bits, not values: an edit of 22 -> 23 must change this, and
    // so must an edit far below any epsilon a comparison would pick.
    std::uint64_t h = 1469598103934665603ull;
    const auto fold = [&h](float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<std::uint64_t>((bits >> (i * 8)) & 0xFFu);
            h *= 1099511628211ull;
        }
    };
    world.query<ecs::LocalTransform>().for_each([&](ecs::Entity, ecs::LocalTransform& lt) {
        fold(lt.value.translation.x);
        fold(lt.value.translation.y);
        fold(lt.value.translation.z);
    });
    m.placement_digest = h;
    return m;
}

// The ground the block stands on and the player walks along. It is NOT in the scene file: blockkit
// authors the block, and a street slab is level geometry every peer stands up for itself — the same
// split `13-networked-player` makes with its floor, and the reason a client can predict standing on
// something without waiting for the server to tell it the floor exists.
physics::BodyId add_street(physics::PhysicsWorld& w, const blockkit::BlockParams& p) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape.type = physics::ShapeType::Box;
    d.shape.half_extents = {p.street_length(), 0.5f, p.street_length()};
    d.position = {p.street_length() * 0.5f, -0.5f, 0.0f};
    return w.create_body(d);
}

// Pull an RGBA8 texture back to the CPU (the 06/07 samples' helper). Only the headless self-check
// uses it — a windowed run has no reason to stall the pipeline reading its own frame.
[[nodiscard]] std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// Write an RGBA8 readback as a binary PPM — the same one-screen helper every other sample carries.
// `--ppm` exists because a demo nobody has looked at is a demo nobody has checked: every claim in
// this file is a counter, and counters cannot see a scene that is correct-but-wrong (m13.3b found
// two such bugs by looking).
void write_ppm(const char* path,
               const std::vector<std::uint8_t>& px,
               std::uint32_t w,
               std::uint32_t h) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "99-the-block: cannot write %s\n", path);
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        (void)std::fwrite(&px[i * 4], 1, 3, f);
    }
    (void)std::fclose(f);
    std::printf("  wrote %s\n", path);
}

[[nodiscard]] double mean_luma(const std::vector<std::uint8_t>& rgba) {
    double sum = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
    }
    return rgba.empty() ? 0.0 : sum / static_cast<double>(rgba.size() / 4);
}

// ── The block, as content every peer loads for itself ───────────────────────────────────────────
//
// Nothing is shared between peers — not the world, not the physics, not the patterns. That is the
// whole reason `Destructible` names a CONTENT id rather than an InstanceId (m13.2c): two peers must
// arrive at their own local indices independently, exactly as two machines would.
struct Peer {
    ecs::World world;
    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    core::JobSystem jobs{0};
    destruction::DestructionWorld destruction;
    std::unordered_map<std::uint64_t, destruction::PatternId> patterns;
    MeasuredScene measured;

    [[nodiscard]] bool register_patterns(const std::filesystem::path& cooked,
                                         destruction_render::PartLeafRenderer* leaves = nullptr,
                                         render::MeshRegistry* meshes = nullptr) {
        for (const blockkit::CookSpec& spec : blockkit::cook_specs()) {
            const auto bytes = platform::read_file(cooked / (std::string(spec.name) + ".rdest"));
            if (!bytes) {
                std::fprintf(stderr,
                             "99-the-block: missing cook '%s' under %s\n",
                             std::string(spec.name).c_str(),
                             cooked.string().c_str());
                return false;
            }
            assets::AssetError err{};
            auto asset = assets::read_destructible(*bytes, err);
            if (!asset) {
                std::fprintf(stderr,
                             "99-the-block: undecodable cook '%s'\n",
                             std::string(spec.name).c_str());
                return false;
            }
            const destruction::PatternId id = destruction.register_pattern(*asset, physics);
            if (!id.is_valid()) {
                std::fprintf(stderr,
                             "99-the-block: physics rejected cook '%s'\n",
                             std::string(spec.name).c_str());
                return false;
            }
            patterns[spec.asset] = id;
            // The leaf meshes are the RENDER half and only the drawing peer wants them. Passing a
            // null registry is a designed seam (m13.2d): minting a MeshId needs a GPU, the leaf
            // life cycle does not, and a GPU-free peer must not be made to care.
            if (leaves != nullptr) {
                (void)leaves->register_pattern(id, *asset, meshes);
            }
        }
        return true;
    }

    // Load the block from the SCENE FILE, exactly as a game would: placement plus a reflected
    // SlabRole, with the look derived at load. Never MeshRef/MaterialRef — those are dense indices
    // into runtime registries, and authoring them would silently mis-shade the day the palette
    // gains an entry at the front (m13.2c). `own_destructibles` decides whether this peer's copy of
    // the block keeps the cooked slabs and crates it just loaded, and the split is the architecture
    // rather than an option.
    //
    // A SERVER OWNS THE BLOCK. It loads everything and replicates the destructibles like any other
    // entity — destruction_server.hpp is explicit that it does NOT replicate them itself:
    // "structure travels as ECS state, and only the damage transitions need a channel of their
    // own".
    //
    // A CLIENT LOADS THE LEVEL AND IS SENT THE BLOCK. It keeps the street furniture, the lamps and
    // the lights — level geometry every peer stands up for itself — and DROPS its own copies of the
    // destructibles, because the server is about to send them. Keeping both would give the client
    // 280 destructible instances: 140 mirrored ones receiving damage and 140 local ones standing
    // there forever, which draws as a building that never falls behind the one that does.
    [[nodiscard]] bool stand_up(destruction::Authority authority,
                                bool own_destructibles,
                                std::string_view scene_path) {
        register_all(world);
        (void)add_street(physics, blockkit::BlockParams{});

        // `--scene <file>` runs A SCENE SOMEONE AUTHORED rather than the one blockgen produces —
        // M14's "done when": open the shipped block in the editor, change it, save it, and run the
        // changed scene here. Without it the demo generates its own, exactly as before.
        //
        // STRICT load either way. A tool opens a scene leniently because it may not have every
        // module; a GAME that cannot construct part of its own level is broken and should say so at
        // the door (scene::LoadOptions).
        if (!scene_path.empty()) {
            const scene::LoadReport r =
                scene::load_scene_file(world, std::filesystem::path(scene_path));
            if (!r.ok) {
                std::fprintf(stderr,
                             "99-the-block: could not load '%s': %s\n",
                             std::string(scene_path).c_str(),
                             r.error.c_str());
                return false;
            }
        } else {
            ecs::World authoring;
            (void)blockkit::assemble(authoring, blockkit::BlockParams{});
            if (!scene::load_scene_from_string(world, scene::save_scene_to_string(authoring)).ok) {
                std::fprintf(stderr,
                             "99-the-block: the block's own scene file did not load back\n");
                return false;
            }
        }
        (void)blockkit::derive_world_transforms(world);
        measured = measure_scene(world);

        // C6's budget, at block scale. 10-destructible-wall runs 48 live; this needs an order of
        // magnitude more. Both caps rise together and the visual one stays LARGER — inverting them
        // would let a still-simulating debris be visually retired, which destruction/world.hpp
        // makes a contract rather than a preference.
        destruction::LifecycleConfig life;
        life.enabled = true; // defaults FALSE, and missing it makes every budget assertion vacuous
        life.max_live_debris = 512;
        life.max_visual_debris = 1024;
        destruction.configure_lifecycle(life);

        if (!own_destructibles) {
            // Drop the locally-loaded destructibles; the server's copies are on their way.
            // Collected first and despawned after, because despawning inside a query mutates what
            // it is walking.
            std::vector<ecs::Entity> local;
            world.query<destruction::Destructible>().for_each(
                [&](ecs::Entity e, destruction::Destructible&) { local.push_back(e); });
            for (const ecs::Entity e : local) {
                world.despawn(e);
            }
            return true; // binding happens per tick as replication delivers them
        }

        const destruction::BindStats bound = bind(authority);
        if (bound.unresolved != 0 || bound.bound != measured.destructibles) {
            std::fprintf(stderr,
                         "99-the-block: bound %zu of %zu destructibles, %zu unresolved\n",
                         bound.bound,
                         measured.destructibles,
                         bound.unresolved);
            return false;
        }
        // Parts are only knowable now: the count lives in the cooked patterns the bind resolved.
        for (std::size_t i = 0; i < destruction.instance_count(); ++i) {
            measured.parts += destruction.instance_part_count(
                destruction::InstanceId{static_cast<std::uint32_t>(i), 0});
        }
        return true;
    }

    // Idempotent by construction (bind.hpp): an entity with a live `DestructibleInstanceRef` is
    // skipped, so a client can call this every tick and pay one query for it. That is exactly what
    // a client must do — a mirror that arrived this tick has to be standing before anything damages
    // it, which is why bind.hpp says to call it from PreSim after replication has applied spawns.
    destruction::BindStats bind(destruction::Authority authority) {
        return destruction::bind_destructibles(
            world,
            destruction,
            physics,
            [this](std::uint64_t asset) {
                const auto it = patterns.find(asset);
                return it == patterns.end() ? destruction::PatternId{} : it->second;
            },
            authority);
    }
};

// ── The two peers, and the wire between them ────────────────────────────────────────────────────

constexpr std::uint64_t kAppId = 0x52494D45u;

struct ServerPeer : Peer {
    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::unique_ptr<replication::ServerReplicator> replicator;
    replication::ServerInputReceiver input;
    gameplay_net::GameplayServer gameplay;
    destruction_net::DestructionServer destruction_net_server;

    gameplay::CharacterConfig character{};
    gameplay::WeaponConfig weapon{};
    std::vector<ecs::Entity> avatars;

    // The game's spawn policy stays in the game, not the engine — ADR-0035 §3. Which prefab, which
    // spawn point, which CharacterConfig and whether to replicate at all are all decisions
    // `GameplayServer` deliberately refuses to make on a game's behalf.
    ecs::Entity spawn_avatar(net::SessionId) {
        core::Transform placement;
        placement.translation = blockkit::west_viewpoint(blockkit::BlockParams{});
        placement.translation.y = character.half_height + character.radius;

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
};

struct ClientPeer : Peer {
    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::unique_ptr<replication::ClientReplicator> replicator;
    replication::ClientInputSender sender;
    gameplay_net::GameplayClient gameplay;
    gameplay_net::Predictor predictor;
    destruction_net::DestructionClient destruction_net_client;
    gameplay::CharacterConfig config{};

    // The view is the CLIENT's and only the client's. Looking around changes nothing the server
    // arbitrates (first_person.hpp), so yaw/pitch accumulate here from devices and ride the input
    // tape — never through CharacterState, which m12.4's replay would rewind and snap.
    gameplay::FirstPersonView view{};
    platform::Input devices;

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

    // WHERE THE CAMERA GOES, and the line m13.3a named and deferred to this sample: the eye rides
    // the PREDICTED player's smoothed pose, not the last snapshot the server sent. Drawing the
    // snapshot instead is what makes a networked game feel like walking through treacle — every
    // step you take arrives a round trip later.
    [[nodiscard]] core::Vec3 eye_anchor() const {
        if (predictor.seeded()) {
            return predictor.visual_position();
        }
        const ecs::Entity e = local_player();
        const auto* s = e.is_valid() ? world.get<gameplay::CharacterState>(e) : nullptr;
        return s != nullptr ? s->position : core::Vec3{};
    }

    [[nodiscard]] core::Transform camera_transform() const {
        gameplay::FirstPersonView v = view;
        v.eye_height =
            config.half_height * 0.8f; // eyes near the top of the capsule, not its centre
        return gameplay::eye_transform(v, eye_anchor());
    }
};

// One process, both peers, a lossy wire between them. In-process and deterministic
// (ScriptedNetwork) rather than real sockets, because the demo's claim is about COMPOSITION at
// scale and a real socket would add a nondeterministic variable that says nothing about it. The
// latency and loss are real numbers though — 80 ms RTT and 5% loss — so the prediction path is
// genuinely exercised rather than bypassed by a zero-latency link.
struct Session {
    net::ScriptedNetwork network;
    net::Endpoint server_endpoint{0x7F000001u, 7901};
    ServerPeer server;
    ClientPeer client;

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::uint64_t tape_origin = 0;

    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    // Running maxima the proof reads. Peaks, not final values: a budget is about the worst moment.
    std::size_t peak_live_debris = 0;
    std::size_t peak_visual_debris = 0;
    destruction::BindStats client_bound{};

    // The two halves of a tick, timed apart (m13.p). This demo is a server AND a client in one
    // process, so its combined simulation cost is roughly what TWO machines pay — and gating a
    // player-facing frame budget on that number would be measuring the demo's topology rather than
    // the engine. A player's machine runs the client half.
    double last_server_ms = 0.0;
    double last_client_ms = 0.0;

    // How many destruction batches the client drained in its worst single tick, and how many
    // physics steps that cost it in total. The client's catch-up loop runs a FULL physics step per
    // queued batch (12-networked-destruction's "one batch per update, never two merged" rule, which
    // is right for fidelity) — so if it ever falls behind during a collapse, the cost of catching
    // up is serialised into one frame with no bound on it. This is the counter that says whether
    // that is happening or whether the spike is something else.
    std::size_t max_batches_per_tick = 0;
    std::uint64_t total_client_steps = 0;
    std::uint64_t shots_fired = 0;
    std::uint64_t shots_hit = 0;
    std::uint64_t damage_ops = 0;
    float max_op_amount = 0.0f;   // largest single damage op; parts stand at 1.0 health
    std::uint64_t lethal_ops = 0; // ops that killed their part outright
    std::uint64_t total_ops = 0;

    explicit Session(std::uint64_t seed) : network(seed, {kLossRate, 0.0f, kOneWayMs, kOneWayMs}) {}

    [[nodiscard]] bool start(const std::filesystem::path& cooked,
                             std::string_view scene_path,
                             destruction_render::PartLeafRenderer* leaves,
                             render::MeshRegistry* meshes) {
        // A rifle, not a demolition charge: cooked parts stand at 1.0 health, so the damage number
        // is expressed against that scale and a building takes sustained fire rather than one shot.
        server.weapon.damage = 0.34f;
        server.weapon.damage_radius = 0.35f;
        server.weapon.cooldown_ticks = 3;

        // The server is GPU-free by construction — it never draws, so it never registers leaf
        // meshes. Only the client does, and that asymmetry is the point of m13.2d's null-registry
        // seam rather than an accident of this sample.
        if (!server.register_patterns(cooked) ||
            !client.register_patterns(cooked, leaves, meshes)) {
            return false;
        }
        // A server binds Local (it owns them, and every damage source feeds them); a client binds
        // Remote, which is what stops its own solver's contact impulses from eroding a wall the
        // server is already eroding for it.
        if (!server.stand_up(destruction::Authority::Local, true, scene_path) ||
            !client.stand_up(destruction::Authority::Remote, false, scene_path)) {
            return false;
        }

        server.link = &network.add_node(server_endpoint);
        net::NetDriver::Config sc;
        sc.app_id = kAppId;
        sc.schema_hash = ecs::component_schema_hash(server.world);
        sc.salt_seed = 0x9001ull;
        server.driver = std::make_unique<net::NetDriver>(*server.link, sc);
        server.driver->listen();
        server.replicator = std::make_unique<replication::ServerReplicator>(server.world);

        const net::Endpoint client_endpoint{0x7F000001u, 7902};
        client.link = &network.add_node(client_endpoint);
        net::NetDriver::Config cc;
        cc.app_id = kAppId;
        cc.schema_hash = ecs::component_schema_hash(client.world);
        cc.salt_seed = 0x9002ull;
        client.driver = std::make_unique<net::NetDriver>(*client.link, cc);
        client.replicator = std::make_unique<replication::ClientReplicator>(client.world);
        (void)client.driver->connect(server_endpoint, now_ms);

        // Every destructible gets a NetId. Without this the damage ops have nothing to address: the
        // op list is canonical and correct, the packing has no name to put in it, and the client
        // sees a block that never breaks while the server's falls down — silently, with every other
        // counter green. It is the first thing to check if `ops_applied` is ever zero.
        //
        // COLLECT, THEN REPLICATE — never replicate inside the query. `ServerReplicator::replicate`
        // makes a STRUCTURAL change (the entity gains its replication components), which moves it
        // to another archetype and reallocates the chunk vector the query is walking. Doing it in
        // the loop body aborts on the next chunk index, which is exactly how the first version of
        // this crashed. The same rule governs the despawn loop in `stand_up`.
        std::vector<ecs::Entity> destructibles;
        server.world.query<destruction::Destructible>().for_each(
            [&](ecs::Entity e, destruction::Destructible&) { destructibles.push_back(e); });
        for (const ecs::Entity e : destructibles) {
            (void)server.replicator->replicate(e);
        }
        if (destructibles.size() != server.measured.destructibles) {
            std::fprintf(stderr,
                         "99-the-block: replicated %zu of %zu destructibles\n",
                         destructibles.size(),
                         server.measured.destructibles);
            return false;
        }
        return true;
    }

    // ── One tick of the whole system ────────────────────────────────────────────────────────────
    // The order is docs/design/simulation-tick.md's, and it is the order both `13-networked-player`
    // and `12-networked-destruction` established. Splicing them together is most of this sample's
    // risk, so the two halves are kept in their original sequence rather than interleaved cleverly.
    void tick(const replication::InputCommand& intent) {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        // ── PreSim: the server takes its mail ──
        events.clear();
        server.driver->update(now_ms, events);
        server.replicator->on_session_events(events);
        server.input.on_session_events(events);
        server.gameplay.on_session_events(
            events,
            [this](net::SessionId id) { return server.spawn_avatar(id); },
            [this](net::SessionId, ecs::Entity p) { server.replicator->despawn(p); });
        for (const net::SessionId id : server.driver->session_ids()) {
            net::Session* s = server.driver->session(id);
            if (s == nullptr) {
                continue;
            }
            inbox.clear();
            (void)s->drain_received(inbox);
            (void)server.replicator->apply_messages(id, inbox);
            (void)server.input.apply_messages(id, inbox);
        }

        // ── PreSim: the client sends this tick's intent, takes its mail, then predicts ──
        const replication::InputCommand command = client.sender.record(intent);
        client.sender.send(*client.driver, now_ms);

        events.clear();
        client.driver->update(now_ms, events);
        for (const net::SessionId id : client.driver->session_ids()) {
            net::Session* s = client.driver->session(id);
            if (s == nullptr) {
                continue;
            }
            inbox.clear();
            (void)s->drain_received(inbox);
            (void)client.replicator->apply_messages(inbox);
            client.sender.apply_messages(inbox);
            (void)client.gameplay.apply_messages(inbox);
            client.destruction_net_client.apply_messages(
                inbox, client.replicator->map(), client.world);
        }

        const core::Stopwatch client_watch;
        ecs::propagate_transforms(client.world, client.jobs);
        // Stand up whatever replication just delivered, BEFORE anything can damage it. Idempotent,
        // so this is one query on the overwhelming majority of ticks where nothing new arrived.
        client_bound = client.bind(destruction::Authority::Remote);
        client.sync.reconcile(client.world, client.physics);

        const ecs::Entity avatar = client.local_player();
        const physics::BodyId self = client.local_body();
        if (avatar.is_valid() && self.is_valid()) {
            if (const auto* cfg = client.world.get<gameplay::CharacterConfig>(avatar)) {
                client.config = *cfg;
            }
            if (const auto* auth = client.world.get<gameplay::CharacterState>(avatar)) {
                (void)client.predictor.reconcile(
                    *auth,
                    client.gameplay.last_processed_input(client.world, client.replicator->map()),
                    client.config,
                    client.physics,
                    self,
                    kDt);
            }
            (void)client.predictor.predict(
                command, client.config, client.physics, self, kDt, nullptr);
            if (client.predictor.seeded()) {
                gameplay::write_character_pose(
                    client.world, avatar, client.predictor.visual_position());
            }
        }
        client.sync.push_in(client.world, client.physics, kDt);

        // ONE queued batch per destruction update — never two merged, which would skip the fracture
        // boundary between them and make two waves of debris look like one island.
        // BOUNDED CATCH-UP (m13.p, and it is a measured fix rather than a precaution).
        //
        // 12-networked-destruction drains EVERY queued batch in one tick — "one batch per
        // destruction update, never two merged", which is right: merging skips the fracture
        // boundary between them and makes two waves of debris look like one island. But each batch
        // costs a full `physics.step`, and during a collapse the client falls behind, so the whole
        // cost of catching up serialises into one frame. Measured on this demo before the cap:
        // **10 physics steps in a single tick**, and `sim.client` p99 = 58 ms against a server that
        // never exceeded 9.5 ms doing the same work. The physics was never the problem.
        //
        // DEFERRING IS NOT MERGING. Batches stay queued and are still applied one at a time, in
        // order, with their fracture boundaries intact — the client simply spreads the catch-up
        // over several ticks and lags a little longer. That it still converges is not assumed: the
        // headless proof drains to quiescence with a BOUND and fails if the peers never agree.
        std::size_t batches_this_tick = 0;
        do {
            (void)client.destruction_net_client.apply_next_batch(
                client.world, client.replicator->map(), client.destruction);
            client.physics.step(kDt);
            client.destruction.update(client.physics);
            ++batches_this_tick;
        } while (client.destruction_net_client.pending_batches() > 0 &&
                 batches_this_tick < kMaxCatchUpBatchesPerTick);
        max_batches_per_tick = std::max(max_batches_per_tick, batches_this_tick);
        total_client_steps += batches_this_tick;
        client.sync.write_back(client.world, client.physics);
        last_client_ms = client_watch.elapsed_ms();

        // ── The server's simulation ──
        const core::Stopwatch server_watch;
        server.world.advance_version();
        server.gameplay.consume(server.world, server.physics, server.input, kDt);
        ecs::propagate_transforms(server.world, server.jobs);
        server.sync.reconcile(server.world, server.physics);
        server.sync.push_in(server.world, server.physics, kDt);
        server.physics.step(kDt);
        server.sync.write_back(server.world, server.physics);

        // The weapon → destruction glue: the consumer's job, kept out of the engine so that
        // `gameplay_net` never links `destruction` (ADR-0035 §3).
        for (const gameplay_net::ShotEvent& shot : server.gameplay.shots()) {
            ++shots_fired;
            if (!shot.did_hit) {
                continue; // a miss is still an event — a tracer and a report, no damage
            }
            ++shots_hit;
            destruction::InstanceId instance{};
            for (std::size_t i = 0; i < server.destruction.instance_count(); ++i) {
                const destruction::InstanceId candidate{static_cast<std::uint32_t>(i), 0};
                if (server.destruction.body_of(candidate) == shot.body) {
                    instance = candidate;
                    break;
                }
            }
            if (!instance.is_valid() || server.destruction.part_from_child(instance, shot.child) ==
                                            destruction::kInvalidPartIndex) {
                continue; // the street, or rubble — not a standing destructible
            }
            server.destruction.apply_damage(
                instance, shot.point, shot.damage_radius, shot.damage, shot.impulse);
            ++damage_ops;
        }
        server.destruction.update(server.physics);
        last_server_ms = server_watch.elapsed_ms();
        // The damage-op shape, and it is not a curiosity — it is the number that made m13.5's
        // runaway collapse legible. Parts stand at 1.0 health, so an op at or above 1.0 kills
        // outright; a population that is almost entirely instant kills means the damage tuning is
        // wrong for the content's scale, whatever the collapse looks like. It read 729 of 799 with
        // the fracturer's default threshold and reads a small minority now.
        for (const destruction::DamageOp& op : server.destruction.committed_ops()) {
            max_op_amount = std::max(max_op_amount, op.amount);
            if (op.amount >= 1.0f) {
                ++lethal_ops;
            }
            ++total_ops;
        }

        // ── PostSim: the debris bridge ──
        // Every new chunk becomes a replicated entity, live chunks get this tick's transform, and
        // reclaimed ones are retracted. AFTER the destruction update that made them and BEFORE
        // publish, so they carry this tick's pose. Omitting it is silent: the block still breaks
        // and the peers still agree, because composition is DERIVED — but no rubble is ever
        // replicated.
        server.destruction_net_server.sync_debris(
            server.world,
            server.destruction,
            server.physics,
            server.replicator->map(),
            [this](ecs::Entity e) { (void)server.replicator->replicate(e); },
            [this](ecs::Entity e) { server.replicator->despawn(e); });

        // ── Publish ──
        server.destruction_net_server.publish(*server.driver,
                                              server.replicator->map(),
                                              server.world,
                                              server.destruction,
                                              tick_index,
                                              now_ms);
        server.replicator->publish(*server.driver, now_ms);
        server.gameplay.publish(*server.driver, server.replicator->map(), now_ms);
        server.input.send_acks(*server.driver, now_ms);
        client.replicator->send_ack(*client.driver, now_ms);
        (void)client.replicator->settle_transform_history();

        peak_live_debris =
            std::max(peak_live_debris, live_debris(server.destruction, server.physics));
        peak_visual_debris = std::max(peak_visual_debris, server.destruction.visual_debris_count());
    }

    // Parts still standing, per building. A single total cannot tell a LOCAL collapse from a global
    // one, and those are completely different facts about the content: one is the demo working, the
    // other is a block that cannot survive being touched.
    [[nodiscard]] std::vector<std::size_t> parts_per_building() {
        const blockkit::BlockParams p;
        std::vector<std::size_t> per(p.building_count() + 1, 0); // last slot: crates and street
        server.world.query<blockkit::SlabRole, destruction::DestructibleInstanceRef>().for_each(
            [&](blockkit::SlabRole& role, destruction::DestructibleInstanceRef& ref) {
                const destruction::InstanceId id{ref.instance};
                const std::size_t slot =
                    role.building < p.building_count() ? role.building : p.building_count();
                const std::uint32_t n = server.destruction.instance_part_count(id);
                for (std::uint32_t i = 0; i < n; ++i) {
                    if (server.destruction.part_alive(id, i)) {
                        ++per[slot];
                    }
                }
            });
        return per;
    }

    // How many of the block's parts are still standing on the server.
    [[nodiscard]] std::size_t parts_alive() const {
        std::size_t alive = 0;
        for (std::size_t i = 0; i < server.destruction.instance_count(); ++i) {
            const destruction::InstanceId id{static_cast<std::uint32_t>(i), 0};
            const std::uint32_t n = server.destruction.instance_part_count(id);
            for (std::uint32_t p = 0; p < n; ++p) {
                if (server.destruction.part_alive(id, p)) {
                    ++alive;
                }
            }
        }
        return alive;
    }
};

// ── The scripted tape ───────────────────────────────────────────────────────────────────────────
//
// Input is DATA — a pure function of tick — never anything read back out of the simulation. A tape
// that aimed by querying the world would re-introduce exactly the float dependence that makes a run
// unreproducible (ADR-0033 A6).
//
// AIMING WITHOUT LOOKING is the trick that makes that possible here. The player walks STRAIGHT AT
// the point it is shooting at, on the constant heading from its own spawn to the south hero
// building's front face, both of which are authored constants in `BlockParams`. Because the walk
// and the aim are the same ray, the shot stays on target however far along it the character
// actually got — so the tape never needs to know where the player ended up.

// Where the south hero building's street-facing wall is. `building_frame` in
// blockkit/src/block.cpp: the street runs +X, south buildings sit at -Z and face +Z.
[[nodiscard]] core::Vec3 hero_face(const blockkit::BlockParams& p) noexcept {
    const float x =
        p.footprint * 0.5f + static_cast<float>(p.hero_south) * (p.footprint + p.building_gap);
    const float z = -(p.street_width + p.footprint) * 0.5f + p.footprint * 0.5f;
    return {x, blockkit::kEyeHeight, z - 0.2f};
}

// The yaw whose forward points from `from` to `to`. Engine convention: forward is (−sin y, 0, −cos
// y), so y = atan2(−dx, −dz). Kept as one expression rather than a lookup so the reader can check
// it against `first_person.hpp`'s basis directly.
[[nodiscard]] float yaw_towards(core::Vec3 from, core::Vec3 to) noexcept {
    return std::atan2(-(to.x - from.x), -(to.z - from.z));
}

[[nodiscard]] replication::InputCommand scripted_tape(std::uint64_t tick) {
    // `--idle` returns an empty tape: the block, standing, with a player who does nothing. It is a
    // CONTROL, and it is how the cascade below was pinned down — with it the block is untouched for
    // 550 ticks and then only the charge acts, so "the player broke it" and "it fell over by
    // itself" stop being the same observation.
    if (g_idle) {
        return replication::InputCommand{};
    }
    const blockkit::BlockParams p;
    const float aim = yaw_towards(blockkit::west_viewpoint(p), hero_face(p));

    replication::InputCommand c;
    c.yaw = aim; // held from the first tick: turning and walking are the same ray
    if (tick < kWalkFrom) {
        return c; // standing still — and STILL SENDING, because the frontier only steps over a
                  // permanently-lost command when a later one arrives.
    }
    if (tick < kAimAt) {
        c.move_y = 1.0f; // walk down the aim ray, closing on the hero building
        return c;
    }
    if (tick >= kFireFrom && tick < kCeaseFire) {
        // Semi-auto, on a period the cooldown does not divide, so the weapon's own gate is what
        // spaces the shots rather than the tape pretending to know it.
        if ((tick - kFireFrom) % 7 == 0) {
            c.pressed |= gameplay::kActionFire;
            c.held |= gameplay::kActionFire;
        }
    }
    return c;
}

// ── The SDF field the DDGI probes trace (m13.L) ──────────────────────────────────────────────────
//
// Without this, `ddgi_enabled` is a lie that reports work: the probes update, trace an EMPTY field,
// and blend in nothing. That is exactly what the first m13.L run measured — 384 probes updated,
// 24,576 rays traced, and `sdf.stamps == 0`. The claim caught it; the flag never would have.
//
// ONE BOX PER BUILDING, NOT ONE PER SLAB. The block has 140 destructible slabs, and a per-slab
// field would be 140 instances recomposed as the street falls down. A building envelope is what a
// bounce actually cares about — the mass that occludes the sun and reflects the street lamps — and
// it is eight instances. The cost is resolution: a doorway is not a hole in the field, and a
// partial collapse does not thin it. Both are honest limits of an envelope, recorded rather than
// hidden, and the finer field is the kind of thing m10.4b's per-part note anticipates.
[[nodiscard]] float box_sdf_distance(core::Vec3 p, core::Vec3 half) noexcept {
    const core::Vec3 q{std::fabs(p.x) - half.x, std::fabs(p.y) - half.y, std::fabs(p.z) - half.z};
    const core::Vec3 m{std::fmax(q.x, 0.0f), std::fmax(q.y, 0.0f), std::fmax(q.z, 0.0f)};
    return core::length(m) + std::fmin(std::fmax(q.x, std::fmax(q.y, q.z)), 0.0f);
}

// An analytic box baked into a voxel grid — the same construction 11-lit-rooms uses for its walls
// (`build_box_sdf`), kept local rather than shared because it is four lines of maths and a sample
// helper is the wrong thing to promote into the engine without a second caller.
[[nodiscard]] assets::MeshSdfAsset build_box_sdf(core::Vec3 half, std::uint32_t target_res = 16) {
    const float longest = std::fmax(half.x, std::fmax(half.y, half.z)) * 2.0f;
    const float voxel = longest / static_cast<float>(target_res);
    const float pad = 2.0f * voxel;
    const float h[3] = {half.x, half.y, half.z};
    assets::MeshSdfAsset sdf;
    std::uint32_t res[3]{};
    float origin[3]{};
    for (int a = 0; a < 3; ++a) {
        res[a] = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil((2.0f * h[a] + 2.0f * pad) / voxel)), 4u);
        origin[a] = -0.5f * static_cast<float>(res[a]) * voxel;
    }
    sdf.grid_origin = {origin[0], origin[1], origin[2]};
    sdf.voxel_size = voxel;
    sdf.resolution = {res[0], res[1], res[2]};
    sdf.local_bounds = assets::Aabb{core::Vec3{-half.x, -half.y, -half.z}, half};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t z = 0; z < res[2]; ++z) {
        for (std::uint32_t y = 0; y < res[1]; ++y) {
            for (std::uint32_t x = 0; x < res[0]; ++x) {
                const core::Vec3 p{sdf.grid_origin.x + (static_cast<float>(x) + 0.5f) * voxel,
                                   sdf.grid_origin.y + (static_cast<float>(y) + 0.5f) * voxel,
                                   sdf.grid_origin.z + (static_cast<float>(z) + 0.5f) * voxel};
                const float d = box_sdf_distance(p, half);
                sdf.distances[sdf.index(x, y, z)] = d;
                max_abs = std::fmax(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

// Where building `b` stands and how big it is — `building_frame` in blockkit/src/block.cpp,
// re-derived from the same authored params, exactly as `hero_face` above does. Kept as a second
// derivation on purpose: if the two ever disagree the field sits where the buildings are not, and
// the GI would be subtly wrong with nothing failing.
[[nodiscard]] core::Vec3 building_centre(std::uint32_t b, const blockkit::BlockParams& p) noexcept {
    const std::uint32_t side = b / p.buildings_per_side;
    const std::uint32_t idx = b % p.buildings_per_side;
    return {p.footprint * 0.5f + static_cast<float>(idx) * (p.footprint + p.building_gap),
            static_cast<float>(p.storeys) * p.storey_height * 0.5f,
            (p.street_width + p.footprint) * 0.5f * (side == 0 ? -1.0f : 1.0f)};
}

// ── The hero beat: a demolition charge on the corner ────────────────────────────────────────────
//
// A RIFLE CANNOT DO THIS, and pretending otherwise would be the dishonest version of this sample.
// Cooked parts stand at 1.0 health and a hitscan damages what it touches, so sustained fire opens a
// hole and drops a wall — real, and worth showing, but nowhere near ADR-0035 §1's peak-debris
// floor. block_standup_test reaches that floor by striking every slab at once with a deliberately
// maximal damage, and says plainly that it is measuring what the CONTENT can supply rather than
// what a shot does.
//
// So the demo's hero beat is a CHARGE — a radial explosion at the building's street corner, which
// is an ordinary game action and exactly what `apply_damage(instance, point, radius, damage,
// impulse)` already models. It is applied on the SERVER, like every other damage source, and
// reaches the client through the same replication path the rifle's damage takes.
struct Charge {
    core::Vec3 at{};
    // Sized to bring the HERO BUILDING down — ADR-0035 §1 wants >= 400 peak live debris and one
    // hero building's 420 parts is what makes that reachable at all (block_standup_test measured
    // exactly that). A background building's 180 could not supply it however hard it were hit.
    //
    // It is applied to that building's slabs ONLY. Whether the collapse then spreads is the
    // question m13.6 answered: it used to, and it was contact damage rather than the charge.
    float radius = 11.0f;
    float damage = 9.0f;
    float impulse = 7.0f;
};

[[nodiscard]] Charge hero_charge(const blockkit::BlockParams& p) noexcept {
    const core::Vec3 face = hero_face(p);
    Charge c;
    // The street-side corner nearest the player's approach, at ground level: the blow-out takes the
    // corner out from under the storeys above it, so the collapse is a cascade through the bond
    // graph rather than every part being struck directly.
    c.at = {face.x - p.footprint * 0.5f, 1.2f, face.z};
    return c;
}

// Detonate on the server. Every slab of the target building is offered the blast; `apply_damage`
// decides per part whether the point is within radius, so parts on the far side simply survive —
// which is what makes this a corner blow-out rather than a building deletion.
std::size_t detonate(Session& s, std::uint32_t building, const Charge& charge) {
    std::vector<destruction::InstanceId> slabs;
    s.server.world.query<blockkit::SlabRole, destruction::DestructibleInstanceRef>().for_each(
        [&](blockkit::SlabRole& role, destruction::DestructibleInstanceRef& ref) {
            if (role.building == building && blockkit::slab_kind::is_destructible(role.kind)) {
                slabs.push_back(destruction::InstanceId{ref.instance});
            }
        });
    for (const destruction::InstanceId inst : slabs) {
        s.server.destruction.apply_damage(
            inst, charge.at, charge.radius, charge.damage, {0.0f, charge.impulse * 0.3f, 0.0f});
    }
    return slabs.size();
}

// ── The visuals ─────────────────────────────────────────────────────────────────────────────────
//
// Owned separately from the session because they need a DEVICE and the simulation does not. A host
// with no Vulkan still runs every sim assertion in --headless; only the render ones are skipped.
struct Visuals {
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    destruction_render::PartLeafRenderer leaves;
    render::SceneRenderer renderer;
    render::text::HudRenderer hud;
    blockkit::BlockPalette palette;
    ecs::Entity camera = ecs::kNullEntity;
    // The pose the SCENE authored for its camera, kept because the demo overwrites the camera
    // entity with the player's eye every frame. See `use_authored_camera`.
    core::Transform authored_camera{};
    render::RGTexture last_ldr{};
    render::CullStats cull{};
    destruction_render::LeafStats leaf_stats{};

    // The M10 stack's work, ACCUMULATED across frames (m13.L). Every one of these counters is reset
    // per frame by its subsystem — `SdfClipmap::add` does `stats_ = {}` on entry, and a converged
    // clipmap deliberately declares nothing at all ("idle work is a bug", ADR-0032 §11). So a claim
    // that reads them after four frames reads the idle frame and concludes the field was never
    // composed. That is exactly what the first version of this asserted, and it was the claim that
    // was wrong rather than the engine.
    std::uint64_t sdf_stamps_total = 0;
    std::uint64_t ddgi_probes_total = 0;
    std::uint64_t spot_maps_total = 0; // rendered + reused

    explicit Visuals(rhi::Device& device)
        : meshes(device), renderer(device, meshes, materials), hud(device, render::kLdrFormat) {
        // ── THE WHOLE M10 STACK, ON (m13.L) ──────────────────────────────────────────────────────
        //
        // It was not, and that is worth writing down rather than quietly fixing. m13.5 set
        // `clustered_enabled` and nothing else — copied from `block_render_test`, which only ever
        // needed the light cap lifted — so the vision demo shipped with **no shadows, no GI, no
        // SSR**. M13's "done when" names M8+**M10**+M11+M12, and the M10 clause was unfulfilled by
        // one line. Nothing caught it: the render claim was "the frame came back lit", and ambient
        // plus clustered point lights is lit.
        //
        // Every gate below is independently defaulted OFF (settings.hpp: "a caller opts in, and
        // until it does the frame is the byte-identical pre-M10 baseline"). That default is right
        // for the regression bridge and it is a trap for a demo, because opting in is silent in
        // both directions. The claims in run_headless now assert each gate is on AND that its
        // subsystem did work — a counter, not a flag, because a flag can be true while the pass
        // never runs.
        render::LightingSettings lighting;

        // Clustered forward is REQUIRED, not a preference: the block carries 36 local lights and
        // the ADR-0022 uniform-block path caps at 16. Without it the frame is not a worse picture,
        // it is a different scene (the m13.2d note).
        lighting.clustered_enabled = true;

        // The sun, through cascades. Three at 2048 over a 44 m street: the near cascade covers the
        // block the player is standing in, the far one the end of the road.
        lighting.shadows_enabled = true;
        lighting.cascade_count = 3;
        lighting.shadow_map_resolution = 2048;

        // The 36 street spots and interior points. NOTE THE CAP, because it bites here and is not
        // this brick's to fix: `kMaxLocalShadows = 8` (settings.hpp:25), so 8 of 36 cast shadows
        // and the other 28 render as unshadowed cones — selected by ECS iteration order, not by
        // relevance. The priority atlas that would evict by intensity x coverage is named as the
        // m10.2 fast-follow and does not exist. Left as a measured limitation for M16.
        lighting.local_shadows_enabled = true;
        lighting.local_shadow_resolution = 1024;

        // The traceable field, and the probes that read it. DDGI REQUIRES the clipmap — it traces
        // the same field m10.4b composes.
        //
        // The lattice is camera-centred and recentres as the player walks, so it does not need to
        // span the street: 8x6x8 at 2 m is a 14x10x14 m volume around the eye, which is the room
        // and the road in front of it. lit-rooms uses 0.5 m spacing for one interior; a street
        // wants reach over resolution.
        lighting.sdf_clipmap_enabled = true;
        lighting.ddgi_enabled = true;
        lighting.ddgi_probe_count_x = 8;
        lighting.ddgi_probe_count_y = 6;
        lighting.ddgi_probe_count_z = 8;
        lighting.ddgi_probe_spacing = 2.0f;
        lighting.ddgi_rays_per_probe = 64;
        lighting.ddgi_hysteresis = 0.85f;

        // Screen-space reflections on the wet dusk road.
        lighting.ssr_enabled = true;
        lighting.ssr_max_distance = 12.0f;
        lighting.ssr_thickness = 0.5f;

        renderer.set_lighting(lighting);
        renderer.set_ambient(blockkit::kAmbient[0], blockkit::kAmbient[1], blockkit::kAmbient[2]);
    }

    // Derive the look from the roles the scene file carries, and find the camera the block
    // authored.
    void dress(ecs::World& world) {
        palette = blockkit::build_palette(materials);
        blockkit::upload_prop_meshes(palette, meshes);
        (void)blockkit::apply_palette(world, palette);

        // The field the DDGI probes trace (m13.L). Ids 1..N are the buildings, 0 is the street.
        const blockkit::BlockParams p;
        const core::Vec3 street_half{
            p.street_length() * 0.5f + p.building_gap, 0.25f, p.street_width * 0.5f + p.footprint};
        renderer.sdf_clipmap().update_instance(
            0,
            build_box_sdf(street_half, 24),
            core::mat4_translation({p.street_length() * 0.5f, -0.25f, 0.0f}));
        const core::Vec3 building_half{p.footprint * 0.5f,
                                       static_cast<float>(p.storeys) * p.storey_height * 0.5f,
                                       p.footprint * 0.5f};
        const assets::MeshSdfAsset building_sdf = build_box_sdf(building_half, 16);
        for (std::uint32_t b = 0; b < p.building_count(); ++b) {
            // One baked grid, eight placements: every building is the same box, so re-baking it per
            // instance would be eight identical voxel grids and eight times the cook.
            renderer.sdf_clipmap().update_instance(
                b + 1, building_sdf, core::mat4_translation(building_centre(b, p)));
        }

        // The camera the block itself authored (blockkit places one at the west viewpoint), found
        // rather than spawned: the demo poses an existing entity so that a scene loaded from disk
        // brings its own camera, exactly as any other content would.
        world.query<ecs::WorldTransform, render::Camera>().for_each(
            [&](ecs::Entity e, ecs::WorldTransform& tf, render::Camera&) {
                if (camera == ecs::kNullEntity) {
                    camera = e;
                    authored_camera = tf.value;
                }
            });
    }
};

// ── The demo, assembled ─────────────────────────────────────────────────────────────────────────
//
// ONE object for all three modes, because three modes that each build the world their own way is
// three things to keep in step and only one of them is ever run by CI. What differs between the
// modes is where the input comes from and who owns the loop — never what is standing there.
struct Demo {
    app::Application app;
    Session session{0x13B10Cull};
    std::unique_ptr<Visuals> visuals;

    // m13.4 measured 0.35 against 10-destructible-wall's 43 voices from 18 events. A collapsing
    // city block is an order of magnitude past that — nearly a thousand events — and at 0.35 the
    // mixdown peaks at 1.05 and clips. The gain is a property of the SCENE's density, not a
    // constant of the engine, so it is set here where the density is known.
    static audio::MixerConfig block_mix() {
        audio::MixerConfig c;
        c.master_gain = 0.12f;
        return c;
    }

    audio::Mixer mixer{audio::SoundBank::engine_defaults(), block_mix()};
    audio::MixStats audio_stats{};

    // Pose the camera from the SCENE rather than from the player.
    //
    // The render claims are made about the frames this is on for, and that is a correction rather
    // than a preference. The player's eye follows a scripted tape that walks AT a building, so the
    // captured frame was a close-up of a wall: 68 of 2,051 draws submitted, and "the frame came
    // back lit" was a statement about masonry. The block authors a camera at the west end looking
    // down its own street (1,830 submitted) — that is the view the scene is about, and the view a
    // claim about the scene should be measured on. The m13.2d lesson, in a third place.
    bool use_authored_camera = false;

    std::uint64_t frames_rendered = 0;
    std::uint64_t audio_events = 0;
    bool charge_fired = false;

    explicit Demo(const app::AppConfig& cfg) : app(cfg) {}

    [[nodiscard]] bool start(const std::filesystem::path& cooked, std::string_view scene_path) {
        if (app.device() != nullptr) {
            visuals = std::make_unique<Visuals>(*app.device());
        }
        if (!session.start(cooked,
                           scene_path,
                           visuals ? &visuals->leaves : nullptr,
                           visuals ? &visuals->meshes : nullptr)) {
            return false;
        }
        if (visuals) {
            visuals->dress(session.client.world);
            if (visuals->camera == ecs::kNullEntity) {
                std::fprintf(stderr, "99-the-block: the block's scene carries no camera\n");
                return false;
            }
        }
        return true;
    }

    // One simulation tick, plus everything downstream of it that is not drawing.
    void step_sim(const replication::InputCommand& intent) {
        session.tick(intent);

        // The scripted hero beat. In --play a key does this instead; either way it is one server
        // side call, so the two paths cannot drift into two different explosions.
        if (!charge_fired && session.tick_index >= kChargeAt) {
            fire_charge();
        }

        // Destruction → audio. The same event stream that drives dust in 10-destructible-wall; the
        // listener rides the camera, so what you hear is where you are standing.
        audio::Listener listener;
        const core::Transform eye = session.client.camera_transform();
        listener.position = eye.translation;
        // The listener takes the camera's ROTATION, not a forward vector: same convention as
        // render::Camera and FirstPersonView, so pointing the camera IS pointing the listener and
        // the two cannot disagree about which way is left.
        listener.orientation = eye.rotation;
        mixer.set_listener(listener);
        using K = destruction::DestructionEventKind;
        for (const destruction::DestructionEvent& e : session.client.destruction.events()) {
            // Events carry a world-space AABB rather than a point — the affected part or island —
            // so the sound is placed at its centre, as 10-destructible-wall does.
            const core::Vec3 at{(e.world_bounds.min.x + e.world_bounds.max.x) * 0.5f,
                                (e.world_bounds.min.y + e.world_bounds.max.y) * 0.5f,
                                (e.world_bounds.min.z + e.world_bounds.max.z) * 0.5f};
            switch (e.kind) {
                case K::PartDied:
                    mixer.play(audio::sound::kPartBreak, at, std::min(1.0f, e.magnitude));
                    ++audio_events;
                    break;
                case K::IslandDetached:
                    mixer.play(audio::sound::kCollapse, at, 1.0f);
                    ++audio_events;
                    break;
                case K::DebrisSettled:
                    // Quiet on purpose: a settle per chunk is dozens of events, and at full gain
                    // they would drown out the break that caused them.
                    mixer.play(audio::sound::kDebrisSettle, at, 0.35f);
                    ++audio_events;
                    break;
                default:
                    break;
            }
        }
        // Render one tick's worth of audio so the mixer's voices actually age and retire. CI is
        // deaf, so the samples go nowhere — but a mixer that is never rendered would report a voice
        // count that only ever grows, and the "voices retire" claim would be untested here.
        static std::vector<float> scratch(
            static_cast<std::size_t>(static_cast<double>(audio::kSampleRate) * kDt) * 2u, 0.0f);
        mixer.render(scratch);
        audio_stats = mixer.stats();

        // DRESS WHATEVER JUST ARRIVED, before the leaves are built from it.
        //
        // `apply_palette` derives MeshRef/MaterialRef from the reflected SlabRole the scene carries
        // (m13.2c). Running it once at load is right for a peer that loads the block off disk — and
        // wrong for a CLIENT, whose destructibles arrive over the wire tick by tick and therefore
        // miss that one call entirely. The symptom is precise and silent: every count is correct,
        // 2,016 leaves exist and follow the sim perfectly, and the renderer considers 35 entities
        // because a leaf with no material is not a draw. Cheap and idempotent, so it runs on the
        // ticks that bound something and is skipped on the overwhelming majority that did not.
        if (visuals && session.client_bound.bound > 0) {
            (void)blockkit::apply_palette(session.client.world, visuals->palette);
        }

        // The render bridge follows the sim, once per tick, AFTER DestructionWorld::update — a leaf
        // posed before the update would draw last tick's pile.
        if (visuals) {
            visuals->leaf_stats = visuals->leaves.update(
                session.client.world, session.client.destruction, session.client.physics);
        }
    }

    void fire_charge() {
        charge_fired = true;
        const blockkit::BlockParams p;
        (void)detonate(session, p.hero_south, hero_charge(p));
    }

    // Pose the camera from the PREDICTED player and draw the client's world. `ctx.world` is the
    // Application's own and is deliberately empty: the thing on screen is what a client holds,
    // which is the only honest thing to show in a networked demo.
    void render(app::FrameContext& ctx) {
        if (!visuals || ctx.graph == nullptr) {
            return;
        }
        ecs::World& world = session.client.world;
        if (auto* tf = world.get<ecs::WorldTransform>(visuals->camera)) {
            tf->value =
                use_authored_camera ? visuals->authored_camera : session.client.camera_transform();
        }
        visuals->renderer.reset_cull_stats();
        visuals->last_ldr = visuals->renderer.render(*ctx.graph, world, ctx.extent, true).ldr;
        visuals->cull = visuals->renderer.cull_stats();
        visuals->sdf_stamps_total += visuals->renderer.sdf_clipmap().stats().stamps;
        visuals->ddgi_probes_total += visuals->renderer.ddgi_stats().probes_updated;
        visuals->spot_maps_total += visuals->renderer.local_shadow_stats().rendered +
                                    visuals->renderer.local_shadow_stats().reused;
        draw_hud(ctx);
        visuals->hud.declare(*ctx.graph, visuals->last_ldr);
        ctx.present = visuals->last_ldr; // the one line that is the whole windowed path (m13.3a)
        ++frames_rendered;
    }

    void draw_hud(app::FrameContext& ctx) {
        namespace st = render::text::style;
        using render::text::HudRenderer;
        render::text::HudRenderer& hud = visuals->hud;
        hud.begin(ctx.extent);

        constexpr float kW = 300.0f;
        const float h = st::kPadding * 2.0f + st::kLineHeight * 7.4f;
        hud.panel(st::kPadding, st::kPadding, kW, h);

        float y = st::kPadding * 2.0f;
        const float left = st::kPadding * 2.0f;
        hud.text(left, y, "RIME", st::kAccent, 18.0f);
        hud.text(left + HudRenderer::text_width("RIME ", 18.0f),
                 y + 3.0f,
                 "the block",
                 st::kLabel,
                 14.0f);
        y += st::kLineHeight * 1.6f;

        char buf[80];
        const auto row = [&](const char* label, const char* value, render::text::Color c) {
            hud.text(left, y, label, st::kLabel, st::kTextSize);
            const float w = HudRenderer::text_width(value, st::kTextSize);
            hud.text(st::kPadding + kW - st::kPadding - w, y, value, c, st::kTextSize);
            y += st::kLineHeight;
        };

        std::snprintf(buf,
                      sizeof(buf),
                      "%llu / %llu",
                      static_cast<unsigned long long>(visuals->cull.submitted),
                      static_cast<unsigned long long>(visuals->cull.considered()));
        row("drawn", buf, visuals->cull.culled > 0 ? st::kAccent : st::kWarn);

        std::snprintf(buf, sizeof(buf), "%zu", session.parts_alive());
        row("parts", buf, st::kText);
        std::snprintf(buf,
                      sizeof(buf),
                      "%zu",
                      live_debris(session.client.destruction, session.client.physics));
        row("debris", buf, st::kText);
        std::snprintf(buf, sizeof(buf), "%zu", visuals->leaf_stats.leaves_live);
        row("leaves", buf, st::kText);
        // The convergence claim, on screen: the two hashes are the peers agreeing or not.
        const bool agree =
            session.client.destruction.state_hash() == session.server.destruction.state_hash();
        row("peers", agree ? "in sync" : "DIVERGED", agree ? st::kText : st::kWarn);
        std::snprintf(buf, sizeof(buf), "%zu", mixer.voice_count());
        row("voices", buf, st::kText);
    }
};

// ── --headless: M13's "done when", as a CI-gated self-check ─────────────────────────────────────
int run_headless(const std::filesystem::path& cooked, std::string_view scene_path) {
    app::AppConfig cfg{};
    cfg.gpu = true; // a device if there is one; the sim half runs either way
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;

    Demo demo(cfg);
    const bool has_device = demo.app.device() != nullptr;
    if (!has_device) {
        std::fprintf(stderr,
                     "99-the-block: no Vulkan device — the render claims will be skipped\n");
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            return 1;
        }
    }
    if (!demo.start(cooked, scene_path)) {
        return 1;
    }

    const MeasuredScene& stats = demo.session.server.measured;
    std::printf("99-the-block: scene digest 0x%016llx%s\n",
                static_cast<unsigned long long>(stats.placement_digest),
                scene_path.empty() ? " (generated)" : " (loaded from --scene)");
    std::printf("99-the-block: %zu entities, %zu destructibles, %zu parts, %zu local lights\n",
                stats.entities,
                stats.destructibles,
                stats.parts,
                stats.local_lights);

    const std::size_t parts_at_start = demo.session.parts_alive();
    const std::vector<std::size_t> parts_at_start_per = demo.session.parts_per_building();
    std::size_t peak_leaves = 0;

    // The frame the render claims are made about is drawn while the block is STANDING, a few ticks
    // in — not at the end. Two reasons, and both were found the hard way. After the script there is
    // no block left to consider, so "the cull considered every part" would measure rubble (the
    // m13.2d lesson about a proof pinned to an accident, in a new place). And at tick 0 the leaves
    // do not exist yet — `PartLeafRenderer::update` runs inside step_sim — so the first frame draws
    // 35 props and nothing else, which passes "the cull did work" while drawing no block at all.
    // WHEN, expressed as a condition rather than a tick number: the first frame at which the client
    // holds the whole block. A fixed tick was wrong twice — too early and the leaves do not exist,
    // slightly later and replication has still only delivered part of the 140 destructibles over a
    // lossy 80 ms link, so the cull sees 35 props and the claim fails for a reason that has nothing
    // to do with culling. "Once the client has every part" is the thing actually meant.
    bool intact_captured = false;
    render::CullStats intact_cull{};
    double intact_luma = 0.0;

    // What the M10 stack did on the frames the render claims are made about.
    struct LightingWork {
        std::uint64_t spot_maps = 0;
        std::uint64_t ddgi_probes = 0;
        std::uint64_t sdf_stamps = 0;
    } lit;

    if (demo.visuals) {
        demo.app.on_render([&demo](app::FrameContext& ctx) { demo.render(ctx); });
    }

    // Run the script. Every mode drives the SAME step_sim; only the intent differs.
    for (std::uint64_t t = 0; t < g_ticks; ++t) {
        demo.step_sim(scripted_tape(t));
        if (demo.visuals) {
            peak_leaves = std::max(peak_leaves, demo.visuals->leaf_stats.leaves_live);
        }
        // A timeline, not just a verdict. When a claim about "parts fell" fails there are two very
        // different causes — nothing broke, or everything did — and a single before/after pair
        // cannot tell them apart. This is what caught the first draft's block standing up and then
        // quietly demolishing itself before a shot was fired.
        if (!intact_captured && demo.visuals &&
            demo.visuals->leaf_stats.leaves_live >= stats.parts) {
            intact_captured = true;
            demo.use_authored_camera = true; // the claims below are about the BLOCK, not the wall
            demo.app.run_frames(4);
            intact_cull = demo.visuals->cull;
            // Snapshot what the LIGHTING actually did on those frames (m13.L). Counters, not the
            // settings flags: a flag can be true while the pass never runs, which is the whole
            // failure mode being closed here.
            lit.spot_maps = demo.visuals->spot_maps_total;
            lit.ddgi_probes = demo.visuals->ddgi_probes_total;
            lit.sdf_stamps = demo.visuals->sdf_stamps_total;
            if (render::RenderGraph* graph = demo.app.graph()) {
                const rhi::TextureHandle tex = graph->physical(demo.visuals->last_ldr);
                if (tex.is_valid()) {
                    const std::vector<std::uint8_t> px =
                        read_rgba8(*demo.app.device(), tex, kWidth, kHeight);
                    intact_luma = mean_luma(px);
                    if (!g_ppm.empty()) {
                        write_ppm(g_ppm.c_str(), px, kWidth, kHeight);
                    }
                }
            }
            demo.app.finish_gpu();
            demo.use_authored_camera = false; // back to the player for the rest of the run
        }
        if (t % 100 == 0 || t == g_ticks - 1) {
            std::printf("    t=%4llu  parts %5zu  live debris %4zu  ops %llu\n",
                        static_cast<unsigned long long>(t),
                        demo.session.parts_alive(),
                        live_debris(demo.session.server.destruction, demo.session.server.physics),
                        static_cast<unsigned long long>(demo.session.damage_ops));
        }
        // Let the handshake settle before the tape's clock starts, so "the tape steps off at tick
        // 30" and "the measurement counts from tick 30" are the same tick.
        if (demo.session.tape_origin == 0 && demo.session.client.local_player().is_valid()) {
            demo.session.tape_origin = demo.session.tick_index;
        }
    }

    // ── Drain to quiescence, with a bound ───────────────────────────────────────────────────────
    // The script ends while the pile is still falling and the wire still has traffic on it, so
    // "the peers agree" cannot be asked yet. Run on with an empty tape until they do — and BOUND
    // it, so the claim cannot pass by never converging. That is the shape the m13.2b eviction proof
    // was corrected to: assert that it CATCHES UP, with a limit, rather than asserting a state that
    // happened to hold at whatever tick the loop stopped.
    constexpr std::uint64_t kSettleBound = 600;
    std::uint64_t settled_after = 0;
    const auto agree = [&demo] {
        return destruction_net::shared_state_hash(demo.session.server.world,
                                                  demo.session.server.replicator->map(),
                                                  demo.session.server.destruction) ==
               destruction_net::shared_state_hash(demo.session.client.world,
                                                  demo.session.client.replicator->map(),
                                                  demo.session.client.destruction);
    };
    while (settled_after < kSettleBound && !agree()) {
        demo.step_sim(replication::InputCommand{});
        ++settled_after;
    }
    std::printf("  settled   : peers agreed after %llu quiet ticks (bound %llu)\n",
                static_cast<unsigned long long>(settled_after),
                static_cast<unsigned long long>(kSettleBound));

    const std::size_t parts_at_end = demo.session.parts_alive();
    {
        const std::vector<std::size_t> per = demo.session.parts_per_building();
        std::printf("  per bldg  :");
        for (std::size_t i = 0; i + 1 < per.size(); ++i) {
            std::printf(" %zu", per[i]);
        }
        std::printf("  (crates %zu)\n", per.back());
    }
    // shared_state_hash, NOT DestructionWorld::state_hash(). The latter folds physics BODY IDS and
    // local instance indices, which are each peer's own bookkeeping — two peers in perfect
    // agreement about what is broken will still disagree on it, and the first draft of this claim
    // failed for exactly that reason. shared_state_hash covers per-part alive bits and health plus
    // debris composition, walked in NetId order: the things both peers are supposed to share, named
    // the way both peers name them.
    const std::uint64_t server_hash =
        destruction_net::shared_state_hash(demo.session.server.world,
                                           demo.session.server.replicator->map(),
                                           demo.session.server.destruction);
    const std::uint64_t client_hash =
        destruction_net::shared_state_hash(demo.session.client.world,
                                           demo.session.client.replicator->map(),
                                           demo.session.client.destruction);
    const destruction_net::DestructionClient& dc = demo.session.client.destruction_net_client;

    std::printf("  scale     : parts %zu >= %zu, local lights %zu >= %zu\n",
                stats.parts,
                kMinParts,
                stats.local_lights,
                kMinLocalLights);
    std::printf("  player    : spawned %s, predicted %s\n",
                demo.session.client.local_player().is_valid() ? "yes" : "NO",
                demo.session.client.predictor.seeded() ? "yes" : "NO");
    std::printf("  weapon    : %llu shots, %llu hits, %llu damage ops\n",
                static_cast<unsigned long long>(demo.session.shots_fired),
                static_cast<unsigned long long>(demo.session.shots_hit),
                static_cast<unsigned long long>(demo.session.damage_ops));
    std::printf("  breakage  : parts %zu -> %zu, peak live debris %zu, peak visual %zu\n",
                parts_at_start,
                parts_at_end,
                demo.session.peak_live_debris,
                demo.session.peak_visual_debris);
    std::printf("  replicated: %llu ticks, %llu ops, composition %llu ok / %llu bad, "
                "debris %llu bound / %llu unresolved\n",
                static_cast<unsigned long long>(dc.ticks_applied()),
                static_cast<unsigned long long>(dc.ops_applied()),
                static_cast<unsigned long long>(dc.composition_matches()),
                static_cast<unsigned long long>(dc.composition_mismatches()),
                static_cast<unsigned long long>(dc.debris_bound()),
                static_cast<unsigned long long>(dc.debris_unresolved()));
    std::printf("  damage    : %llu ops, %llu instant kills (>= 1.0 health), largest %.2f\n",
                static_cast<unsigned long long>(demo.session.total_ops),
                static_cast<unsigned long long>(demo.session.lethal_ops),
                static_cast<double>(demo.session.max_op_amount));
    std::printf("  audio     : %llu events, %zu voices, peak %.2f, %llu clipped\n",
                static_cast<unsigned long long>(demo.audio_events),
                demo.mixer.voice_count(),
                static_cast<double>(demo.audio_stats.peak),
                static_cast<unsigned long long>(demo.audio_stats.clipped_samples));

    // ── COLLAPSE LOCALITY — m13.5 found this failing, m13.6 fixed it, and it is a claim now ─────
    //
    // The defect: one demolition charge on ONE building used to flatten the entire block, across
    // the 12 m gap to its neighbours and then across the 12 m street. Not the player, not the
    // rifle, and not the charge's size — `--idle` reproduced it with a player who never moves.
    //
    // The cause was contact damage tuned for a different object. `damage_threshold` is cooked per
    // pattern and the fracturer's 5.0 default was set for M8's small test wall; a building slab
    // part is two orders of magnitude heavier. Measured here: contact impulses reached 669 kg·m/s
    // against a threshold of 5 and a part health of 1.0, so 729 of 799 contact ops were INSTANT
    // KILLS and the cascade could not damp. The cooks now carry their own tuning
    // (--damage-threshold /
    // --damage-scale, m13.6) and the same charge leaves the far side of the street untouched.
    //
    // WHAT IS ASSERTED, and why it is not "only the charged building falls": a nine-metre building
    // collapsing four metres from its neighbour SHOULD hurt it, and the demo shows exactly that —
    // the two adjacent buildings lose parts. What must not happen is the damage carrying on across
    // the street. So the claim is about REACH, which is the thing that was broken.
    const std::vector<std::size_t> parts_end_per = demo.session.parts_per_building();
    std::size_t far_side_intact = 0;
    std::size_t fully_intact = 0;
    {
        const blockkit::BlockParams p;
        std::printf("  locality  :");
        for (std::size_t b = 0; b + 1 < parts_end_per.size(); ++b) {
            const bool intact = parts_end_per[b] >= parts_at_start_per[b];
            // "Far side" = the north row, across the street from the south hero that was charged.
            if (b >= p.buildings_per_side && intact) {
                ++far_side_intact;
            }
            if (intact) {
                ++fully_intact;
            }
            std::printf(" %zu/%zu", parts_end_per[b], parts_at_start_per[b]);
        }
        std::printf("  (far side intact %zu/%u)\n", far_side_intact, p.buildings_per_side);
    }

    struct Claim {
        const char* name;
        bool ok;
    };

    std::vector<Claim> claims{
        // 1. It composes, at ADR-0035 §1's declared scale.
        {"scale: parts >= 1500", stats.parts >= kMinParts},
        {"scale: local lights >= 32", stats.local_lights >= kMinLocalLights},
        // 4. The player is in it, and PREDICTED — the m13.3a deferral, closed here.
        {"player: avatar replicated to the client", demo.session.client.local_player().is_valid()},
        {"player: the predictor is seeded", demo.session.client.predictor.seeded()},
        // 2. It is destructible under fire, and the pile then settles.
        {"weapon: shots reached the destructible block", demo.session.damage_ops > 0},
        {"breakage: parts fell", parts_at_end < parts_at_start},
        {"breakage: peak live debris >= 400", demo.session.peak_live_debris >= kMinPeakDebris},
        {"breakage: the visual budget stayed above the live one",
         demo.session.peak_visual_debris >= demo.session.peak_live_debris},
        // 3. The peers agree — the composition claim that a silent divergence would break.
        {"net: destruction replicated to the client", dc.ops_applied() > 0},
        {"net: composition checks all matched", dc.composition_mismatches() == 0},
        {"net: no debris left unresolved", dc.debris_unresolved() == 0},
        {"net: the peers' destruction state hashes agree", server_hash == client_hash},
        {"net: they agreed WITHIN the settle bound", settled_after < kSettleBound},
        // The claim m13.5 shipped as a KNOWN DEFECT and m13.6 earned. Before the fix this read
        // 0 of 4 and 0 of 8 — the whole block, every time.
        {"collapse: the far side of the street is untouched",
         far_side_intact == blockkit::BlockParams{}.buildings_per_side},
        {"collapse: most of the block is still standing", fully_intact >= 5},
        // Audio ran and did not distort.
        {"audio: destruction drove the mixer", demo.audio_events > 0},
        {"audio: nothing clipped", demo.audio_stats.clipped_samples == 0},
    };
    if (demo.visuals) {
        // 5. It draws. Device-gated, so a GPU-less host skips these and keeps the rest. Every
        // number here is from the INTACT frames drawn before the script, for the reason given
        // above.
        claims.push_back({"render: frames were drawn", demo.frames_rendered > 0});
        claims.push_back(
            {"render: the cull considered every part", intact_cull.considered() >= stats.parts});
        claims.push_back({"render: the cull rejected some and kept some",
                          intact_cull.culled > 0 && intact_cull.submitted > 0});
        claims.push_back(
            {"render: a leaf exists for every standing part", peak_leaves >= stats.parts});
        claims.push_back({"render: every leaf found its mesh",
                          demo.visuals->leaf_stats.instances_without_meshes == 0});
        // The dusk rig is dark by design, so this is a deliberately modest bar: it separates "a lit
        // street came back" from "the pass ran and produced black", which is the failure a counter
        // cannot see. block_render_test makes the sharper comparative claim.
        claims.push_back({"render: the frame came back lit", intact_luma > 1.0});

        // ── The M10 clause of M13's "done when" (m13.L) ──────────────────────────────────────
        //
        // m13.5 shipped this demo with only `clustered_enabled` and every claim green, because
        // "the frame came back lit" is satisfied by ambient plus point lights. These are the
        // assertions that would have caught it, and they are deliberately about WORK DONE rather
        // than about the settings struct: `lighting().shadows_enabled` being true proves someone
        // set a bool, while `local_shadow_stats().rendered + .reused` being non-zero proves a
        // shadow map exists.
        const render::LightingSettings& ls = demo.visuals->renderer.lighting();
        claims.push_back({"lighting: every M10 gate is on",
                          ls.shadows_enabled && ls.local_shadows_enabled && ls.clustered_enabled &&
                              ls.sdf_clipmap_enabled && ls.ddgi_enabled && ls.ssr_enabled});
        claims.push_back({"lighting: spot shadow maps were produced", lit.spot_maps > 0});
        claims.push_back({"lighting: the SDF field was composed", lit.sdf_stamps > 0});
        claims.push_back({"lighting: DDGI probes were traced", lit.ddgi_probes > 0});
        std::printf("  lighting  : %llu spot maps, %llu sdf stamps, %llu probe updates\n",
                    static_cast<unsigned long long>(lit.spot_maps),
                    static_cast<unsigned long long>(lit.sdf_stamps),
                    static_cast<unsigned long long>(lit.ddgi_probes));
        std::printf("  drawn     : %llu culled of %llu considered, %llu submitted, %zu leaves, "
                    "mean luma %.2f\n",
                    static_cast<unsigned long long>(intact_cull.culled),
                    static_cast<unsigned long long>(intact_cull.considered()),
                    static_cast<unsigned long long>(intact_cull.submitted),
                    peak_leaves,
                    intact_luma);
    }

    int failed = 0;
    std::printf("\n  M13 \"done when\":\n");
    for (const Claim& c : claims) {
        std::printf("    [%s] %s\n", c.ok ? " ok " : "FAIL", c.name);
        failed += c.ok ? 0 : 1;
    }
    demo.app.finish_gpu(); // we drove the loop; we owe the idle wait before anything is destroyed
    if (failed != 0) {
        std::fprintf(stderr, "99-the-block: %d of %zu claims failed\n", failed, claims.size());
        return 1;
    }
    std::printf("\n99-the-block: all %zu claims hold — M13's \"done when\" is met.\n",
                claims.size());
    return 0;
}

// ── --perf: the hardware report (m13.p, ADR-0035 §2b) ───────────────────────────────────────────
//
// Deliberately NOT a CTest, and 11-lit-rooms:36-40 states the rule: CI renders on lavapipe, a CPU
// rasteriser, where a millisecond is a statement about the runner's mood. The counts stay in
// --headless where CI can fail on them forever; the clock lives here, run by hand or by
// scripts/perf.sh on a machine whose fingerprint is written into the report.
//
// IT MEASURES THE AUTHORED CAMERA, looking down the street, and that is the whole point. The
// scripted player walks AT a building and sees 68 of 2,051 draws; the authored view sees 1,830.
// Gating the frame budget on the near-empty view would be gating nothing — the number that has to
// hold is the one for the view the demo is about.
struct PerfOptions {
    int warmup = 90;        // unmeasured: DDGI convergence and the first-frame uploads are not the
                            // steady state being gated
    int frames = 600;       // 10 s at 60 Hz — enough that p99 is the 594th frame, not max
    int charge_frame = 300; // the hero building comes down mid-run, inside the sample
    int collapse = 90;      // frames after it that count as "the collapse window"
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    const char* out = nullptr;
    const char* baseline = nullptr;
};

int run_perf(const std::filesystem::path& cooked,
             std::string_view scene_path,
             const PerfOptions& opt) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {opt.width, opt.height};
    cfg.tick_hz = 60.0;

    Demo demo(cfg);
    if (demo.app.device() == nullptr) {
        std::fprintf(stderr,
                     "99-the-block --perf: no Vulkan device — a perf run needs real "
                     "hardware, and there is nothing here to measure\n");
        return 1;
    }
    if (!demo.start(cooked, scene_path)) {
        return 1;
    }
    demo.use_authored_camera = true; // see the note above

    core::PerfReport report;
    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = demo.app.device()->adapter();
    fp.gpu = adapter.name;
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    fp.preset = "block-all-lighting-gates"; // csm + local shadows + clustered + sdf + ddgi + ssr
    fp.width = opt.width;
    fp.height = opt.height;
    report.set_machine(fp);
    report.set_run(core::RunInfo::detect("99-the-block"));

    std::vector<core::PassTiming> passes;
    bool timestamps_seen = false;
    demo.app.on_post_submit([&](render::RenderGraph& graph, rhi::CommandBuffer& cmd) {
        passes.clear();
        for (const render::RenderGraph::PassTiming& t : graph.resolve_timings(cmd)) {
            passes.push_back(core::PassTiming{std::string(t.name), t.gpu_ms});
            timestamps_seen = true;
        }
    });
    demo.app.on_render([&demo](app::FrameContext& ctx) { demo.render(ctx); });

    std::printf("99-the-block --perf: %s (%s %s), %ux%u, warmup %d, measuring %d frames…\n",
                fp.gpu.c_str(),
                adapter.driver_name.c_str(),
                adapter.driver_info.c_str(),
                opt.width,
                opt.height,
                opt.warmup,
                opt.frames);

    std::uint64_t tick = 0;
    for (int i = 0; i < opt.warmup; ++i, ++tick) {
        demo.step_sim(scripted_tape(tick));
        demo.app.step(demo.app.fixed_dt());
    }

    core::ZoneTimelines zones(report);
    for (int i = 0; i < opt.frames; ++i, ++tick) {
        if (i == opt.charge_frame && !demo.charge_fired) {
            demo.fire_charge();
        }
        const core::Stopwatch watch;
        // The SIM is inside the frame's clock because a player pays for it. It is also on its own
        // timeline: ADR-0035 budgets the tick separately, and `sim.*` zones come from the
        // Application's schedule, which this demo does not use — its simulation is the Session.
        const core::Stopwatch sim_watch;
        demo.step_sim(scripted_tape(tick));
        report.observe("sim.block", sim_watch.elapsed_ms());
        // The split that decides whether the ENGINE misses the budget or this DEMO's topology does.
        report.observe("sim.client", demo.session.last_client_ms);
        report.observe("sim.server", demo.session.last_server_ms);
        const core::Stopwatch render_watch;
        demo.app.step(demo.app.fixed_dt());
        const double render_ms = render_watch.elapsed_ms();
        const double ms = watch.elapsed_ms();
        report.observe_frame(static_cast<std::uint64_t>(i), ms, passes);
        report.observe("frame.render", render_ms);
        // WHAT A PLAYER'S MACHINE WOULD PAY, and it is a diagnostic rather than the gate.
        //
        // This demo is a server AND a client in one process — that is what makes it a proof about
        // M11+M12 rather than a single-player scene — so its wall-clock frame includes a whole
        // authoritative simulation no player runs. `frame` stays the gated number, because moving
        // the goalposts to the flattering measurement is exactly what a ratified budget exists to
        // prevent. This line is here to answer the different question the budget cannot: is the
        // ENGINE too slow, or is this demo's topology?
        report.observe("frame.player", demo.session.last_client_ms + render_ms);
        if (i >= opt.charge_frame && i < opt.charge_frame + opt.collapse) {
            // The collapse, on its own timeline. A hitch there is invisible in a 600-frame p99 —
            // 90 frames cannot move the 594th — and obvious in a 90-frame one. That is exactly why
            // ADR-0035 asks for the window separately.
            report.observe("frame.collapse", ms);
        }
    }
    zones.stop();
    demo.app.finish_gpu();

    // The ledger travels WITH the timings, so a report can never be read as "fast" without also
    // being read as "…and here is the work it did" (ADR-0035 §2b's vacuity guard). A run that was
    // quick because the lighting silently switched itself off is not a pass — which is not a
    // hypothetical here: m13.L found the demo shipped with M10 off and every claim green.
    //
    // draws.submitted / draws.culled LAND HERE AT LAST. ADR-0035 §2a named them as the entry to
    // add, and 11-lit-rooms:651 still carries the note deferring them because "nothing in the
    // engine counts a draw". The frustum cull has counted them since m13.2a; the ledger entry never
    // followed.
    core::WorkLedger ledger;
    ledger.set("frames.measured", static_cast<std::uint64_t>(opt.frames));
    ledger.set("draws.submitted", demo.visuals->cull.submitted);
    ledger.set("draws.culled", demo.visuals->cull.culled);
    ledger.set("parts.alive_end", demo.session.parts_alive());
    ledger.set("debris.peak_live", demo.session.peak_live_debris);
    ledger.set("leaves.live", demo.visuals->leaf_stats.leaves_live);
    ledger.set("sdf.stamps", demo.visuals->sdf_stamps_total);
    ledger.set("ddgi.probes_updated", demo.visuals->ddgi_probes_total);
    ledger.set("shadow.spot_maps", demo.visuals->spot_maps_total);
    ledger.set("net.max_batches_per_tick", demo.session.max_batches_per_tick);
    ledger.set("net.client_physics_steps", demo.session.total_client_steps);
    report.set_ledger(ledger);

    core::PerfGate gate;
    gate.at_most("frame", core::PerfStat::P99, 16.6)
        .at_most("frame", core::PerfStat::Max, 33.0)
        .at_most("frame.collapse", core::PerfStat::Max, 33.0)
        .at_most("sim.block", core::PerfStat::P99, 6.0)
        .require_samples("frame", 200)
        .require_samples("frame.collapse", 45)
        .max_regression(0.10);
    // The vacuity guard, and every floor here is a thing that was actually found switched off at
    // some point in M13.
    gate.work()
        .at_least("draws.submitted", 1)
        .at_least("sdf.stamps", 1)
        .at_least("ddgi.probes_updated", 1)
        .at_least("shadow.spot_maps", 1)
        .at_least("debris.peak_live", 1);

    core::PerfReport baseline;
    const core::PerfReport* baseline_ptr = nullptr;
    if (opt.baseline != nullptr) {
        std::string error;
        if (core::PerfReport::load_file(opt.baseline, baseline, error)) {
            baseline_ptr = &baseline;
        } else {
            std::printf("  (no usable baseline: %s)\n", error.c_str());
        }
    }
    const core::PerfGate::Result result = gate.check(report, baseline_ptr);

    const auto frame = report.distribution("frame");
    if (!frame) {
        std::fprintf(stderr, "99-the-block --perf: no frames were measured\n");
        return 1;
    }
    std::printf("  frame     p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (%zu frames)\n",
                frame->p50_ms,
                frame->p95_ms,
                frame->p99_ms,
                frame->max_ms,
                frame->count);
    if (const auto collapse = report.distribution("frame.collapse")) {
        std::printf("  collapse  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (%zu frames)\n",
                    collapse->p50_ms,
                    collapse->p95_ms,
                    collapse->p99_ms,
                    collapse->max_ms,
                    collapse->count);
    }
    if (const auto sim = report.distribution("sim.block")) {
        std::printf(
            "  sim(both) p50 %.3f  p99 %.3f  max %.3f ms\n", sim->p50_ms, sim->p99_ms, sim->max_ms);
    }
    if (const auto c = report.distribution("sim.client")) {
        std::printf("  sim.client p50 %.3f  p99 %.3f  max %.3f ms  <- what a player pays\n",
                    c->p50_ms,
                    c->p99_ms,
                    c->max_ms);
    }
    if (const auto r = report.distribution("frame.render")) {
        std::printf(
            "  render    p50 %.2f  p99 %.2f  max %.2f ms\n", r->p50_ms, r->p99_ms, r->max_ms);
    }
    if (const auto pl = report.distribution("frame.player")) {
        std::printf("  frame.player p50 %.2f  p99 %.2f  max %.2f ms  "
                    "<- client + render, i.e. one machine\n",
                    pl->p50_ms,
                    pl->p99_ms,
                    pl->max_ms);
    }
    if (const auto sv = report.distribution("sim.server")) {
        std::printf(
            "  sim.server p50 %.3f  p99 %.3f  max %.3f ms\n", sv->p50_ms, sv->p99_ms, sv->max_ms);
    }
    std::printf("  worst frame #%llu at %.2f ms\n",
                static_cast<unsigned long long>(report.worst_frame().index),
                report.worst_frame().ms);
    if (!timestamps_seen) {
        std::printf("  (this device reports no GPU timestamps — the per-pass table is empty)\n");
    }
    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());

    if (opt.out != nullptr) {
        std::FILE* f = std::fopen(opt.out, "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "  could not write %s\n", opt.out);
            return 1;
        }
        const std::string json = report.to_json();
        (void)std::fwrite(json.data(), 1, json.size(), f);
        (void)std::fclose(f);
        std::printf("  wrote %s\n", opt.out);
    }

    std::fflush(stdout); // so the gate's stderr lands after the numbers it judges
    if (!result.ok()) {
        std::fprintf(stderr, "  PERF GATE:\n%s", core::PerfGate::format(result).c_str());
    } else {
        std::printf("  perf gate: %s", core::PerfGate::format(result).c_str());
    }
    std::printf("99-the-block --perf: %s\n",
                result.ok() ? "within budget" : "FAILED the perf gate");
    return result.ok() ? 0 : 1;
}

// ── --play: walk around it ──────────────────────────────────────────────────────────────────────
int run_play(const std::filesystem::path& cooked, std::string_view scene_path) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    cfg.windowed = true;
    cfg.window_title = "Rime — the block";
    cfg.window_size = {kWidth, kHeight};

    Demo demo(cfg);
    if (demo.app.device() == nullptr) {
        std::fprintf(stderr, "99-the-block: --play needs a Vulkan device\n");
        return 1;
    }
    if (!demo.start(cooked, scene_path)) {
        return 1;
    }

    if (demo.app.windowed()) {
        std::printf("99-the-block: presenting in a window.\n"
                    "  WASD move · hold RIGHT-DRAG to look · LEFT CLICK to fire\n"
                    "  F detonates the charge on the hero building · ESC to exit\n");
    } else {
        std::printf("99-the-block: no display available; running the script headless instead.\n");
    }

    // The live intent, rebuilt every FRAME from devices and sampled by the tick below. The frame
    // and the tick run at different rates, so the tick reads the latest fold rather than doing its
    // own — which is also what keeps the yaw the mover receives the one the player is looking
    // along.
    replication::InputCommand live{};
    bool quit = false;

    demo.app.add_sim_stage(app::Application::SimStage::PreSim, [&](ecs::World&, double) {
        demo.step_sim(live);
        live.pressed = 0; // an edge is consumed by exactly one tick, or one click fires forever
    });
    demo.app.on_render([&](app::FrameContext& ctx) {
        const gameplay::FrameIntent intent = gameplay::map_frame_input(
            demo.session.client.devices, ctx.input, demo.session.client.view);
        live = gameplay_net::to_input_command(intent.character);
        if (intent.quit) {
            quit = true;
            demo.app.request_quit();
        }
        if (demo.session.client.devices.key_pressed(platform::Key::F)) {
            demo.fire_charge();
        }
        demo.render(ctx);
    });

    if (demo.app.windowed()) {
        (void)demo.app.run();
    } else {
        demo.app.run_frames(static_cast<int>(g_ticks));
    }
    std::printf("99-the-block: %llu frames, %llu ticks%s\n",
                static_cast<unsigned long long>(demo.app.frame_index()),
                static_cast<unsigned long long>(demo.app.tick_count()),
                quit ? " (ESC)" : "");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode { Headless, Play, Perf } mode = Mode::Headless;
    PerfOptions perf{};
    std::filesystem::path cooked = RIME_BLOCK_COOKED_DIR;
    std::string scene_path; // --scene: run a scene someone authored, not the generated one (m14.4)

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless") {
            mode = Mode::Headless;
        } else if (a == "--play") {
            mode = Mode::Play;
        } else if (a == "--perf") {
            mode = Mode::Perf;
        } else if (a == "--frames" && i + 1 < argc) {
            perf.frames = std::atoi(argv[++i]);
        } else if (a == "--warmup" && i + 1 < argc) {
            perf.warmup = std::atoi(argv[++i]);
        } else if (a == "--width" && i + 1 < argc) {
            perf.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (a == "--height" && i + 1 < argc) {
            perf.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (a == "--out" && i + 1 < argc) {
            perf.out = argv[++i];
        } else if (a == "--baseline" && i + 1 < argc) {
            perf.baseline = argv[++i];
        } else if (a == "--ticks" && i + 1 < argc) {
            g_ticks = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (a == "--ppm" && i + 1 < argc) {
            g_ppm = argv[++i];
        } else if (a == "--idle") {
            g_idle = true;
        } else if (a == "--scene" && i + 1 < argc) {
            scene_path = argv[++i];
        } else if (a == "--cooked" && i + 1 < argc) {
            cooked = argv[++i];
        }
    }

    if (mode == Mode::Perf) {
        // A short probe run (scripts/perf.sh uses --frames 12) must not try to fire the charge
        // after the run has ended, nor demand a collapse window it cannot fill.
        perf.charge_frame = std::min(perf.charge_frame, perf.frames / 2);
        perf.collapse = std::min(perf.collapse, perf.frames - perf.charge_frame);
        return run_perf(cooked, scene_path, perf);
    }
    if (mode == Mode::Play) {
        return run_play(cooked, scene_path);
    }
    return run_headless(cooked, scene_path);
}
