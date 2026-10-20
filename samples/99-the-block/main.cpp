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
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "authored_environment.hpp"
#include "play_building_support.hpp"
#include "play_city.hpp"
#include "play_effects.hpp"
#include "play_environment.hpp"
#include "play_input.hpp"
#include "play_sunset.hpp"
#include "play_viewmodel.hpp"
#include "play_weapon_fx.hpp"
#include "play_weapon_policy.hpp"
#include "play_world.hpp"
#include "player_spawn.hpp"
#include "rime/app/application.hpp"
#include "rime/app/content_root.hpp"
#include "rime/assets/asset_server.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/audio/mixer.hpp"
#include "rime/audio/sink.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/core/diagnostics/log.hpp"
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
#include "rime/ground/bind.hpp"
#include "rime/ground/derive.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/lighting/sky.hpp"
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
#ifndef RIME_VIEWMODEL_COOKED_DIR
#define RIME_VIEWMODEL_COOKED_DIR "viewmodel-cooked"
#endif

namespace {

using namespace rime;
std::filesystem::path g_viewmodel_content = RIME_VIEWMODEL_COOKED_DIR;

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
bool g_effects_preview = false;
bool g_network_impairment = false;
bool g_mute = false;

// --island-profile: per-island solve TIMING (#311). Off by default because it reads a clock once
// per active island -- off the per-constraint path and tiny, but not free, so A RUN TAKEN WITH THIS
// FLAG MUST NOT BE FILED AS A BASELINE. It answers one question: what fraction of the solve is the
// biggest island in TIME, where every other counter here measures it in BODIES.
bool g_island_profile = false;

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
    block_play::register_block_components(
        world); // engine + game + PlayerSpawn, shared with the host
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

// Mean luminance over the BOTTOM `fraction` of the frame (m17.7b).
//
// The sky A/B needs this rather than the whole-frame mean above, and the difference matters: with
// the sky switched off the background reverts to the clear colour, so a whole-frame mean moves for
// two quite different reasons — the street being lit differently, and the sky simply not being
// painted. Only the first is the claim. The block's authored camera looks slightly down at the
// street, so the bottom of the frame is the surface the claim is about and carries no sky at all.
// Mean RED/BLUE ratio over the bottom `fraction` of the frame (m17.7b) — how the street is TINTED.
//
// This is the measurement the sky claim rests on, and it is deliberately a ratio rather than a
// brightness. The block's sky was tuned so its irradiance matches the flat constant it displaces to
// within a few percent, which is what keeps the demo's exposure stable — but it also means
// switching the sky OFF barely moves the street's brightness, so a luminance test would mostly be
// measuring the tuning error. Colour is immune to that: a flat constant delivers one tint no matter
// what the sky looks like, so a street whose tint FOLLOWS the sky can only be lit by the sky.
[[nodiscard]] double mean_rb_ratio_lower(const std::vector<std::uint8_t>& rgba,
                                         std::uint32_t width,
                                         std::uint32_t height,
                                         double fraction) {
    const auto first_row = static_cast<std::size_t>(static_cast<double>(height) * (1.0 - fraction));
    double r = 0.0;
    double b = 0.0;
    for (std::size_t y = first_row; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t i = (y * width + x) * 4;
            if (i + 3 >= rgba.size()) {
                break;
            }
            r += rgba[i];
            b += rgba[i + 2];
        }
    }
    return b <= 0.0 ? 0.0 : r / b;
}

[[nodiscard]] double mean_luma_lower(const std::vector<std::uint8_t>& rgba,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     double fraction) {
    const auto first_row = static_cast<std::size_t>(static_cast<double>(height) * (1.0 - fraction));
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t y = first_row; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t i = (y * width + x) * 4;
            if (i + 3 >= rgba.size()) {
                break;
            }
            sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
            ++n;
        }
    }
    return n == 0 ? 0.0 : sum / static_cast<double>(n);
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
    destruction::DestructionWorld destruction;
    std::unordered_map<std::uint64_t, destruction::PatternId> patterns;
    MeasuredScene measured;

    // The play-only city (play_city.hpp): 24 more buildings and destructible lamps and barriers.
    // Set by Demo::start BEFORE patterns are registered, and only for the generated scene -- a
    // `--scene` run keeps exactly the level it was handed.
    bool city = false;
    bool city_patterns = false;

    // The map's authored player spawn, resolved once in stand_up after the transforms exist
    // (player_spawn.hpp). `None` for every map without an enabled marker, which keeps the old
    // west-viewpoint default; copied data, so it outlives structural changes to `world`.
    block_play::SpawnResolution spawn;

    // HAND THE PHYSICS A JOB SYSTEM (m17.5). Without this pointer `PhysicsWorld` takes the
    // sequential island solve — same answer, one thread — and it had been taking it here since the
    // demo was written: each peer owned a job system and spent it only on
    // `ecs::propagate_transforms`. `09-physics-playground` and `10-destructible-wall` both wire it;
    // the one sample M13's frame-rate clause is actually about did not.
    //
    // BORROWED, one per process, which is what `set_job_system` documents ("the engine constructs
    // one job system and hands it to every subsystem") and what this demo got wrong twice over: a
    // Peer that owned one meant TWO, each sized to the machine, so a 32-core box would run ~62
    // workers on 32 cores and the parallel solve would compete with itself. That configuration was
    // AVOIDED, not measured — the run that appeared to measure it was contaminated and withdrawn.
    //
    // It is a nullable pointer, which is what let the original omission be silent: nothing fails,
    // nothing warns, the solve is just slower. Hence `physics.job_workers` in the work ledger — a
    // report that records the work but not the wiring cannot tell "parallelism did not help" from
    // "parallelism was never switched on".
    void use_jobs(core::JobSystem& shared) { physics.set_job_system(&shared); }

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
        // The city's two generated patterns ride the same seam (null leaves/meshes on a server).
        if ((city || city_patterns) &&
            !block_play::city::register_patterns(destruction, physics, patterns, leaves, meshes)) {
            std::fprintf(stderr, "99-the-block: physics rejected a city pattern\n");
            return false;
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
                                std::string_view scene_path,
                                std::uint64_t ground_material) {
        register_all(world);

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
            // The street's cooked material rides the generated scene as authored data (m17.8b): a
            // content id names cooked bytes, so unlike every other look in the block it survives
            // the save/load round trip instead of being re-derived from the role. 0 when the cook
            // did not run, and then the block is byte-identical to the one before this existed.
            blockkit::BlockParams params;
            params.ground_material = ground_material;
            (void)blockkit::assemble(authoring, params);
            if (city) {
                (void)block_play::city::expand(authoring);
            }
            if (!scene::load_scene_from_string(world, scene::save_scene_to_string(authoring)).ok) {
                std::fprintf(stderr,
                             "99-the-block: the block's own scene file did not load back\n");
                return false;
            }
        }
        (void)blockkit::derive_world_transforms(world);

        // THE AUTHORED SPAWN, resolved before anything gameplay-side starts. An enabled marker that
        // cannot be honoured stops the run here with the reason: starting the player at the old
        // default instead would hide an author's mistake. Both peers resolve (same file, same
        // answer); only the server announces it, so the line appears once.
        spawn = block_play::resolve_player_spawn(world);
        if (spawn.status == block_play::SpawnStatus::Refused) {
            std::fprintf(stderr,
                         "99-the-block: refusing player spawn (%zu enabled): %s\n",
                         spawn.enabled_count,
                         spawn.reason.c_str());
            return false;
        }
        if (spawn.status == block_play::SpawnStatus::Valid && own_destructibles) {
            std::printf("99-the-block: authored player spawn: capsule centre (%.3f, %.3f, %.3f) m, "
                        "yaw %.4f rad, pitch %.4f rad\n",
                        static_cast<double>(spawn.placement.capsule_centre.x),
                        static_cast<double>(spawn.placement.capsule_centre.y),
                        static_cast<double>(spawn.placement.capsule_centre.z),
                        static_cast<double>(spawn.placement.yaw),
                        static_cast<double>(spawn.placement.pitch));
        }

        // THE GROUND, from the scene rather than from a formula (m17.8). It used to be a static box
        // built here, before the load, from `BlockParams` — half-extent 44 against a drawn surface
        // of 38, so six metres of the world were standable and invisible. Now the surface is
        // authored in the scene and the collider is derived from it, which is why this runs AFTER
        // the load and takes no parameters.
        //
        // Still level geometry every peer stands up for itself: a client binds its own ground
        // rather than waiting to be told the floor exists, and the derivation being pure is what
        // makes the two agree.
        const ground::BindStats ground_bound = ground::bind_ground(world, physics);
        if (ground_bound.bound == 0 || ground_bound.scaled_refused != 0) {
            std::fprintf(stderr,
                         "99-the-block: the scene has no usable ground (%zu bound, %zu refused for "
                         "a scaled transform)\n",
                         ground_bound.bound,
                         ground_bound.scaled_refused);
            return false;
        }
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
        if (spawn.status == block_play::SpawnStatus::Valid) {
            // Authored: the marker IS the capsule centre, so no height adjustment.
            placement.translation = spawn.placement.capsule_centre;
        } else {
            placement.translation = blockkit::west_viewpoint(blockkit::BlockParams{});
            placement.translation.y = character.half_height + character.radius;
        }

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
        v.eye_height = block_play::play_eye_height(config);
        return gameplay::eye_transform(v, eye_anchor());
    }
};

void push_loose_hit(physics::PhysicsWorld& world,
                    physics::BodyId body,
                    core::Vec3 point,
                    core::Vec3 impulse) {
    // Impulse at contact supplies both translation and torque. Clamp momentum rather than
    // velocity, preserving mass/inertia response; PhysicsWorld rejects dead or immovable ids.
    const float magnitude = core::length(impulse);
    if (std::isfinite(magnitude) && magnitude > 0)
        world.apply_impulse(body, impulse * (std::min(magnitude, 25.0f) / magnitude), point);
}

// One process, both peers, a lossy wire between them. In-process and deterministic
// (ScriptedNetwork) rather than real sockets, because the demo's claim is about COMPOSITION at
// scale and a real socket would add a nondeterministic variable that says nothing about it. The
// latency and loss are real numbers though — 80 ms RTT and 5% loss — so the prediction path is
// genuinely exercised rather than bypassed by a zero-latency link.
struct Session {
    // Declared FIRST so it is destroyed LAST: both peers' PhysicsWorlds hold a borrowed pointer to
    // it, and members die in reverse declaration order.
    core::JobSystem jobs{0};

    net::ScriptedNetwork network;
    net::Endpoint server_endpoint{0x7F000001u, 7901};
    ServerPeer server;
    ClientPeer client;
    block_play::PlayWorld* play_world = nullptr; // owned by Demo; active in --play/--world-smoke
    // Owned by Demo; non-null ONLY in --play. The authored/headless/perf paths never set it, so
    // they run exactly as before. Stepped on the authoritative peer just before its update.
    block_play::PlayBuildingSupport* building_support = nullptr;
    bool world_stopped = false;

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::uint64_t tape_origin = 0;

    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    // Running maxima the proof reads. Peaks, not final values: a budget is about the worst moment.
    std::size_t peak_live_debris = 0;
    std::size_t peak_visual_debris = 0;

    // The solver's PARALLELISM, not just its cost (m17.5). `physics.solve` being slow says nothing
    // about whether it CAN be spread: the active-island count is the width available and the
    // largest ACTIVE island is the critical path through it, and a block that is still mostly one
    // connected building is one enormous island no number of workers can divide.
    //
    // Three deliberate choices here, each of them a wrong version this brick shipped first:
    //
    // 1. KEYED ON THE STEP, not on the peer's half of the tick. `last_server_ms` also covers
    //    gameplay, sync, the shot -> apply_damage loop and `destruction.update`, so the most
    //    expensive server TICK can easily be the tick the charge went off rather than the tick the
    //    solver was widest — and an island count sampled there answers a question nobody asked.
    // 2. PER WORLD, because there are two of them. The client re-simulates (bounded catch-up, up
    //    to `kMaxCatchUpBatchesPerTick` steps in a tick), and `sim.client` p99 runs to roughly
    //    twice `sim.server`, so a statement about "the frame" that samples only the server's world
    //    covers well under half of it.
    // 3. CO-SAMPLED, not two independent maxima. "106 active islands at some moment" and "an
    //    island of 612 at some moment" describe no single tick, and reading them together is how a
    //    3x parallel ceiling gets inferred from a run that never had one. `max_active_islands` is
    //    kept as a genuine CEILING — the widest the solve ever got — and everything else about the
    //    expensive moment comes from one struct sampled at one instant.
    // 4. And keyed on the TICK, not on one step, because that is the shape the tail actually has.
    //    A tick runs one server step plus up to `kMaxCatchUpBatchesPerTick` client ones, and
    //    `physics.step.per_frame` — the 24 ms that nearly IS the frame — is their SUM. The single
    //    most expensive step in this run is ~9 ms; no per-step maximum can explain a 24 ms tick,
    //    and reading one as if it did is the same error as reading two peaks as one moment.
    struct TickSolve {
        double ms = 0.0; // every physics.step in the tick, both worlds
        std::uint64_t tick = 0;
        std::uint32_t steps = 0;
        std::uint32_t min_active_islands = 0; // the NARROWEST step in the tick
        std::uint32_t max_active_islands = 0;
        std::uint32_t largest_active_island = 0;
        std::uint32_t awake_bodies = 0; // the DENOMINATOR the largest island is a share of
        std::uint32_t parallel_steps = 0;
        // The same two facts in TIME rather than in bodies, and the reason they exist: every field
        // above counts BODIES, so the serial fraction this tick implies has always been a body
        // share -- `largest_active_island / awake_bodies`. Two independent reviews rejected that as
        // a basis for the recorded 1.09x island-parallel ceiling, in the same words: body share is
        // not solver-time share. These are summed and maxed exactly as `ms` and
        // `largest_active_island` are, so `island_ns_largest / island_ns_total` is the tick's REAL
        // Amdahl serial fraction. Zero unless --island-profile was passed.
        std::uint64_t island_ns_total = 0;   // every active island's solve, both worlds
        std::uint64_t island_ns_largest = 0; // the single most expensive island in the tick
    };

    TickSolve tick_solve;       // accumulating, this tick
    TickSolve worst_tick_solve; // the most expensive one so far
    std::uint32_t max_active_islands = 0;

    // Per-world POPULATION, because the split zones raised a question the split zones cannot
    // answer. The two worlds' `physics.solve` costs the same per call (2.744 vs 2.737 ms — the
    // same constraint load, as replication promises), while the client's `physics.contacts` costs
    // 1.25x the server's. Contacts is broadphase + narrowphase, so the difference would have to be
    // in how many bodies each world carries into the pair search, not in how many actually touch.
    // (It was not: the 1.25x is a sampling artifact — see the whole-distribution note in the m17.5
    // roadmap entry. These counters are what ruled the population explanation out.)
    //
    // READ THEM AS BOUNDS, NOT AS AN EQUALITY. They are per-world PEAKS over the run, and two
    // maxima from two different ticks are not a claim about any one tick — the same mistake the
    // island peaks invited one brick ago, and the first draft of this very comment made it again.
    // A large or growing gap means one world is retaining bodies the other reclaimed and is worth
    // chasing; server 762 against client 774 is two peaks landing on different frames.
    std::uint32_t server_max_bodies = 0;
    std::uint32_t client_max_bodies = 0;
    std::uint32_t server_max_pairs = 0;
    std::uint32_t client_max_pairs = 0;

    // One physics step, folded into this tick's total. Called from BOTH worlds — the client
    // re-simulates and the server does not, so a panel that saw only one of them described well
    // under half of the tick it was being used to explain.
    void note_step(double step_ms, const physics::WorldStats& s) {
        tick_solve.ms += step_ms;
        ++tick_solve.steps;
        tick_solve.min_active_islands =
            (tick_solve.steps == 1) ? s.active_islands
                                    : std::min(tick_solve.min_active_islands, s.active_islands);
        tick_solve.max_active_islands = std::max(tick_solve.max_active_islands, s.active_islands);
        tick_solve.largest_active_island =
            std::max(tick_solve.largest_active_island, s.largest_active_island);
        // Without this the largest island is a number with no scale. "One island of 605" is a
        // statement about parallelism only against how many bodies were awake at all: 605 of 1900
        // caps the speedup near 3x, 605 of 650 caps it at nothing, and the two look identical in a
        // report that records only the island.
        tick_solve.awake_bodies = std::max(tick_solve.awake_bodies, s.awake_bodies);
        tick_solve.parallel_steps += (s.islands_solved_parallel > 0) ? 1u : 0u;
        max_active_islands = std::max(max_active_islands, s.active_islands);
        // Summed over the tick's steps like `ms`; maxed like `largest_active_island`. Both are 0
        // when the profile is off, and then the ratio below is simply not reported.
        tick_solve.island_ns_total += s.island_solve_ns_total;
        tick_solve.island_ns_largest =
            std::max(tick_solve.island_ns_largest, s.largest_active_island_solve_ns);
    }

    // The most expensive single SOLVE STEP, and its per-island table -- snapshotted only when a
    // step beats the record, so the measured loop pays a vector copy a handful of times across a
    // run rather than once per step.
    //
    // KEYED ON THE STEP, NOT THE TICK, AND THAT IS THE WHOLE POINT. Amdahl's serial fraction is a
    // property of ONE parallel region, and the parallel region here is one step's island dispatch.
    // A tick runs a server step plus up to kMaxCatchUpBatchesPerTick client ones, SEQUENTIALLY, so
    // a tick-level ratio of (max island over steps) / (summed island time over steps) is not a
    // serial fraction at all -- with two similar steps it lands near 0.5 whatever the solve looks
    // like. The first version of this instrument did exactly that and reported a 2.040x ceiling for
    // a tick whose worst step had ONE active island, i.e. a true ceiling of 1.00x. The per-tick
    // sums below are still recorded, as totals; only the per-step ratio is a bound.
    std::vector<physics::IslandSolveCost> worst_step_islands;
    std::uint64_t worst_step_island_ns = 0;  // that step's total island solve
    std::uint64_t worst_step_largest_ns = 0; // its single most expensive island
    std::uint32_t worst_step_active_islands = 0;
    std::uint32_t worst_step_largest_bodies = 0;
    std::uint32_t worst_step_awake_bodies = 0;
    std::uint64_t worst_step_tick = 0; // the tick_index the record step ran in (absolute)

    void note_island_profile(const physics::PhysicsWorld& w, const physics::WorldStats& s) {
        if (s.island_solve_ns_total <= worst_step_island_ns) {
            return;
        }
        worst_step_island_ns = s.island_solve_ns_total;
        worst_step_largest_ns = s.largest_active_island_solve_ns;
        worst_step_active_islands = s.active_islands;
        worst_step_largest_bodies = s.largest_active_island;
        worst_step_awake_bodies = s.awake_bodies;
        worst_step_tick = tick_index;
        const std::span<const physics::IslandSolveCost> p = w.island_solve_profile_last();
        worst_step_islands.assign(p.begin(), p.end());
    }

    // Steps whose solve actually went through the job system. THE HANDOFF, not the value: a pool
    // reporting 32 workers proves a pool exists, not that anything was ever handed to it, and
    // `set_job_system` is a nullable pointer nobody is obliged to call. These are 0 for the entire
    // run if the wiring is missing, which is the only way the report can tell "parallelism did not
    // help here" from "parallelism was never switched on".
    std::uint64_t server_parallel_steps = 0;
    std::uint64_t client_parallel_steps = 0;
    destruction::BindStats client_bound{};

    // The two halves of a tick, timed apart (m13.p). This demo is a server AND a client in one
    // process, so its combined simulation cost is roughly what TWO machines pay — and gating a
    // player-facing frame budget on that number would be measuring the demo's topology rather than
    // the engine. A player's machine runs the client half.
    double last_server_ms = 0.0;
    double last_client_ms = 0.0;
    double last_join_ms = 0.0; // the client's thread waiting for the server's half (p2-perf)

    // How many destruction batches the client drained in its worst single tick, and how many
    // physics steps that cost it in total. The client's catch-up loop runs a FULL physics step per
    // queued batch (12-networked-destruction's "one batch per update, never two merged" rule, which
    // is right for fidelity) — so if it ever falls behind during a collapse, the cost of catching
    // up is serialised into one frame with no bound on it. This is the counter that says whether
    // that is happening or whether the spike is something else.
    std::size_t max_batches_per_tick = 0;
    std::uint64_t total_client_steps = 0;
    std::uint64_t total_client_batches = 0;
    std::uint64_t shots_fired = 0;
    std::uint64_t shots_hit = 0;
    std::uint64_t damage_ops = 0;
    float max_op_amount = 0.0f;   // largest single damage op; parts stand at 1.0 health
    std::uint64_t lethal_ops = 0; // ops that killed their part outright
    std::uint64_t total_ops = 0;

    // Start the measured window (m17.5). Every running total above accumulates from the first tick
    // of the process, and a `--perf` run steps the whole simulation `--warmup` times before the
    // report's zone collector is even installed — so a ledger read at the end describes a longer
    // run than the distributions beside it, and the two disagree in a way nothing announces.
    //
    // It was wrong by exactly the warmup: `net.client_physics_steps` read 794 against a
    // `physics.step` zone count of 1304 = 600 server + 704 client, and "473 of 600 ticks solved in
    // parallel" was 473 of 690. A denominator that quietly includes warmup makes every ratio in
    // the write-up wrong in the flattering direction, which is the worst kind of wrong.
    //
    // Perf only: the headless proof has no warmup and reads these counters over its whole run.
    void begin_measured_window() {
        tick_solve = {};
        worst_tick_solve = {};
        max_active_islands = 0;
        server_max_bodies = 0;
        client_max_bodies = 0;
        server_max_pairs = 0;
        client_max_pairs = 0;
        server_parallel_steps = 0;
        client_parallel_steps = 0;
        peak_live_debris = 0;
        peak_visual_debris = 0;
        max_batches_per_tick = 0;
        total_client_steps = 0;
        total_client_batches = 0;
        shots_fired = 0;
        shots_hit = 0;
        damage_ops = 0;
        max_op_amount = 0.0f;
        lethal_ops = 0;
        total_ops = 0;
    }

    using HitSurface = block_play::PlayWeaponFx::Surface;
    std::array<HitSurface, 64> shot_surfaces{};

    [[nodiscard]] HitSurface hit_surface(physics::BodyId body, core::Vec3 point) {
        // Classify authored roles, not colors sampled from a rendered frame. Resolve detached
        // debris back to its source slab so a metal lamp stays metal after it falls over.
        destruction::InstanceId source{};
        for (std::size_t i = 0; i < server.destruction.debris_count(); ++i)
            if (server.destruction.debris_body(i) == body) {
                source = server.destruction.debris_source(i);
                break;
            }
        HitSurface surface = HitSurface::Soil; // untagged streamed heightfield
        server.world.query<blockkit::SlabRole>().for_each([&](ecs::Entity e,
                                                              const blockkit::SlabRole& role) {
            const auto* handle = server.world.get<physics::RigidBodyHandle>(e);
            const auto* instance = server.world.get<destruction::DestructibleInstanceRef>(e);
            const bool same_instance =
                instance && ((source.is_valid() && instance->instance == source.index) ||
                             server.destruction.body_of({instance->instance, 0}) == body);
            if (!(handle && handle->body == body) && !same_instance)
                return;
            using namespace blockkit::slab_kind;
            switch (role.kind) {
                case kWall:
                case kHalfWall:
                case kSideWall:
                    surface = role.tint == 1   ? HitSurface::Brick
                              : role.tint == 0 ? HitSurface::Concrete
                                               : HitSurface::Plaster;
                    break;
                case kLampMast:
                case kLampHead:
                    surface = HitSurface::Metal;
                    break;
                case kCrate:
                case kTimber:
                    surface = HitSurface::Wood;
                    break;
                // The playable street's outer strips are dressed as grass/path at |z|>14.4,
                // sharing the same solid slab. This is a cosmetic surface override only.
                case kStreet:
                    surface = std::abs(point.z) > 14.4f ? HitSurface::Soil : HitSurface::Concrete;
                    break;
                default:
                    surface = HitSurface::Concrete;
                    break;
            }
        });
        return surface;
    }

    explicit Session(std::uint64_t seed, bool impaired)
        : network(seed,
                  {impaired ? kLossRate : 0.0f,
                   0.0f,
                   impaired ? kOneWayMs : 0u,
                   impaired ? kOneWayMs : 0u}) {
        // Both peers solve on the SAME job system (m17.5) — see Peer::use_jobs.
        server.use_jobs(jobs);
        client.use_jobs(jobs);
        // …and report their step zones apart. Until this, `physics.step` was one distribution over
        // two simulations: an authoritative server and a client that re-simulates on catch-up and
        // costs roughly twice as much. `sim.client`/`sim.server` already split the same work, so
        // the report carried two decompositions that did not compose — and the merged one is the
        // shape m17.5 would otherwise have gone off and optimised.
        server.physics.set_profile_label("server");
        client.physics.set_profile_label("client");
        // BOTH worlds, because the tick's solve is their sum: a serial fraction taken from the
        // server alone would describe half of the thing the budget is about.
        server.physics.set_island_profile_enabled(g_island_profile);
        client.physics.set_island_profile_enabled(g_island_profile);
    }

    [[nodiscard]] bool start(const std::filesystem::path& cooked,
                             std::string_view scene_path,
                             destruction_render::PartLeafRenderer* leaves,
                             render::MeshRegistry* meshes,
                             std::uint64_t ground_material) {
        // A rifle, not a demolition charge: cooked parts stand at 1.0 health, so the damage number
        // is expressed against that scale and a building takes sustained fire rather than one shot.
        server.weapon.eye_height = block_play::play_eye_height(server.character);
        server.weapon.damage = 0.34f;
        server.weapon.damage_radius = 0.35f;
        server.weapon.cooldown_ticks = server.weapon.automatic ? 6 : 3;

        // Saved city maps do not trigger generation. Inspect only their content identities so
        // both peers register the same procedural patterns before binding strict-loaded entities.
        if (!scene_path.empty()) {
            ecs::World authored;
            register_all(authored);
            const auto loaded = scene::load_scene_file(authored, std::filesystem::path(scene_path));
            if (!loaded.ok) {
                std::fprintf(stderr, "city scene: %s\n", loaded.error.c_str());
                return false;
            }
            bool city_pattern = false;
            authored.query<destruction::Destructible>().for_each([&](ecs::Entity, const auto& d) {
                city_pattern |= d.asset == block_play::city::kLampAsset ||
                                d.asset == block_play::city::kBarrierAsset;
            });
            server.city_patterns = client.city_patterns = city_pattern;
        }

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
        if (!server.stand_up(destruction::Authority::Local, true, scene_path, ground_material) ||
            !client.stand_up(destruction::Authority::Remote, false, scene_path, ground_material)) {
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
        tick_solve = {};
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
        replication::InputCommand command{};
        if (!play_world) {
            command = client.sender.record(intent);
            client.sender.send(*client.driver, now_ms);
        }

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

        if (play_world) {
            // Recovery returns to the SAME place the avatar started: the authored marker when the
            // map has one, otherwise the old west spawn.
            const core::Vec3 spawn = [&] {
                if (server.spawn.status == block_play::SpawnStatus::Valid)
                    return server.spawn.placement.capsule_centre;
                core::Vec3 p = blockkit::west_viewpoint(blockkit::BlockParams{});
                p.y = server.character.half_height + server.character.radius;
                return p;
            }();
            bool recovered = false;
            for (ecs::Entity e : server.avatars) {
                auto* state = server.world.get<gameplay::CharacterState>(e);
                if (!state || !block_play::needs_recovery(state->position))
                    continue;
                state->position = spawn;
                state->velocity = {};
                state->grounded = false;
                if (auto* t = server.world.get<ecs::LocalTransform>(e))
                    t->value.translation = spawn;
                if (auto* t = server.world.get<ecs::WorldTransform>(e))
                    t->value.translation = spawn;
                if (const auto* h = server.world.get<physics::RigidBodyHandle>(e)) {
                    physics::BodyState body;
                    if (server.physics.get_body_state(h->body, body)) {
                        body.position = spawn;
                        body.linear_velocity = {};
                        body.angular_velocity = {};
                        (void)server.physics.set_body_state(h->body, body);
                    }
                }
                recovered = true;
            }
            if (recovered) {
                play_world->note_recovery();
                const ecs::Entity e = client.local_player();
                if (auto* state =
                        e.is_valid() ? client.world.get<gameplay::CharacterState>(e) : nullptr) {
                    state->position = spawn;
                    state->velocity = {};
                    state->grounded = false;
                    client.predictor.reset(*state);
                    gameplay::write_character_pose(client.world, e, spawn);
                }
            }
            core::Vec3 server_position = spawn;
            if (!server.avatars.empty()) {
                if (const auto* s =
                        server.world.get<gameplay::CharacterState>(server.avatars.front())) {
                    server_position = s->position;
                }
            }
            core::Vec3 client_position = client.eye_anchor();
            if (!client.local_player().is_valid())
                client_position = spawn;
            if (block_play::needs_recovery(client_position)) {
                const ecs::Entity e = client.local_player();
                if (auto* state =
                        e.is_valid() ? client.world.get<gameplay::CharacterState>(e) : nullptr) {
                    *state = gameplay::CharacterState{};
                    state->position = spawn;
                    client.predictor.reset(*state);
                    gameplay::write_character_pose(client.world, e, spawn);
                }
                client_position = spawn;
                play_world->note_recovery();
            }
            // Both collision worlds are mutated here on the main thread, before the server solve
            // job is launched and before either gameplay or prediction reads physics queries.
            if (!play_world->prepare(
                    server.world,
                    server.destruction,
                    client.world,
                    client.destruction,
                    tick_index,
                    server_position,
                    client_position,
                    std::max(server.character.max_speed, client.config.max_speed))) {
                world_stopped = true;
                std::fprintf(stderr,
                             "99-the-block: procedural collision barrier refused tick %llu\n",
                             static_cast<unsigned long long>(tick_index));
                return;
            }
            // A refused barrier must not consume this input or advance the prediction tape.
            command = client.sender.record(intent);
            client.sender.send(*client.driver, now_ms);
        }

        // ── THE TWO HALVES RUN AT ONCE (p2-perf, ADR-0077) ──
        //
        // On one machine this demo is two machines: a server and a client, each with its own
        // world, physics, destruction and replication state. The tick ran them in series, so
        // `sim.block` was the SUM of two physics steps (~7.6 ms p99 apiece on the RTX 3060 box)
        // when a real deployment pays only one of them per machine. From here to the join the two
        // halves touch disjoint state — the network is read above and written below, never in
        // between — so the server's half runs as ONE job while this thread runs the client's.
        //
        // Why a job rather than a thread: `JobSystem` may be submitted to from inside a running
        // job (its threading contract), so the server's physics keeps its parallel solve through
        // the same pool, and no second pool puts ~62 workers on 32 cores (the m17.5 lesson).
        // Each world's step is deterministic regardless of which or how many workers run it —
        // ADR-0026's witnesses — so overlapping them changes when the work happens, not its
        // result.
        //
        // What must NOT cross the join: the server's per-step bookkeeping (`note_step` writes the
        // shared `tick_solve`) is captured here and noted after it, in the order it always was
        // — client first, then server. The server's profile zones close on a worker, so the perf
        // loop collects them after this returns (`ZoneTimelines::collect_parked`).
        double server_step_ms = 0.0;
        physics::WorldStats server_step_stats{};
        core::JobSystem::Counter server_done{0};
        jobs.run(
            [&] {
                // ── The server's simulation ──
                const core::Stopwatch server_watch;
                server.world.advance_version();
                server.gameplay.consume(server.world, server.physics, server.input, kDt);
                ecs::propagate_transforms(server.world, jobs);
                server.sync.reconcile(server.world, server.physics);
                server.sync.push_in(server.world, server.physics, kDt);
                const core::Stopwatch server_step_watch;
                server.physics.step(kDt);
                {
                    const physics::WorldStats ss = server.physics.stats();
                    server_step_ms = server_step_watch.elapsed_ms();
                    server_step_stats = ss;
                    server_max_bodies = std::max(server_max_bodies, ss.body_count);
                    server_max_pairs = std::max(server_max_pairs, ss.broadphase_pairs);
                }
                server.sync.write_back(server.world, server.physics);

                // The weapon → destruction glue: the consumer's job, kept out of the engine so that
                // `gameplay_net` never links `destruction` (ADR-0035 §3).
                std::size_t surface_index = 0;
                for (const gameplay_net::ShotEvent& shot : server.gameplay.shots()) {
                    if (surface_index < shot_surfaces.size())
                        shot_surfaces[surface_index++] = shot.did_hit
                                                             ? hit_surface(shot.body, shot.point)
                                                             : HitSurface::Concrete;
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
                    if (!instance.is_valid() ||
                        server.destruction.part_from_child(instance, shot.child) ==
                            destruction::kInvalidPartIndex) {
                        push_loose_hit(server.physics, shot.body, shot.point, shot.impulse);
                        continue; // static street is a no-op; live rubble wakes and moves
                    }
                    server.destruction.apply_damage(
                        instance, shot.point, shot.damage_radius, shot.damage, shot.impulse);
                    ++damage_ops;
                }
                if (building_support != nullptr) {
                    building_support->step(server.destruction);
                }
                server.destruction.update(server.physics);
                last_server_ms = server_watch.elapsed_ms();
            },
            &server_done);

        const core::Stopwatch client_watch;
        ecs::propagate_transforms(client.world, jobs);
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

        // ONE DESTRUCTION UPDATE PER BATCH, AND ONE PHYSICS STEP PER TICK (m17.5).
        //
        // The fracture boundary that must not be skipped is the `update()`, not the `step()`, and
        // that is measured rather than argued: `tests/destruction_net` "the fracture boundary is
        // the destruction update, not the physics step" feeds two mirrors the identical pair of
        // remote batches, one with a step between them and one without, and their composition
        // hashes and debris rosters are equal — while merging both batches into a SINGLE update
        // still hashes differently, which is the divergence ADR-0033 A12 actually names. So the
        // boundary is preserved here and the second step is not.
        //
        // The step is the WALL CLOCK's, so it belongs to the tick and not to the batch. Taking one
        // per batch also ran the client's physics permanently ahead of the server's — 704 steps
        // against 600 over the measured window, 1.73 s of extra simulated flight by the end.
        //
        // WHY THIS IS SAFE ONLY FOR REMOTE OPS, because it is not safe in general. A `DamageOp` off
        // the wire carries an already-resolved `part` index. LOCAL damage carries a world POINT
        // that `update()` resolves against the current pose, so for a Local instance a step between
        // the blast and the update genuinely changes which parts are hit — the first version of the
        // test above measured exactly that and came out at 13 chunks against 14. Every instance on
        // this client is bound Remote, and a Remote instance refuses local damage outright.
        //
        // What it is worth: the frames that set this demo's p99 ran one server step plus TWO client
        // steps of ~8 ms each. Now they run one apiece.
        //
        // BOUNDED CATCH-UP still applies (m13.p). Before the cap this demo drained every queued
        // batch in one tick — 10 physics steps in a single tick and `sim.client` p99 = 58 ms — and
        // deferring is still not merging: batches stay queued and are applied one at a time, in
        // order, with their boundaries intact. That it converges is not assumed; the headless proof
        // drains to quiescence with a BOUND and fails if the peers never agree.
        std::size_t batches_this_tick = 0;
        do {
            (void)client.destruction_net_client.apply_next_batch(
                client.world, client.replicator->map(), client.destruction);
            if (batches_this_tick == 0) {
                // The tick's one step, in exactly the place it has always been — so a tick that
                // drains a single batch, which is 496 of 600 here, is the sequence it always was.
                const core::Stopwatch client_step_watch;
                client.physics.step(kDt);
                const physics::WorldStats cs = client.physics.stats();
                note_step(client_step_watch.elapsed_ms(), cs);
                if (g_island_profile) {
                    note_island_profile(client.physics, cs);
                }
                client_parallel_steps += (cs.islands_solved_parallel > 0) ? 1u : 0u;
                client_max_bodies = std::max(client_max_bodies, cs.body_count);
                client_max_pairs = std::max(client_max_pairs, cs.broadphase_pairs);
            }
            client.destruction.update(client.physics);
            ++batches_this_tick;
        } while (client.destruction_net_client.pending_batches() > 0 &&
                 batches_this_tick < kMaxCatchUpBatchesPerTick);
        max_batches_per_tick = std::max(max_batches_per_tick, batches_this_tick);
        // Steps and batches STOPPED BEING THE SAME NUMBER at m17.5, so they stopped sharing a
        // counter. `total_client_steps += batches_this_tick` was true only while the loop took a
        // step per batch, and left uncorrected it would have reported the saving as though it had
        // never happened — the ledger claiming 704 steps for a client that now takes 600.
        total_client_steps += 1; // exactly one per tick: the wall clock's
        total_client_batches += batches_this_tick;
        client.sync.write_back(client.world, client.physics);
        last_client_ms = client_watch.elapsed_ms();
        {
            // What the client's thread still waits for once its own half is done: the part of
            // the server's half that did not fit under the client's. `sim.join` is what keeps the
            // `sim.block` accounting a real nesting — client half, then this, in series.
            const core::Stopwatch join_watch;
            jobs.wait(server_done);
            last_join_ms = join_watch.elapsed_ms();
        }
        note_step(server_step_ms, server_step_stats);
        if (g_island_profile) {
            note_island_profile(server.physics, server_step_stats);
        }

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
        server_parallel_steps += (server.physics.stats().islands_solved_parallel > 0) ? 1u : 0u;
        tick_solve.tick = tick_index;
        if (tick_solve.ms > worst_tick_solve.ms) {
            worst_tick_solve = tick_solve;
        }
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

render::ExtractedCamera optical_camera(const core::Mat4& optic_world) {
    render::ExtractedCamera camera{};
    camera.found = true;
    camera.view = core::inverse(optic_world);
    const auto position = core::transform_point(optic_world, {});
    camera.position[0] = position.x;
    camera.position[1] = position.y;
    camera.position[2] = position.z;
    // Feed FOV accounts for the physical aperture and eye relief, not just parent focal ratio.
    camera.fov_y = block_play::PlayViewmodel::scope_fov_y();
    camera.z_near = .04f;
    camera.z_far = 3072.0f;
    return camera;
}

// ── The visuals ─────────────────────────────────────────────────────────────────────────────────
//
// Owned separately from the session because they need a DEVICE and the simulation does not. A host
// with no Vulkan still runs every sim assertion in --headless; only the render ones are skipped.
struct Visuals {
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    destruction_render::PartLeafRenderer leaves;
    block_play::AuthoredEnvironment authored_environment;
    render::SceneRenderer renderer;
    std::unique_ptr<render::SceneRenderer> scope_renderer;
    render::text::HudRenderer hud;
    blockkit::BlockPalette palette;
    ecs::Entity camera = ecs::kNullEntity;
    // The pose the SCENE authored for its camera, kept because the demo overwrites the camera
    // entity with the player's eye every frame. See `use_authored_camera`.
    core::Transform authored_camera{};
    render::RGTexture last_ldr{};
    render::RGTexture last_scope_ldr{};
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
        : meshes(device), authored_environment(&device), renderer(device, meshes, materials),
          hud(device, render::kLdrFormat) {
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

        // The sun needs local detail even while the terrain camera sees several kilometres.
        // Spend the three maps on the nearby city instead of stretching the first over 500 m.
        lighting.shadows_enabled = true;
        lighting.cascade_count = 3;
        lighting.shadow_map_resolution = 2048;
        lighting.shadow_max_distance = 160.0f;

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

        // The sky (m17.7b). Until now nothing in any sample ever created one, so the milestone's
        // sky had never been seen outside a test — and the block, which is what "The Visual Bar" is
        // judged by looking at, was lit by a single hand-picked constant.
        //
        // This does two things at once and the second is the one to know about: it paints a dusk
        // sky ABOVE the horizon, and its physical solar source becomes what LIGHTS the street.
        // `set_ambient` above is now the sky-less fallback. m17.7d deliberately retired the
        // analytic gradient's old ambient-match calibration; palette.hpp records that this exposure
        // now needs a physical visual-bar measurement rather than a copied number.
        //
        // `use_scene_sun` is left ON, so the disc follows the authored DirectionalLight rather than
        // being pointed a second time. ADR-0040 §4: two authored sun directions that drift apart is
        // a bug class closed by construction.
        renderer.set_sky(authored_sky());
    }

    // The block's sky, in one place so the headless A/B below can put back exactly what it took
    // away. palette.hpp records which controls remain background art and which feed physical light.
    [[nodiscard]] static render::SkyParams authored_sky() {
        render::SkyParams sky{};
        sky.enabled = true;
        for (int i = 0; i < 3; ++i) {
            sky.zenith[i] = blockkit::kSkyZenith[i];
            sky.horizon[i] = blockkit::kSkyHorizon[i];
        }
        sky.intensity = blockkit::kSkyIntensity;
        sky.clouds_enabled = true;
        sky.coverage = blockkit::kSkyCloudCoverage;
        return sky;
    }

    // Both cameras must trace the same authored lighting proxies.
    void seed_field(render::SceneRenderer& target, ecs::World& world) {
        // The field the DDGI probes trace (m13.L). Ids 1..N are the buildings, 0 is the street.
        //
        // SIZED FROM THE GROUND ITSELF since m17.8, and it was the third of three disagreeing
        // extents: this proxy used to compute its own half from `BlockParams` and came out at
        // 26 x 14 against a surface drawn at 38 x 38, so the probes lit a street a third the size
        // of the one on screen. Now it asks the surface, like everything else that needs to know
        // how big the ground is.
        const blockkit::BlockParams p;
        core::Vec3 street_half{
            p.street_length() * 0.5f + p.building_gap, 0.25f, p.street_width * 0.5f + p.footprint};
        core::Vec3 street_at{p.street_length() * 0.5f, -0.25f, 0.0f};
        world.query<ground::GroundSurface>().for_each(
            [&](ecs::Entity e, ground::GroundSurface& surface) {
                const core::Vec3 half = ground::half_extents(surface);
                street_half = {half.x, 0.25f, half.z};
                const ecs::WorldTransform* wt = world.get<ecs::WorldTransform>(e);
                const ecs::LocalTransform* lt = world.get<ecs::LocalTransform>(e);
                const core::Transform placement = wt != nullptr   ? wt->value
                                                  : lt != nullptr ? lt->value
                                                                  : core::Transform{};
                street_at = {placement.translation.x, -0.25f, placement.translation.z};
            });
        target.sdf_clipmap().update_instance(
            0, build_box_sdf(street_half, 24), core::mat4_translation(street_at));
        const core::Vec3 building_half{p.footprint * 0.5f,
                                       static_cast<float>(p.storeys) * p.storey_height * 0.5f,
                                       p.footprint * 0.5f};
        const assets::MeshSdfAsset building_sdf = build_box_sdf(building_half, 16);
        for (std::uint32_t b = 0; b < p.building_count(); ++b) {
            // One baked grid, eight placements: every building is the same box, so re-baking it per
            // instance would be eight identical voxel grids and eight times the cook.
            target.sdf_clipmap().update_instance(
                b + 1, building_sdf, core::mat4_translation(building_centre(b, p)));
        }
    }

    // Derive the look from authored roles and find the block camera.
    void dress(ecs::World& world) {
        palette = blockkit::build_palette(materials);
        blockkit::upload_prop_meshes(palette, meshes);
        (void)blockkit::apply_palette(world, palette);
        // The drawn ground, derived from the same surface the collider came from (m17.8). After
        // the palette, which has already given the street its material by role — `apply_ground`
        // supplies the mesh and leaves an existing material alone.
        (void)ground::apply_ground(world, meshes, palette.street);

        seed_field(renderer, world);

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
constexpr audio::SoundId kGunReport = audio::sound::kUserBase;
constexpr audio::SoundId kBulletImpact = kGunReport + 3;
// One id past the last impact material: the empty-magazine click.
constexpr audio::SoundId kDryClick =
    kBulletImpact + static_cast<unsigned>(block_play::PlayWeaponFx::Surface::Count);

audio::SoundBank play_sound_bank() {
    auto bank = audio::SoundBank::engine_defaults();
    // Subtractive transient synthesis: filtered deterministic noise supplies the report's crack,
    // a short low sine supplies its body, and exponential envelopes prevent discontinuous tails.
    // Build these mono buffers once; the existing mixer spatializes bounded one-shot voices.
    for (unsigned weapon = 0; weapon < 3; ++weapon) {
        auto report = audio::noise_burst(.24f, .72f, weapon == 2 ? 23.0f : 32.0f, 0x706u + weapon);
        audio::mix_into(report, audio::decaying_sine(.24f, weapon == 2 ? 85.0f : 130.0f, 35), .55f);
        bank.set(kGunReport + weapon, std::move(report));
    }
    for (unsigned material = 0;
         material < static_cast<unsigned>(block_play::PlayWeaponFx::Surface::Count);
         ++material) {
        const bool metal =
            material == static_cast<unsigned>(block_play::PlayWeaponFx::Surface::Metal);
        const bool water =
            material == static_cast<unsigned>(block_play::PlayWeaponFx::Surface::Water);
        auto impact = audio::noise_burst(.18f,
                                         metal   ? .85f
                                         : water ? .08f
                                                 : .24f,
                                         water ? 20.0f : 45.0f,
                                         0x173u + material);
        if (metal)
            audio::mix_into(impact, audio::decaying_sine(.18f, 2300, 28), .35f);
        bank.set(kBulletImpact + material, std::move(impact));
    }
    // Dry click: a very short bright noise tick plus a high, fast-dying sine -- the hammer
    // falling on an empty chamber. Built once like the other one-shots.
    auto click = audio::noise_burst(.05f, .6f, 90.0f, 0x2c1u);
    audio::mix_into(click, audio::decaying_sine(.05f, 1800, 120), .35f);
    bank.set(kDryClick, std::move(click));
    return bank;
}

class PlayAudioOutput {
public:
    static constexpr std::size_t kSamples = (audio::kSampleRate / 60) * 2;

    ~PlayAudioOutput() {
        if (worker_.joinable()) {
            worker_.request_stop();
            ready_.notify_one();
            worker_.join();
            std::printf("play audio: %llu blocks delivered, %llu dropped, %llu underruns\n",
                        static_cast<unsigned long long>(delivered_),
                        static_cast<unsigned long long>(dropped_),
                        static_cast<unsigned long long>(sink_->underruns()));
        }
    }

    void start() {
        sink_ = audio::open_audio_sink(audio::kSampleRate);
        if (!sink_) {
            std::printf("play audio: no output device available; mixer remains active\n");
            return;
        }
        active_ = true;
        std::printf("play audio: %s output enabled\n", sink_->backend_name());
        // Producer/consumer queue: AudioSink::write is a blocking device-clock operation. Keep
        // it off the simulation thread, with at most three ticks of latency. On overload discard
        // the oldest queued block so current gunfire never trails a long backlog of rubble noise.
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::array<float, kSamples> block{};
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [&] { return count_ != 0 || stop.stop_requested(); });
                    if (stop.stop_requested())
                        break;
                    block = queue_[read_];
                    read_ = (read_ + 1) % queue_.size();
                    --count_;
                }
                if (!sink_->write(block)) {
                    std::lock_guard lock(mutex_);
                    active_ = false;
                    std::fprintf(stderr, "play audio: device write failed; output disabled\n");
                    break;
                }
                ++delivered_;
            }
        });
    }

    void submit(std::span<const float> samples) {
        std::lock_guard lock(mutex_);
        if (!active_ || samples.size() != kSamples)
            return;
        if (count_ == queue_.size()) {
            read_ = (read_ + 1) % queue_.size();
            --count_;
            ++dropped_;
        }
        auto& destination = queue_[(read_ + count_) % queue_.size()];
        std::copy(samples.begin(), samples.end(), destination.begin());
        ++count_;
        ready_.notify_one();
    }

private:
    std::unique_ptr<audio::AudioSink> sink_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<std::array<float, kSamples>, 3> queue_{};
    std::size_t read_ = 0, count_ = 0;
    std::uint64_t delivered_ = 0, dropped_ = 0;
    bool active_ = false;
    std::jthread worker_;
};

struct Demo {
    app::Application app;
    Session session;
    std::unique_ptr<block_play::PlayEnvironment> environment;
    std::unique_ptr<block_play::PlayEffects> effects;
    std::vector<render::GpuParticle> smoke_particles;
    ecs::Entity fire_light = ecs::kNullEntity;
    bool effects_visible = true;
    block_play::city::Dresser city_dresser; // per-part lamp/barrier look, fed by sync() each tick
    std::unique_ptr<Visuals> visuals;
    std::unique_ptr<block_play::PlayWorld> play_world;
    std::unique_ptr<block_play::PlayBuildingSupport> building_support;
    bool building_support_enabled = false; // set by run_play only
    std::unique_ptr<block_play::PlayViewmodel> viewmodel;
    std::unique_ptr<block_play::PlayWeaponFx> weapon_fx;
    bool shot_presented = false;
    std::array<std::uint32_t, 3> weapon_rounds{};
    std::vector<render::GpuParticle> combined_weapon_smoke;
    bool streamed_world = false;
    bool environment_enabled = false; // the playable art layer, also exercised without weapon art
    bool development_hud = true;
    bool player_help = false;
    bool viewmodel_enabled = false;  // explicit --play/--viewmodel-smoke only
    float scope_transmission = 1.0f; // real optical-input sensitivity control in the art proof
    std::uint64_t scope_feed_frames = 0;

    // ── The cooked catalog (m17.8b) ─────────────────────────────────────────────────────────────
    // Held HERE, not in `Visuals`, because `GpuAssetBridge::set_catalog` keeps a REFERENCE to the
    // manifest: it has to outlive every resolve, and a temporary parsed inside `start` would leave
    // the bridge reading freed memory the moment the first texture landed.
    //
    // All three are optional on purpose. This sample loads its `.rdest` patterns by FILENAME and
    // has never needed an asset runtime; the ground's material is the first thing in it that comes
    // through M16's asset path, and a run whose cook has not happened must still work — it simply
    // keeps the palette's flat street, exactly as it looked before this brick.
    std::optional<assets::Manifest> catalog;
    std::unique_ptr<assets::AssetServer> asset_server;
    std::unique_ptr<render::GpuAssetBridge> bridge;

    // `settle`'s own contract: a caller handed back `max_rounds` "should treat it as 'did not
    // converge' and look at the pending counters, NOT as success". Named so the check below and the
    // call site cannot drift apart — comparing `rounds >= 8` against a literal 8 elsewhere is
    // exactly how that check stops meaning anything.
    static constexpr std::size_t kSettleRounds = 8;

    // m13.4 measured 0.35 against 10-destructible-wall's 43 voices from 18 events. A collapsing
    // city block is an order of magnitude past that — nearly a thousand events — and at 0.35 the
    // mixdown peaks at 1.05 and clips. The gain is a property of the SCENE's density, not a
    // constant of the engine, so it is set here where the density is known.
    static audio::MixerConfig block_mix() {
        audio::MixerConfig c;
        c.master_gain = 0.12f;
        return c;
    }

    audio::Mixer mixer{play_sound_bank(), block_mix()};
    PlayAudioOutput audio_output;
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

    // Whether step_sim fires the charge itself at kChargeAt. Headless and perf keep the scripted
    // beat; --play turns it off, because an interactive player is the one who decides when the
    // block goes (F calls fire_charge()).
    bool scripted_charge = true;

    // Proof/perf sessions explicitly retain the deterministic impaired wire. Interactive play
    // opts in with --network-impairment; removing fake WAN delay does not bypass the protocol.
    explicit Demo(const app::AppConfig& cfg, bool impaired = true)
        : app(cfg), session(0x13B10Cull, impaired) {}

    [[nodiscard]] bool start(const std::filesystem::path& cooked, std::string_view scene_path) {
        // The city is the PLAYABLE street: the environment layer on a generated scene. Headless,
        // perf and the authored `--scene` path never set it, so they stay byte-identical.
        session.server.city = session.client.city = environment_enabled && scene_path.empty();
        if (app.device() != nullptr) {
            visuals = std::make_unique<Visuals>(*app.device());
        }
        // THE STREET'S COOKED MATERIAL, if `rime ground` has run into the cooked directory. The
        // id is the hash of the cooked bytes, so only the manifest can say what it is — which is
        // why it is read here and handed to the assembly rather than being a constant anywhere.
        std::uint64_t ground_material = 0;
        if (const auto text = platform::read_file(cooked / "manifest.txt")) {
            catalog = assets::Manifest::parse(
                std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
            if (catalog) {
                ground_material = blockkit::ground_material_id(*catalog);
            }
        }

        if (!session.start(cooked,
                           scene_path,
                           visuals ? &visuals->leaves : nullptr,
                           visuals ? &visuals->meshes : nullptr,
                           ground_material)) {
            return false;
        }
        if (streamed_world) {
            play_world = std::make_unique<block_play::PlayWorld>(
                session.server.world, session.server.physics, session.client.physics, app.device());
            if (!play_world->valid()) {
                std::fprintf(stderr, "99-the-block: invalid street footprint or terrain pin\n");
                return false;
            }
            session.play_world = play_world.get();
            const core::Vec3 spawn = session.server.spawn.status == block_play::SpawnStatus::Valid
                                         ? session.server.spawn.placement.capsule_centre
                                         : blockkit::west_viewpoint(blockkit::BlockParams{});
            if (!play_world->prepare(session.server.world,
                                     session.server.destruction,
                                     session.client.world,
                                     session.client.destruction,
                                     0,
                                     spawn,
                                     spawn,
                                     session.server.character.max_speed)) {
                std::fprintf(stderr, "99-the-block: spawn terrain could not be installed\n");
                return false;
            }
        }
        if (building_support_enabled) {
            building_support = std::make_unique<block_play::PlayBuildingSupport>();
            const bool loaded = building_support->register_cooked_dir(cooked);
            const bool built =
                loaded && building_support->build(session.server.world, session.server.destruction);
            const auto& bs = building_support->stats();
            std::fprintf(stderr,
                         "99-the-block: building support: loaded=%d all_supported=%d buildings=%u "
                         "quarantined=%u nodes=%u roots=%u bond_edges=%u contact_edges=%u "
                         "unreachable=%u missing_geometry=%u\n",
                         loaded ? 1 : 0,
                         built ? 1 : 0,
                         bs.buildings,
                         bs.quarantined_buildings,
                         bs.nodes,
                         bs.roots,
                         bs.bond_edges,
                         bs.contact_edges,
                         bs.initial_unreachable_parts,
                         bs.missing_geometry);
            session.building_support = building_support.get();
        }
        if (visuals) {
            visuals->dress(session.client.world);
            if (session.client.city) {
                city_dresser.init(visuals->palette, visuals->materials, visuals->meshes);
            }
            if (visuals->camera == ecs::kNullEntity) {
                std::fprintf(stderr, "99-the-block: the block's scene carries no camera\n");
                return false;
            }

            // AFTER `dress`, which stamps the palette's flat street on everything with a role.
            // The bridge then overrules it on the one entity that named a cooked material, which
            // is the ordering `apply_palette`'s "authored outranks derived" rule exists to make
            // safe — the palette re-runs on every tick that binds new destructibles, and it must
            // leave that entity alone or the ground would flicker between the two looks.
            // GATED ON WHAT THE WORLD ASKS FOR, not on what the cook offers.
            //
            // The first version of this gated on `catalog && ground_material != 0` — i.e. on the
            // manifest existing — and then treated "nothing resolved" as fatal. That is wrong for
            // every scene that simply does not name a cooked material: `rime-blockgen` without
            // `--assets`, any scene authored before this brick, any editor save of one. Once a tree
            // has run `ctest -R block_demo_cook` the manifest always exists, so `--scene` on an
            // ordinary block failed at the door — including in `scripts/authoring-round-trip.sh`,
            // which CI runs. The cook's presence says nothing about whether this scene wants it.
            bool scene_names_a_material = false;
            session.client.world.query<render::MaterialAsset>().for_each(
                [&](ecs::Entity, render::MaterialAsset& a) {
                    scene_names_a_material = scene_names_a_material || a.asset != 0;
                });
            if (scene_names_a_material && !catalog) {
                // The scene ASKED for a cooked material and there is no manifest to answer with.
                // In `--perf` this is the deliberate flat arm of the A/B, where the ledger's
                // `ground.materials_bound` records it as 0 — but everywhere else it is a cook that
                // did not happen, and it used to pass in total silence: one claim fewer and no line
                // of output. A skip path with no counter is a skip nobody can see.
                std::fprintf(stderr,
                             "99-the-block: the scene names a cooked material but no manifest was "
                             "found in %s — the street keeps the palette's flat look\n",
                             cooked.string().c_str());
            }
            if (catalog && scene_names_a_material) {
                asset_server = std::make_unique<assets::AssetServer>(session.jobs);
                bridge = std::make_unique<render::GpuAssetBridge>(*app.device(), *asset_server);
                bridge->set_mesh_sink(visuals->meshes, visuals->materials);
                bridge->set_catalog(*catalog, cooked);
                // Settle now, before any measured frame: the chain is asynchronous and the cost of
                // waiting for it belongs to start-up, not to the frame-time distribution.
                const std::size_t rounds = bridge->settle(session.client.world, kSettleRounds);
                const auto& st = bridge->material_asset_stats();

                // THE ORDER OF THESE THREE CHECKS MATTERS, and it was wrong. A device without BC
                // support refuses every block-compressed upload; the material is left holding the
                // placeholder, so `settle` never reaches quiescence — which means the generic "did
                // not settle" check would fire first and return, and this specific, actionable
                // diagnosis (the one ADR-0039 asked for by name) could never print in the single
                // situation it was written for. Most specific explanation first.
                if (bridge->textures_refused_unsupported() != 0) {
                    std::fprintf(stderr,
                                 "99-the-block: this adapter cannot sample the block-compressed "
                                 "ground cook (%zu textures refused) — re-cook without --bc\n",
                                 bridge->textures_refused_unsupported());
                    return false;
                }
                // The scene NAMED a cooked material and nothing ended up wearing it: a broken
                // pipeline rather than a missing one, and it would otherwise show up only as a
                // street that looks a bit flat.
                //
                // `unresolved_count()`, NOT `st.unresolved`. The stats are per-CALL: an id the
                // manifest does not know is counted on the round `build_material` runs, but that
                // material is cached afterwards, and every later round takes the cache-hit path and
                // reports `steady` with `unresolved == 0`. Reading the LAST round's stats therefore
                // says "nothing unresolved" about a material permanently missing a texture slot —
                // it draws on the shader's 1x1 fallback and start-up says nothing. The bridge's
                // `unresolved_count()` is the deduplicated set that never clears, and exists (as
                // its own comment says) so "which asset did you not find" stays answerable later.
                if (st.resolved + st.steady == 0 || bridge->unresolved_count() != 0) {
                    std::fprintf(
                        stderr,
                        "99-the-block: the scene's cooked ground material resolved onto no "
                        "entity (%zu rounds, %zu unresolved ids, %zu unresolved this "
                        "round)\n",
                        rounds,
                        bridge->unresolved_count(),
                        st.unresolved);
                    return false;
                }
                // `settle` returning `max_rounds` means it never reached quiescence, which its own
                // contract says to treat as "did not converge" rather than as success.
                if (rounds >= kSettleRounds || st.pending != 0) {
                    std::fprintf(stderr,
                                 "99-the-block: the cooked ground material did not settle (%zu "
                                 "rounds, %zu still pending)\n",
                                 rounds,
                                 st.pending);
                    return false;
                }
            }
        }
        if (environment_enabled && visuals && play_world) {
            environment = std::make_unique<block_play::PlayEnvironment>(*app.device());
            if (!environment->build(session.client.world,
                                    visuals->meshes,
                                    visuals->materials,
                                    visuals->palette,
                                    play_world->config())) {
                std::fprintf(stderr, "99-the-block: environment art generation failed\n");
                return false;
            }
            play_world->set_surface_palette(environment->terrain_palette());
            auto lighting = visuals->renderer.lighting();
            lighting.ddgi_volume_fade = true;
            lighting.ddgi_stable_rays = true;
            // 28 m wide so the finite support still reaches the 12 m street's facades. All 512
            // probes fit the existing per-frame budget; 128 fixed rays reduce quadrature bias.
            lighting.ddgi_probe_count_y = 8;
            lighting.ddgi_probe_spacing = 4;
            lighting.ddgi_rays_per_probe = 128;
            visuals->renderer.set_lighting(lighting);
        }
        if (environment_enabled && visuals) {
            combined_weapon_smoke.reserve(block_play::AuthoredEnvironment::smoke_capacity +
                                          block_play::PlayWeaponFx::kDustSlots);
            weapon_fx = std::make_unique<block_play::PlayWeaponFx>(
                *app.device(), visuals->meshes, visuals->materials);
            if (!weapon_fx->initialize())
                return false;
        }
        if (g_effects_preview && environment_enabled && visuals) {
            // Scripted presentation sources, deliberately opt-in while M18 is over budget.
            // Their particles and light never decide damage, ignition or gameplay visibility.
            effects = std::make_unique<block_play::PlayEffects>(core::Vec3{4, .25f, -1.5f});
            render::PointLight light{};
            light.intensity = 0;
            fire_light = session.client.world.spawn_with(ecs::WorldTransform{}, light);
            smoke_particles.reserve(block_play::PlayEffects::kParticleCapacity);
        }
        if (viewmodel_enabled) {
            if (!visuals)
                return false;
            // The held weapon art is third-party CC-BY work that deliberately does not live in
            // this repository (assets/viewmodel/README.md says how to fetch and prepare it), so a
            // clean clone simply has no cook to load. That is a normal state, not a defect: drop
            // the hands and say why. Art that IS present but fails to load is the opposite case --
            // a real fault -- and stays fatal below, so the two can never be confused.
            if (!std::filesystem::exists(g_viewmodel_content / "manifest.txt")) {
                std::fprintf(stderr,
                             "99-the-block: no cooked viewmodel under %s -- running without the "
                             "held weapon (see samples/99-the-block/assets/viewmodel/README.md)\n",
                             g_viewmodel_content.string().c_str());
                viewmodel_enabled = false;
                return true;
            }
            viewmodel = std::make_unique<block_play::PlayViewmodel>();
            if (!viewmodel->load(g_viewmodel_content,
                                 session.jobs,
                                 *app.device(),
                                 visuals->meshes,
                                 visuals->materials)) {
                return false;
            }
        }
        return true;
    }

    void ensure_scope_renderer() {
        if (!visuals->scope_renderer) {
            visuals->scope_renderer = std::make_unique<render::SceneRenderer>(
                *app.device(), visuals->meshes, visuals->materials);
            visuals->scope_renderer->set_frames_in_flight(app.frames_in_flight());
            visuals->seed_field(*visuals->scope_renderer, session.client.world);
            if (effects) {
                render::FxSettings settings{};
                settings.enabled = effects_visible;
                settings.max_particles = block_play::PlayEffects::kParticleCapacity;
                visuals->scope_renderer->set_smoke_particles(smoke_particles, settings);
            }
            visuals->authored_environment.apply_to(*visuals->scope_renderer);
        }
    }

    bool warm_scope_view() {
        // Loading work belongs before interactive input. Use the SAME graph cache/rings as play,
        // with blocking submissions before any presented frame exists. A throwaway graph would
        // free its scratch textures and leave the first aim paying their allocation cost again.
        const auto eye = session.client.camera_transform();
        if (play_world && !play_world->recenter(eye.translation))
            return false;
        ensure_scope_renderer();
        auto& optical = *visuals->scope_renderer;
        auto scope_lighting = visuals->renderer.lighting();
        scope_lighting.shadow_caster_culling = true;
        optical.set_lighting(scope_lighting);
        optical.set_sky(visuals->renderer.sky());
        const bool taa = optical.taa_resolve_enabled();
        optical.set_taa_resolve_enabled(false);
        const auto camera = optical_camera(
            core::to_matrix(eye) *
            core::mat4_translation({0, 0, -block_play::PlayViewmodel::kScopeEyeRelief}));
        auto& graph = *app.graph();
        for (unsigned i = 0; i < 3; ++i) {
            graph.reset();
            if (play_world)
                optical.set_terrain_draws(play_world->pass(), play_world->draws());
            const auto output =
                optical.render(graph, session.client.world, {512, 512}, true, &camera);
            graph.export_texture(output.hdr);
            graph.export_texture(output.ldr);
            auto commands = app.device()->begin_commands();
            graph.execute(*commands);
            app.device()->submit_blocking(*commands);
            if (play_world)
                play_world->mark_drawn();
        }
        // Warm resources, not a stale aiming image: first actual ADS starts with no TAA history.
        optical.set_taa_resolve_enabled(taa);
        return true;
    }

    bool water_intersection(core::Vec3 origin, core::Vec3 direction, core::Vec3& hit) {
        if (!environment || !visuals)
            return false;
        bool found = false;
        float nearest = core::length(hit - origin);
        // Cosmetic water sheets have no solid collider. Intersect their local plane before
        // the physics hit, inside an ellipse inscribed in their mesh bounds. The conservative
        // footprint avoids splashes in dry bounding-box corners of the irregular shoreline.
        session.client.world.query<ecs::WorldTransform, render::MeshRef, render::MaterialRef>()
            .for_each([&](ecs::Entity,
                          const ecs::WorldTransform& pose,
                          const render::MeshRef& mesh,
                          const render::MaterialRef& material) {
                if (material.material != environment->water_material() ||
                    !visuals->meshes.contains(mesh.mesh))
                    return;
                const auto inverse = core::inverse(core::to_matrix(pose.value));
                const auto local = core::transform_point(inverse, origin);
                const auto ray = core::transform_vector(inverse, direction);
                if (ray.y >= -1e-5f)
                    return;
                const float t = -local.y / ray.y;
                if (t < 0 || t >= nearest)
                    return;
                const auto at = local + ray * t;
                const auto& shape = visuals->meshes.get(mesh.mesh);
                const float rx = (shape.local_max.x - shape.local_min.x) * .42f;
                const float rz = (shape.local_max.z - shape.local_min.z) * .42f;
                if (rx <= 0 || rz <= 0)
                    return;
                const float x = (at.x - (shape.local_max.x + shape.local_min.x) * .5f) / rx;
                const float z = (at.z - (shape.local_max.z + shape.local_min.z) * .5f) / rz;
                if (x * x + z * z > 1)
                    return;
                nearest = t;
                hit = origin + direction * t;
                found = true;
            });
        return found;
    }

    [[nodiscard]] core::Transform render_camera_transform() const {
        auto eye = session.client.camera_transform();
        if (viewmodel && viewmodel_enabled) {
            // Presentation-only view punch: compose a small local rotation onto a COPY of the
            // eye. Both the parent camera and the optical pose inherit it, while the input tape
            // and authoritative hitscan continue to use the untouched simulation view.
            const auto kick = viewmodel->recoil_rotation();
            eye.rotation = eye.rotation * core::quat_from_euler(kick.x * .35f, kick.y * .10f, 0);
        }
        return eye;
    }

    // The shared live-input seam is also exercised by the accepted-shot smoke proof.
    void step_play(gameplay::CharacterInput input) {
        if (viewmodel) {
            // This local play session owns one authoritative weapon. Selection configures
            // its existing trigger contract; it does not invent a replicated inventory.
            block_play::configure_selected_weapon(
                session.server.weapon,
                static_cast<block_play::PlayWeaponMotion::Weapon>(viewmodel->selected()));
            // GameplayServer reads the avatar component, not the spawn template above.
            // Updating only that template would leave an already-spawned sniper automatic.
            for (const auto avatar : session.server.avatars)
                if (auto* weapon = session.server.world.get<gameplay::WeaponConfig>(avatar))
                    block_play::configure_selected_weapon(
                        *weapon,
                        static_cast<block_play::PlayWeaponMotion::Weapon>(viewmodel->selected()));
            input = block_play::filter_busy_fire(
                input, viewmodel->busy(), session.server.weapon.fire_bit);
            // Empty magazine: the pull produces no shot (so no recoil, tracer or report, all of
            // which hang off an accepted shot), clicks once and starts the reload. The busy
            // filter above already owns the trigger during a reload, so there is one click.
            const auto trigger = viewmodel->ammo().filter_trigger(
                input, viewmodel->weapon(), viewmodel->busy(), session.server.weapon.fire_bit);
            input = trigger.input;
            if (trigger.dry_fire) {
                mixer.play(kDryClick, session.client.camera_transform().translation, .7f);
                (void)viewmodel->begin_ammo_reload();
            }
        }
        step_sim(gameplay_net::to_input_command(input));
    }

    // One simulation tick, plus everything downstream of it that is not drawing.
    void step_sim(const replication::InputCommand& intent) {
        session.tick(intent);
        if (visuals)
            (void)visuals->authored_environment.tick(session.client.world, session.tick_index, kDt);
        if (effects)
            (void)effects->tick(
                session.tick_index, session.client.destruction.events(), static_cast<float>(kDt));
        // Session::tick has joined both jobs: these are accepted authoritative shots, including
        // misses. Rejected cooldown inputs produce neither recoil nor visual feedback.
        std::size_t surface_index = 0;
        for (const auto& shot : session.server.gameplay.shots()) {
            auto surface = surface_index < session.shot_surfaces.size()
                               ? session.shot_surfaces[surface_index]
                               : block_play::PlayWeaponFx::Surface::Concrete;
            ++surface_index;
            if (session.server.avatars.empty() ||
                shot.shooter_entity != session.server.avatars.front())
                continue;
            shot_presented = true;
            // One round per accepted shot, counted per tick (never per render frame).
            if (viewmodel)
                (void)viewmodel->ammo().consume(viewmodel->weapon());
            if (weapon_fx) {
                const auto eye = session.client.camera_transform();
                const auto muzzle =
                    viewmodel ? viewmodel->muzzle_world(eye) : shot.origin + shot.direction * .5f;
                auto hit = shot.did_hit
                               ? shot.point
                               : shot.origin + shot.direction * session.server.weapon.range;
                const bool water = water_intersection(shot.origin, shot.direction, hit);
                if (water)
                    surface = block_play::PlayWeaponFx::Surface::Water;
                const unsigned weapon =
                    viewmodel ? static_cast<unsigned>(viewmodel->selected()) : 0;
                weapon_fx->set_flash_size(block_play::PlayWeaponFx::kWeaponFlashSize[weapon]);
                const unsigned stride = weapon == 0 ? 3 : 4;
                const bool tracer = weapon != 2 && weapon_rounds[weapon]++ % stride == 0;
                weapon_fx->fire(shot.origin,
                                shot.direction,
                                muzzle,
                                hit,
                                shot.did_hit || water,
                                water ? core::Vec3{0, 1, 0} : shot.normal,
                                shot.body,
                                &session.server.physics,
                                surface,
                                tracer);
                mixer.play(kGunReport + weapon, shot.origin, .85f);
                if (shot.did_hit || water)
                    mixer.play(kBulletImpact + static_cast<unsigned>(surface), hit, .65f);
                if (!viewmodel || !viewmodel->scoped())
                    weapon_fx->queue_case();
            }
        }
        if (session.world_stopped) {
            app.request_quit();
            return;
        }

        // The scripted hero beat. In --play a key does this instead; either way it is one server
        // side call, so the two paths cannot drift into two different explosions.
        if (scripted_charge && !charge_fired && session.tick_index >= kChargeAt) {
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
        // deaf, so only interactive play submits to the device — a mixer never rendered reports a
        // voice count that only ever grows, and the "voices retire" claim would be untested here.
        static std::vector<float> scratch(
            static_cast<std::size_t>(static_cast<double>(audio::kSampleRate) * kDt) * 2u, 0.0f);
        mixer.render(scratch);
        audio_output.submit(scratch);
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
            if (city_dresser.ready()) {
                (void)city_dresser.sync(
                    session.client.world, session.client.destruction, visuals->leaves);
            }
        }
    }

    void fire_charge() {
        charge_fired = true;
        const blockkit::BlockParams p;
        (void)detonate(session, p.hero_south, hero_charge(p));
    }

    // AIMED CHARGE for --play. fire_charge() above targets the authored hero corner of the
    // generated block, which is empty ground on an authored city scene, so F did nothing there.
    // Here the charge goes where the player looks: a ray from the eye finds the first
    // surface within reach, and the blast is offered to EVERY destructible instance, because an
    // authored scene has no SlabRole building index. apply_damage tests each part against the
    // radius, so far-away instances cost a bounds check and stay intact. Returns false (nothing
    // detonated) when the ray hits nothing, so a charge thrown at the sky is not spent.
    bool fire_charge_aimed(float reach = 60.0f) {
        const core::Transform eye = session.client.camera_transform();
        const core::Vec3 forward = core::rotate(eye.rotation, core::Vec3{0, 0, -1});
        // Aim against the CLIENT's replica (what the player sees), excluding the player's own
        // capsule or the ray stops at distance ~0. Only the hit POINT crosses to the server, as
        // a world position, so the two worlds' differing BodyIds never meet.
        physics::QueryFilter filter;
        filter.exclude = session.client.local_body();
        physics::RayHit hit{};
        if (!session.client.physics.raycast({eye.translation, forward, reach}, hit, filter)) {
            RIME_INFO(
                "play: aimed charge: ray from ({:.1f}, {:.1f}, {:.1f}) hit nothing within {} m",
                eye.translation.x,
                eye.translation.y,
                eye.translation.z,
                reach);
            return false;
        }
        charge_fired = true;
        {
            // DIAGNOSTIC: the same ray on the SERVER world, and which destructible instance (if
            // any) owns the body it strikes.
            physics::RayHit shit{};
            const bool sh =
                session.server.physics.raycast({eye.translation, forward, reach}, shit, {});
            std::size_t owner = static_cast<std::size_t>(-1);
            for (std::size_t i = 0; sh && i < session.server.destruction.instance_count(); ++i)
                if (session.server.destruction.body_of(
                        destruction::InstanceId{static_cast<std::uint32_t>(i), 0}) == shit.body)
                    owner = i;
            RIME_INFO("play: server ray hit={} dist={:.1f} body={} owning_instance={} of {}",
                      sh,
                      shit.distance,
                      shit.body.index,
                      static_cast<long long>(owner),
                      session.server.destruction.instance_count());
        }
        Charge charge;
        // Pull the centre back off the surface so the blast straddles the wall instead of
        // sitting on its face, where half the sphere would be wasted in open air.
        charge.at = hit.point - forward * 0.5f;
        charge.radius = 4.0f; // a breach, not the whole-building hero blow-out (11 m)
        // Walk the destruction world's own instance table, as the bullet path does.
        auto& destruction = session.server.destruction;
        const std::size_t offered = destruction.instance_count();
        for (std::size_t i = 0; i < offered; ++i)
            destruction.apply_damage(destruction::InstanceId{static_cast<std::uint32_t>(i), 0},
                                     charge.at,
                                     charge.radius,
                                     charge.damage,
                                     {0.0f, charge.impulse * 0.3f, 0.0f});
        RIME_INFO("play: aimed charge at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m away, offered to {} "
                  "destructible instances",
                  charge.at.x,
                  charge.at.y,
                  charge.at.z,
                  hit.distance,
                  offered);
        return true;
    }

    // Pose the camera from the PREDICTED player and draw the client's world. `ctx.world` is the
    // Application's own and is deliberately empty: the thing on screen is what a client holds,
    // which is the only honest thing to show in a networked demo.
    void render(app::FrameContext& ctx) {
        if (!visuals || ctx.graph == nullptr) {
            return;
        }
        ecs::World& world = session.client.world;
        smoke_particles.clear();
        if (effects) {
            smoke_particles.clear();
            const auto fire = effects_visible ? effects->light() : std::nullopt;
            auto* light = world.get<render::PointLight>(fire_light);
            auto* transform = world.get<ecs::WorldTransform>(fire_light);
            if (light && transform) {
                light->intensity = fire ? fire->intensity * 6.0f : 0;
                if (fire) {
                    transform->value.translation = fire->position;
                    light->color_r = fire->color.x;
                    light->color_g = fire->color.y;
                    light->color_b = fire->color.z;
                    light->radius = fire->radius * 4.0f;
                }
                world.mark_changed<ecs::WorldTransform>(fire_light);
                world.mark_changed<render::PointLight>(fire_light);
            }
            if (effects_visible) {
                for (const auto& particle : effects->field().particles()) {
                    render::GpuParticle p{};
                    p.position_size[0] = particle.position.x;
                    p.position_size[1] = particle.position.y;
                    p.position_size[2] = particle.position.z;
                    p.position_size[3] = particle.size;
                    // Approximate incident light at the billboard centre. This shares the fire
                    // light's hue/range; it is not volumetric scattering or self-shadowing.
                    float warm = 0;
                    if (fire) {
                        const float range = fire->radius * 4.0f;
                        const float falloff = std::max(
                            0.0f, 1.0f - core::length(particle.position - fire->position) / range);
                        warm = falloff * falloff * fire->intensity * .35f;
                    }
                    p.color_alpha[0] = .12f + warm;
                    p.color_alpha[1] = .13f + warm * .35f;
                    p.color_alpha[2] = .14f + warm * .08f;
                    const float life = particle.age / particle.lifetime;
                    p.color_alpha[3] = .45f * std::min(particle.age * 6, 1.0f) * (1 - life);
                    smoke_particles.push_back(p);
                }
            }
            render::FxSettings settings{};
            settings.enabled = effects_visible;
            settings.max_particles = block_play::PlayEffects::kParticleCapacity;
            visuals->renderer.set_smoke_particles(smoke_particles, settings);
            if (visuals->scope_renderer)
                visuals->scope_renderer->set_smoke_particles(smoke_particles, settings);
        }
        visuals->authored_environment.publish(
            world, smoke_particles, visuals->renderer, visuals->scope_renderer.get());
        if (weapon_fx) {
            weapon_fx->advance(static_cast<float>(ctx.frame_dt));
            // Merge AFTER authored publication, retaining its full budget. Bullet dust owns a
            // separate fixed pool and must not displace scripted smoke or overwrite fire smoke.
            const auto& authored = visuals->authored_environment.snapshot();
            const auto base = authored.override_smoke
                                  ? authored.smoke_span()
                                  : std::span<const render::GpuParticle>(smoke_particles);
            combined_weapon_smoke.assign(base.begin(), base.end());
            weapon_fx->append_smoke(combined_weapon_smoke);
            render::FxSettings settings{};
            settings.enabled = true;
            settings.max_particles = block_play::AuthoredEnvironment::smoke_capacity +
                                     block_play::PlayWeaponFx::kDustSlots;
            visuals->renderer.set_smoke_particles(combined_weapon_smoke, settings);
            if (visuals->scope_renderer)
                visuals->scope_renderer->set_smoke_particles(combined_weapon_smoke, settings);
        }
        if (auto* tf = world.get<ecs::WorldTransform>(visuals->camera)) {
            tf->value = use_authored_camera ? visuals->authored_camera : render_camera_transform();
        }
        if (play_world) {
            // --world-gpu-smoke pins the camera at a far, deterministic pose; live play and
            // --world-smoke never set use_authored_camera together with a streamed world.
            const core::Vec3 eye = use_authored_camera
                                       ? visuals->authored_camera.translation
                                       : session.client.camera_transform().translation;
            if (!play_world->recenter(eye)) {
                session.world_stopped = true;
                app.request_quit();
                return;
            }
            visuals->renderer.set_terrain_draws(play_world->pass(), play_world->draws());
            play_world->mark_drawn();
        }
        if (viewmodel && viewmodel_enabled) {
            const core::Transform eye = render_camera_transform();
            viewmodel->observe_camera(eye.translation);
            visuals->renderer.set_first_person_draws(viewmodel->pose(eye),
                                                     viewmodel->identity_epoch());
            visuals->renderer.set_first_person_deformations(viewmodel->deformations());
            if (weapon_fx) {
                // Pose the gun before its live flash. Fixed-tick shot positions lag the
                // predicted camera during movement; only accepted world paths stay there.
                const auto direction = core::rotate(eye.rotation, core::Vec3{0, 0, -1});
                const auto right = core::rotate(eye.rotation, core::Vec3{1, 0, 0});
                const auto up = core::rotate(eye.rotation, core::Vec3{0, 1, 0});
                const auto velocity = session.client.predictor.seeded()
                                          ? session.client.predictor.state().velocity
                                          : core::Vec3{};
                const auto muzzle = viewmodel->muzzle_world(eye);
                const block_play::PlayWeaponFx::ViewPose view{eye.translation,
                                                              direction,
                                                              muzzle,
                                                              muzzle - direction * .32f +
                                                                  right * .075f - up * .025f,
                                                              velocity};
                weapon_fx->pose(world, &session.server.physics, &view);
            }
            if (viewmodel->scoped()) {
                // Each optical view owns its history. The main camera is never mutated,
                // and both views read the same replicated world and streamed terrain.
                const bool feed_visible = viewmodel->scope_feed_visible(eye);
                if (feed_visible)
                    ensure_scope_renderer();
                const core::Mat4 optic_world = viewmodel->optic_world();
                const auto camera = optical_camera(optic_world);
                render::RGTexture scope_hdr;
                core::Mat4 optical_view_proj{};
                visuals->last_scope_ldr = {};
                if (feed_visible) {
                    auto& optical = *visuals->scope_renderer;
                    auto scope_lighting = visuals->renderer.lighting();
                    // Measured wins belong to the narrow optical view: the main cascades
                    // enclose this entire city, so their per-draw tests only add CPU cost.
                    scope_lighting.shadow_caster_culling = true;
                    optical.set_lighting(scope_lighting);
                    optical.set_sky(visuals->renderer.sky());
                    if (play_world)
                        optical.set_terrain_draws(play_world->pass(), play_world->draws());
                    const auto scoped =
                        optical.render(*ctx.graph, world, {512, 512}, true, &camera);
                    scope_hdr = scoped.hdr;
                    visuals->last_scope_ldr = scoped.ldr;
                    optical_view_proj = optical.view_proj_unjittered();
                    ++scope_feed_frames;
                } else {
                    // Keep the SAME physical glass/reflection pass when the eye cannot see
                    // through the pupil. Its transmitted term is identically zero, so a 1x1
                    // black input replaces an otherwise invisible full second world render.
                    scope_hdr = ctx.graph->create_texture(
                        {{1, 1}, render::kHdrFormat, "scope-closed-pupil"});
                    const render::RGColorAttachment color[] = {
                        {scope_hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 0}}};
                    render::RenderGraph::RasterPassDesc desc{};
                    desc.colors = color;
                    ctx.graph->add_raster_pass(
                        "scope-closed-pupil", desc, [](rhi::CommandBuffer&) {});
                }
                render::ScopeLensPass::Params params{};
                params.optic_from_world = core::inverse(optic_world);
                params.optical_view_proj = optical_view_proj;
                params.optical_tan_half_fov = std::tan(camera.fov_y * .5f);
                params.lens_radius = block_play::PlayViewmodel::kScopeLensRadius;
                params.eye_relief = block_play::PlayViewmodel::kScopeEyeRelief;
                params.eye_box_radius = block_play::PlayViewmodel::kScopeEyeBoxRadius;
                params.eye_relief_tolerance = block_play::PlayViewmodel::kScopeEyeReliefTolerance;
                params.intensity = scope_transmission;
                visuals->renderer.set_first_person_scope(scope_hdr, viewmodel->lens(), params);
            } else if (viewmodel->selected() == block_play::PlayViewmodel::Weapon::Scar) {
                render::ScopeLensPass::Params params{};
                params.unmagnified = true;
                params.optic_from_world = core::inverse(viewmodel->optic_world());
                params.lens_radius = .026f;
                params.intensity = scope_transmission;
                visuals->renderer.set_first_person_scope({}, viewmodel->lens(), params);
            }
        }
        if (weapon_fx && (!viewmodel || !viewmodel_enabled))
            weapon_fx->pose(world, &session.server.physics);
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
        if (!development_hud)
            return;
        if (viewmodel_enabled && viewmodel && viewmodel->aim_fraction() > .5f)
            return; // keep every aiming sight clear of the development statistics panel

        if (player_help && viewmodel) {
            constexpr std::array names{"1 SCAR-H | RMB: sight | hold LMB: fire | R: reload",
                                       "2 M4A1 | RMB: sight | hold LMB: fire | R: reload",
                                       "3 MSR / PM II | RMB: scope | click LMB: fire | R: reload"};
            hud.text(st::kPadding,
                     static_cast<float>(ctx.extent.height) - 24.0f,
                     names[static_cast<unsigned>(viewmodel->selected())],
                     st::kAccent,
                     14.0f);
            // rounds / capacity, right above the hint; red-ish while empty, amber while reloading.
            const auto weapon = viewmodel->weapon();
            char ammo_text[32];
            std::snprintf(ammo_text,
                          sizeof(ammo_text),
                          viewmodel->reloading() ? "%u / %u  RELOADING" : "%u / %u",
                          viewmodel->ammo().rounds(weapon),
                          block_play::PlayAmmo::capacity(weapon));
            hud.text(st::kPadding,
                     static_cast<float>(ctx.extent.height) - 48.0f,
                     ammo_text,
                     viewmodel->ammo().empty(weapon) ? st::kWarn : st::kText,
                     22.0f);
        }
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
        // Compare by wire identity, just like the headless convergence proof. Local instance
        // allocation order can differ as well as BodyIds: dropping handles from a local-order
        // hash still falsely labels an undamaged city as divergent.
        const bool agree = destruction_net::shared_state_hash(session.client.world,
                                                              session.client.replicator->map(),
                                                              session.client.destruction) ==
                           destruction_net::shared_state_hash(session.server.world,
                                                              session.server.replicator->map(),
                                                              session.server.destruction);
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
    double intact_street = 0.0;   // the STREET only — no sky pixels, so the claim is about light
    double intact_rb = 0.0;       // its red/blue tint under the authored sky
    double red_sun_rb = 0.0;      // and under a deliberately red physical source (m17.7d)
    std::uint32_t sky_filled = 0; // how many times the sky was actually baked

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
                    intact_street = mean_luma_lower(px, kWidth, kHeight, 0.35);
                    intact_rb = mean_rb_ratio_lower(px, kWidth, kHeight, 0.35);
                    if (!g_ppm.empty()) {
                        write_ppm(g_ppm.c_str(), px, kWidth, kHeight);
                    }
                }
            }
            demo.app.finish_gpu();

            // The sky's own A/B (m17.7b), taken on the SAME frame and the same camera. Before this
            // brick the sky was a background and changed no lit pixel; now it is what lights the
            // street, and the only way to show that from the outside is to take it away and watch
            // the street change. A claim that cannot fail is not a claim — if these two numbers
            // come out equal, the sky is not reaching the shading and the claim below says so.
            if (render::RenderGraph* graph = demo.app.graph()) {
                // Arm two: the SAME frame under a deliberately red physical solar source. Nothing
                // else changes — same camera, direction, materials and tick. m17.7d deliberately
                // leaves zenith/horizon as background-only art controls, so changing those would
                // be a false A/B; this source is what the physical sky-view/SH body actually uses.
                render::SkyParams red = Visuals::authored_sky();
                red.sun_radiance[0] = 1.0f;
                red.sun_radiance[1] = 0.03f;
                red.sun_radiance[2] = 0.03f;
                demo.visuals->renderer.set_sky(red);
                demo.app.run_frames(4);
                const rhi::TextureHandle tex = graph->physical(demo.visuals->last_ldr);
                if (tex.is_valid()) {
                    const std::vector<std::uint8_t> red_px =
                        read_rgba8(*demo.app.device(), tex, kWidth, kHeight);
                    red_sun_rb = mean_rb_ratio_lower(red_px, kWidth, kHeight, 0.35);
                }
                demo.app.finish_gpu();
                demo.visuals->renderer.set_sky(Visuals::authored_sky()); // the scene as authored
                demo.app.run_frames(1);
                demo.app.finish_gpu();
            }
            sky_filled = demo.visuals->renderer.sky_lighting_stats().filled;

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
    // WHY the loop's two exits must be told apart: it leaves either because the peers agreed or
    // because the bound ran out, and only the first is convergence. A message that says "agreed"
    // on both is a proof that reads as passing when it is not. When the bound DOES run out there
    // are two very different causes: delivery is behind (or an op was lost), or the authority is
    // still committing NEW damage — empty player input is not authoritative quiescence, since the
    // falling pile keeps fracturing things. The server's own state hash is the tell: while it
    // keeps changing, the server is still producing damage and the peers cannot be expected to
    // match; once it has been stable for many ticks and the peers still disagree, the fault is in
    // delivery. This is observation only — it changes no behaviour and gates nothing.
    const auto server_state_hash = [&demo] {
        return destruction_net::shared_state_hash(demo.session.server.world,
                                                  demo.session.server.replicator->map(),
                                                  demo.session.server.destruction);
    };
    const auto client_state_hash = [&demo] {
        return destruction_net::shared_state_hash(demo.session.client.world,
                                                  demo.session.client.replicator->map(),
                                                  demo.session.client.destruction);
    };
    std::uint64_t server_hash_changes = 0;
    std::uint64_t server_stable_tail = 0;
    auto last_server_hash = server_state_hash();
    bool peers_agree = last_server_hash == client_state_hash();
    while (settled_after < kSettleBound && !peers_agree) {
        demo.step_sim(replication::InputCommand{});
        ++settled_after;
        const auto sh = server_state_hash();
        if (sh != last_server_hash) {
            ++server_hash_changes;
            server_stable_tail = 0;
            last_server_hash = sh;
        } else {
            ++server_stable_tail;
        }
        peers_agree = sh == client_state_hash();
    }
    if (peers_agree) {
        std::printf("  settled   : peers agreed after %llu quiet ticks (bound %llu)\n",
                    static_cast<unsigned long long>(settled_after),
                    static_cast<unsigned long long>(kSettleBound));
    } else {
        std::printf("  settled   : peers did NOT agree - the bound was exhausted after %llu quiet "
                    "ticks (bound %llu)\n",
                    static_cast<unsigned long long>(settled_after),
                    static_cast<unsigned long long>(kSettleBound));
    }
    std::printf("  settle obs: server hash changed on %llu of %llu quiet ticks, unchanged for the "
                "last %llu (moving = authority still damaging; stable while peers differ = "
                "delivery behind or lost)\n",
                static_cast<unsigned long long>(server_hash_changes),
                static_cast<unsigned long long>(settled_after),
                static_cast<unsigned long long>(server_stable_tail));

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
    {
        const auto& ds = demo.session.server.destruction_net_server;
        const std::uint64_t sent = ds.ops_sent();
        const std::uint64_t applied = dc.ops_applied();
        std::printf("  op ledger : server sent %llu ops in %llu packets (%llu unaddressable, %llu "
                    "composition checks); client applied %llu, dropped-unmapped %llu, malformed "
                    "%llu, unverified checks %llu; outstanding (sent - applied) %lld\n",
                    static_cast<unsigned long long>(sent),
                    static_cast<unsigned long long>(ds.op_packets_sent()),
                    static_cast<unsigned long long>(ds.ops_unaddressable()),
                    static_cast<unsigned long long>(ds.composition_checks_sent()),
                    static_cast<unsigned long long>(applied),
                    static_cast<unsigned long long>(dc.ops_dropped_unmapped()),
                    static_cast<unsigned long long>(dc.malformed_messages()),
                    static_cast<unsigned long long>(dc.composition_checks_unverified()),
                    static_cast<long long>(sent) - static_cast<long long>(applied));
        // Refusals are a separate line on purpose. They happen BEFORE channel admission (the
        // reliable channel's backlog was full), so they are neither in "sent" above nor packets the
        // scripted network later lost: an op counted here never left the server at all. Folding
        // them into "outstanding" would change what that number means, and leaving them out would
        // let a lost damage transition read as a clean ledger.
        std::printf("  op refused: %llu ops in %llu packets, %llu composition checks "
                    "(refused at admission; not in sent, not network loss)\n",
                    static_cast<unsigned long long>(ds.ops_refused()),
                    static_cast<unsigned long long>(ds.op_packets_refused()),
                    static_cast<unsigned long long>(ds.composition_checks_refused()));
    }
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
    std::size_t collapsed = 0, standing = 0;
    {
        const blockkit::BlockParams p;
        std::printf("  locality  :");
        for (std::size_t b = 0; b + 1 < parts_end_per.size(); ++b) {
            const bool intact = parts_end_per[b] >= parts_at_start_per[b];
            // "Far side" = the north row, across the street from the south hero that was charged.
            if (b >= p.buildings_per_side && intact) {
                ++far_side_intact;
            }
            // RETENTION BANDS, not "lost zero parts". The fraction each building keeps is sharply
            // bimodal in this beat -- measured 2026-10-17, the three collapsing buildings retain
            // 1.7 %, 8.1 % and 23.9 %, while every building left standing retains 98.3 % to
            // 100 %. Nothing lands between 24 % and 98 %, so thresholds placed in that empty band
            // describe the CONTENT (did this building come down, or not) instead of describing
            // floating-point ordering. The old primitive was `lost zero parts`, which asks
            // whether any one fragment's trajectory shifted by a single contact -- so it flipped
            // on a legitimate debris fix that moved 3 parts out of 2016, 0.15 % of the block,
            // while the beat itself was unchanged. These bands give the tightest claim 15 parts
            // of margin instead of zero. Re-measure the bands if the beat is ever re-authored;
            // they are a measurement, not a convention.
            const double keep = parts_at_start_per[b] > 0
                                    ? static_cast<double>(parts_end_per[b]) /
                                          static_cast<double>(parts_at_start_per[b])
                                    : 1.0;
            if (keep < 0.50) {
                ++collapsed;
            }
            if (keep >= 0.90) {
                ++standing;
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
        // REMOVED (m17.5): {"net: no debris left unresolved", dc.debris_unresolved() == 0}.
        //
        // It could not fail. `debris_unresolved_` and `debris_bound_` are incremented in exactly
        // one place — inside `DestructionClient::sync_debris` — and this demo's client never calls
        // it. Only the SERVER's `sync_debris` is wired (the debris bridge above), so both counters
        // are structurally 0 for the whole run and the check read as evidence while asserting
        // nothing. Deleting a proof is worse than keeping it only if the proof was doing work; this
        // one was doing the opposite, by making a gap look covered.
        //
        // THE GAP IT WAS HIDING, recorded rather than quietly fixed: no sample binds replicated
        // debris on the client — not this one and not `12-networked-destruction`. The three
        // `destruction_client.sync_debris` call sites in the repository are all in
        // `tests/destruction_net`. So the client-side half of m11.4b's addressing proof (mirrors
        // resolving to the chunks the client derived) is exercised by tests and by nothing that
        // ships. Wiring it here would add per-tick client cost to the very number m17.5 is
        // measuring, so it is a scope decision rather than a drive-by: see the m17.5 notes in
        // docs/ROADMAP.md.
        {"net: the peers' destruction state hashes agree", server_hash == client_hash},
        {"net: they agreed WITHIN the settle bound", settled_after < kSettleBound},
        // The claim m13.5 shipped as a KNOWN DEFECT and m13.6 earned. Before the fix this read
        // 0 of 4 and 0 of 8 — the whole block, every time.
        {"collapse: the far side of the street is untouched",
         far_side_intact == blockkit::BlockParams{}.buildings_per_side},
        {"collapse: most of the block is still standing", standing >= 5},
        // The complement, and the one that actually catches a GLOBAL collapse: if the block
        // cannot survive being touched, this count runs away while the one above drops.
        {"collapse: the damage stayed local to a few buildings", collapsed <= 3},
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

        // ── THE COOKED GROUND (m17.8b, ADR-0041 Ruling 5) ────────────────────────────────────
        // Only when the cook ran: `rime ground` is a build fixture, and a tree without it must
        // still pass with the palette's flat street. That is the degradation `apply_ground`'s
        // fallback exists for, so making this claim unconditional would turn a supported
        // configuration into a red run.
        //
        // Reported rather than inferred. `start` already refuses if nothing wore the material, but
        // that only proves a MaterialRef changed. The property worth asserting is one step further
        // down: the surface most of the frame is made of must wear a material whose base-colour
        // map is RESIDENT. `is_valid()` alone cannot say that — the magenta placeholder is a
        // perfectly valid handle, which is how a barrel once shipped rendering solid magenta past
        // a green assertion — so the comparison is against the placeholder itself.
        //
        // EVERY surface must pass, and there must be one. The block spawns a single street today,
        // but a claim that let the last surface visited overwrite the verdict would pass a broken
        // first surface the day a second one appears, depending only on iteration order.
        if (demo.bridge) {
            std::size_t surfaces = 0;
            std::size_t surfaces_cooked = 0;
            ecs::World& cw = demo.session.client.world;
            cw.query<ground::GroundSurface>().for_each([&](ecs::Entity e, ground::GroundSurface&) {
                ++surfaces;
                const render::MaterialRef* ref = cw.get<render::MaterialRef>(e);
                if (ref == nullptr) {
                    return;
                }
                const render::PbrMaterialDesc& d = demo.visuals->materials.get(ref->material);
                if (d.base_color_texture.is_valid() &&
                    d.base_color_texture != demo.bridge->placeholder_texture()) {
                    ++surfaces_cooked;
                }
            });
            claims.push_back({"render: the street wears its COOKED material",
                              surfaces > 0 && surfaces_cooked == surfaces});
        }

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
        // The sky (m17.7d). Work done, not a flag set — `filled` counts bakes that actually ran,
        // and the luminance pair is the sky's effect on the STREET rather than on the horizon.
        claims.push_back({"sky: the sky was baked", sky_filled > 0});
        claims.push_back({"sky: the street is tinted by the physical sky, not by a constant",
                          intact_rb > 0.0 && red_sun_rb > intact_rb * 1.10});
        std::printf("  sky       : baked %u time(s); street r/b %.3f under the authored sky vs "
                    "%.3f under a red physical sun (street luma %.2f)\n",
                    sky_filled,
                    intact_rb,
                    red_sun_rb,
                    intact_street);
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
    // Frames the GPU may be behind the CPU. 1 = `submit_blocking`, which is what every committed
    // baseline was measured under and therefore the default (m17.5).
    std::uint32_t pipelined = 1;
};

// An upper bound on --pipelined, because every frame in flight costs a full set of per-frame rings
// and, past a couple, the image trails the simulation by long enough that what the report measures
// stops resembling what a player would see. It is not a safety limit — the rings resize now — it is
// a limit on what this sample is willing to claim it measured.
constexpr std::uint32_t kMaxPipelineDepth = 4;

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

    // PIPELINE THE LOOP, if asked (m17.5). Set before the first frame, because every ring sized
    // from this number is rebuilt by the call and doing that under live GPU work is the bug the
    // rings exist to prevent.
    //
    // `set_headless_frames_in_flight` resizes the ring the Application OWNS — the graph's CPU
    // scratch — and it cannot reach the ones it does not own. This comment used to claim it resized
    // "the SceneRenderer's uniforms" too. It did not, and at a depth of 3 that was a live race:
    // the graph grew to 4 slots while the SceneRenderer and the HUD stayed at 3, so frame K wrote
    // the slot frame K-3 was still being read out of. The contract is stated on
    // Application::frames_in_flight() — every owner of a per-frame resource sizes it from that —
    // and this is the sample honouring it rather than a comment asserting someone else did.
    demo.app.set_headless_frames_in_flight(opt.pipelined);
    if (demo.visuals) {
        demo.visuals->renderer.set_frames_in_flight(demo.app.frames_in_flight());
        demo.visuals->hud.set_frames_in_flight(demo.app.frames_in_flight());
    }

    core::PerfReport report;
    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = demo.app.device()->adapter();
    fp.gpu = adapter.name;
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    // The preset is part of the FINGERPRINT, so what goes in it decides what may be compared
    // (ADR-0035 decision 3). Pipelining belongs in it: a pipelined `frame` measures CPU wall with
    // the GPU alongside, a serialized one measures both in series, and comparing the two would be
    // exactly the quiet-wrong-comparison ADR-0041 Ruling 4 refuses. Naming it here means the gate
    // REFUSES that comparison by itself rather than relying on anyone noticing.
    fp.preset = "block-all-lighting-gates"; // csm + local shadows + clustered + sdf + ddgi + ssr
    if (opt.pipelined > 1)
        fp.preset += "+pipelined-" + std::to_string(opt.pipelined);
    fp.width = opt.width;
    fp.height = opt.height;
    report.set_machine(fp);
    report.set_run(core::RunInfo::detect("99-the-block"));

    // WHAT MUST ADD UP (m17.3c, ADR-0041 Ruling 2). m17.3a gave every pass a name and m17.3b gave
    // the simulation eleven, but "the frame is attributable" was still an unchecked belief: nothing
    // said the named parts account for the whole, so a subsystem added tomorrow without a zone
    // would simply be absent, and absent looks exactly like free.
    //
    // Three nestings, each one a real containment rather than a plausible grouping — a residual
    // over parts that are not inside their parent measures nothing:
    //
    //   frame        = sim.block + frame.render        (the two halves of the loop below)
    //   frame.render = the Application's own stage zones inside `app.step`
    //   sim.block    = the physics inside the Session's tick
    //
    // The third is the interesting one and the reason to do this before m17.5 rather than after:
    // `sim.block` p99 is 25.49 ms against a ratified 6.0, `physics.step.per_frame` is the only part
    // of it anything measures, and the difference — replication, destruction, debris, transforms —
    // has never had a number. `sim.block.unaccounted` is that number.
    //
    // `frame.present` is deliberately not a child: `--perf` runs headless, so the present zone
    // never fires, and naming a child that cannot record would count a gap on every frame. When
    // the bar moves windowed (m17.4 onward) it becomes one, and until then present time is
    // genuinely unaccounted rather than pretend-accounted.
    report.declare_accounting("frame", {"sim.block", "frame.render"});
    report.declare_accounting("frame.render",
                              {"sim.tick.per_frame",
                               "frame.declare.per_frame",
                               "frame.execute.per_frame",
                               "frame.submit.per_frame"});
    // Both worlds, because the tick runs both and the accounting must add up to the tick. Naming
    // only one leaves the other as unaccounted residual, which is the failure m17.3c's gate exists
    // to catch — it would read as "the simulation spends 8 ms somewhere nobody named".
    //
    // THE TREE CHANGED SHAPE WHEN THE HALVES STARTED OVERLAPPING (p2-perf, ADR-0077). It was
    // `sim.block = server step + client step + rest`, true only while the two ran in series; with
    // the server's half running alongside, those two steps sum to MORE than the wall clock, the
    // residual goes negative, and a `≤ 6.0` rule on it passes whatever happens — a ratchet that
    // can no longer fire. The containments that ARE real now:
    //
    //   sim.block  = sim.client + sim.join + rest   (this thread, in series: its half, then the
    //   wait) sim.client = physics.client.step + rest     (the client's half) sim.server =
    //   physics.server.step + rest     (the server's half, on a worker)
    //
    // The old single residual — everything in the sim that is not a physics step — is now three,
    // and the gate below splits its 6.0 between them rather than granting each the whole of it.
    report.declare_accounting("sim.block", {"sim.client", "sim.join"});
    report.declare_accounting("sim.client", {"physics.client.step.per_frame"});
    report.declare_accounting("sim.server", {"physics.server.step.per_frame"});
    // `frame.submit` is a blocking submit, and a blocking submit is three different costs
    // (p2-perf): the driver taking the command buffer, the fence wait while the GPU runs it, and
    // our teardown. The RHI times each; declaring them here makes the split add up or say it does
    // not. A negative residual would mean a blocking submit ran OUTSIDE `frame.submit` (an upload
    // mid-declare, say), which is exactly the misattribution worth seeing.
    report.declare_accounting("frame.submit.per_frame",
                              {"rhi.submit.queue.per_frame",
                               "rhi.submit.wait.per_frame",
                               "rhi.submit.reclaim.per_frame"});

    // Deliveries QUEUE rather than overwrite, and are drained after the loop rather than inside it.
    // A single slot consumed in the loop body loses the tail: pipelined, the frames still in flight
    // when the loop ends are retired by `finish_gpu()` AFTERWARDS, so their timings arrive with
    // nothing left to consume them — and arrive within one call, so each would overwrite the last
    // anyway. At `--pipelined 2` that silently cost every per-pass distribution two of its samples
    // and left the worst frame's breakdown empty whenever the worst frame was one of the last two.
    // (Blocking, timings resolve inline in the same frame and none were ever lost, which is exactly
    // why the default path could not see this.)
    std::vector<std::pair<std::uint64_t, std::vector<core::PassTiming>>> passes_queue;
    bool timestamps_seen = false;
    // The HIGH WATER MARK across the run, not the last frame's count (m17.3b). Which passes a frame
    // declares varies — local shadows only re-render invalidated slots — so testing the final
    // frame's count against the timestamp pool would report "everything was timed" whenever the run
    // happened to end quiet.
    // …and it counts what the frame DECLARED, not what came back timed (m17.3c). m17.3b compared
    // `passes.size()` against the pool, which is the count AFTER the cap has already truncated it
    // and (since the clipmap's repeats fold) after several passes have merged into one row — a
    // check that reads the output of the thing it is trying to detect can never see an overflow
    // larger than the cap, and now cannot see one at all. `pass_count()` is the declaration, which
    // is the number that has to fit.
    std::size_t max_passes_declared = 0;
    // `on_frame_timings` rather than `on_post_submit` (m17.5), because it is the only one of the
    // two that survives pipelining: it hands over resolved, OWNED timings tagged with the frame
    // they describe, instead of a graph and a command buffer whose lifetimes stopped lining up
    // with "now" the moment the loop stopped waiting for the GPU. Under `submit_blocking` it fires
    // inside `step()`; pipelined it fires when the ring retires that frame, two frames later —
    // which is why nothing below keys on the loop counter.
    demo.app.on_frame_timings(
        [&](std::uint64_t index, std::span<const render::RenderGraph::PassTiming> timings) {
            if (timings.empty())
                return;
            std::vector<core::PassTiming> owned;
            owned.reserve(timings.size());
            for (const render::RenderGraph::PassTiming& t : timings) {
                owned.push_back(core::PassTiming{t.name, t.gpu_ms});
                timestamps_seen = true;
            }
            passes_queue.emplace_back(index, std::move(owned));
        });
    demo.app.on_post_submit([&](render::RenderGraph& graph, rhi::CommandBuffer&) {
        // The declared-pass high-water still needs the graph itself, and this fires only on the
        // blocking path — which is the path the committed baselines are measured on, so the
        // headroom line keeps meaning what it meant.
        max_passes_declared = std::max(max_passes_declared, graph.pass_count());
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
    // The ledger counts the same window the distributions do — see Session::begin_measured_window.
    demo.session.begin_measured_window();
    const std::uint64_t measure_from_tick = tick;

    // THE TWO INDEX SPACES, and they are not the same one. `Application` counts every frame it has
    // ever rendered, warmup included; this loop counts measured frames from zero, and that is the
    // index the report is keyed on. The timings callback speaks the former. Translating here is
    // what makes `observe_passes` land on the frame it describes — without it the worst frame's
    // breakdown is silently empty, which is exactly how it looked before this line existed.
    const std::uint64_t frame_base = demo.app.frame_index();

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
        const double sim_ms = sim_watch.elapsed_ms();
        // The server's half ran as a job, so its zones closed on a worker and are parked; the
        // tick has joined it, so they belong to THIS frame. Folded in before anything reads them.
        zones.collect_parked();
        report.observe("sim.block", sim_ms);
        // The split that decides whether the ENGINE misses the budget or this DEMO's topology does.
        report.observe("sim.client", demo.session.last_client_ms);
        report.observe("sim.server", demo.session.last_server_ms);
        report.observe("sim.join", demo.session.last_join_ms);
        const core::Stopwatch render_watch;
        demo.app.step(demo.app.fixed_dt());
        const double render_ms = render_watch.elapsed_ms();
        const double ms = watch.elapsed_ms();
        // EVERY PART IS RECORDED BEFORE THE `observe_frame` THAT CLOSES THE FRAME. That ordering is
        // the accounting contract (m17.3c), not a style preference: a part recorded one line later
        // has no sample for the frame being closed, so it counts as a gap and the residual is
        // inflated by exactly its own cost. `frame.render` was observed after `observe_frame` until
        // this brick, which would have made `frame.unaccounted` read as the entire render cost.
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
            // The collapse, on TWO timelines. A hitch there is invisible in a 600-frame p99 — 90
            // frames cannot move the 594th — and obvious in a 90-frame one, which is exactly why
            // ADR-0035 asks for the window separately.
            //
            // `sim.collapse` is new (m17.3c) and it closes a hole the review found: ADR-0035
            // ratified TWO collapse numbers, "frame max ≤ 33 ms" and "collapse-tick max ≤ 12 ms",
            // and only the first had a timeline. So the gate below held the collapse to the frame
            // budget while the tick budget — the tighter, more specific one — went unenforced
            // through two milestones that quoted it as ratified.
            //
            // The interpretation, said out loud because it is one: `sim.block` is the sim's
            // per-FRAME wall clock for both peers, not one tick in isolation, and this reads the
            // ratified per-tick number against it. That is the same equivalence the existing
            // `sim.block p99 ≤ 6.0` rule already makes; making it twice without saying so is how
            // a budget quietly changes meaning.
            report.observe("frame.collapse", ms);
            report.observe("sim.collapse", sim_ms);
        }
        report.observe_frame(static_cast<std::uint64_t>(i), ms);
    }
    zones.stop();
    // Drains the ring, delivering the timings of every frame still in flight — which is why the
    // queue is drained AFTER this and not in the loop.
    demo.app.finish_gpu();

    // The passes go in SEPARATELY, keyed by the frame they belong to. Handing them to observe_frame
    // would be right under `submit_blocking` and wrong pipelined, where what arrives describes a
    // frame two back. Recorded after every observe_frame, so the worst-frame record already exists
    // to receive its own breakdown, whichever frame turned out to be the worst.
    //
    // `index` is the Application's frame counter and `i` was the loop's; they differ by the warmup,
    // which is what frame_base subtracts. A frame below frame_base is a warmup frame and is
    // dropped.
    for (const auto& [index, timings] : passes_queue) {
        if (index >= frame_base) {
            report.observe_passes(index - frame_base, timings);
            // The GPU's own account of the frame, summed over its timed passes (p2-perf). Read
            // against `rhi.submit.wait` it answers the one question the CPU zones cannot: is a
            // long fence wait the GPU working, or the GPU idle while something else stalls? A sum
            // of pass brackets, not first-begin to last-end, so gaps between passes are NOT in it.
            double gpu_ms = 0.0;
            for (const core::PassTiming& t : timings)
                gpu_ms += t.ms;
            report.observe("gpu.passes", gpu_ms);
        }
    }

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
    // ── The cooked ground material, counted rather than inferred (m17.8b) ─────────────────
    //
    // The A/B this brick is measured by differs in exactly ONE thing: whether `manifest.txt` was
    // present when the run started. Binary, scene, frame count and draw count are identical by
    // construction — which is what makes the comparison worth anything, and equally what makes the
    // two arms indistinguishable once the run is over. Without this counter the only record of
    // which arm a report came from is which file the operator happened to move, and an operator's
    // memory is not evidence. A skip path with no counter is a skip nobody can see.
    //
    // `resolved + steady`, not `resolved`: the entity-owned resolver skips an entity already
    // wearing the right material, so `resolved` falls to ZERO once everything converges. A run
    // that bound the ground perfectly would report 0 and read exactly like one that never bound it
    // at all (see MaterialAssetStats). The flat arm reads 0 here by construction — no manifest, no
    // catalog, no bridge object at all — so the counter separates the arms rather than describing
    // only one of them.
    std::uint64_t ground_materials_bound = 0;
    std::uint64_t ground_textures_refused = 0;
    if (demo.bridge) {
        const auto& mat = demo.bridge->material_asset_stats();
        ground_materials_bound = static_cast<std::uint64_t>(mat.resolved + mat.steady);
        // The device-capability refusal ADR-0039 ratified and m16.7 never implemented: a BC7 cook
        // asked of an adapter that cannot sample it. Nonzero means the ground drew untextured
        // while every structural claim in the run still passed.
        ground_textures_refused =
            static_cast<std::uint64_t>(demo.bridge->textures_refused_unsupported());
    }
    ledger.set("ground.materials_bound", ground_materials_bound);
    ledger.set("ground.textures_refused_unsupported", ground_textures_refused);
    ledger.set("net.max_batches_per_tick", demo.session.max_batches_per_tick);
    ledger.set("net.client_physics_steps", demo.session.total_client_steps);
    // Separate from the steps since m17.5: the client applies a destruction batch per queued
    // fracture boundary but steps physics once per tick, so one number can no longer stand for
    // both. Their RATIO is the catch-up backlog, which is the thing worth watching.
    ledger.set("net.client_batches_applied", demo.session.total_client_batches);
    // ── The solver's parallelism panel (m17.5), per world and co-sampled ────────────────────
    //
    // Whether the solve was even ALLOWED to go wide, whether it actually went, and how much width
    // there was at the moment it cost the most. A ledger that records the work but not the wiring
    // cannot distinguish "parallelism did not help" from "parallelism was never switched on" — the
    // exact confusion that let this demo run its solver single-threaded unnoticed. `job_workers`
    // is the pool and cannot fall to zero when the wiring is dropped; the `parallel_steps` pair is
    // the handoff and does.
    //
    // Both worlds appear because both simulate. Reporting the server's alone described under half
    // of the frame it was being used to explain.
    // PARTICIPANTS, not workers: `JobSystem::participant_count()` counts the calling thread too,
    // so a pool of 31 workers reports 32. Naming it `job_workers` was off by one in the direction
    // that flatters, and a ledger key is read years later by someone who will not check.
    ledger.set("physics.job_participants",
               static_cast<std::uint64_t>(demo.session.jobs.participant_count()));
    ledger.set("physics.server.parallel_steps", demo.session.server_parallel_steps);
    ledger.set("physics.client.parallel_steps", demo.session.client_parallel_steps);
    // The CEILING: the widest the solve ever got, in either world. Not to be combined with
    // anything below it — it is a different tick.
    ledger.set("physics.max_active_islands", demo.session.max_active_islands);
    ledger.set("physics.server.max_bodies", demo.session.server_max_bodies);
    ledger.set("physics.client.max_bodies", demo.session.client_max_bodies);
    ledger.set("physics.server.max_broadphase_pairs", demo.session.server_max_pairs);
    ledger.set("physics.client.max_broadphase_pairs", demo.session.client_max_pairs);
    // …and the tick the tail is made of, co-sampled. `min_islands` is the number that decides
    // whether a job system could have helped: it is the narrowest step in the most expensive tick,
    // and a 1 there means at least one of that tick's steps had nothing to divide.
    const auto& wt = demo.session.worst_tick_solve;
    ledger.set("physics.worst_tick_index", wt.tick - measure_from_tick);
    ledger.set("physics.worst_tick_us", static_cast<std::uint64_t>(wt.ms * 1000.0));
    ledger.set("physics.worst_tick_steps", wt.steps);
    ledger.set("physics.worst_tick_min_islands", wt.min_active_islands);
    ledger.set("physics.worst_tick_max_islands", wt.max_active_islands);
    ledger.set("physics.worst_tick_largest", wt.largest_active_island);
    ledger.set("physics.worst_tick_awake_bodies", wt.awake_bodies);
    ledger.set("physics.worst_tick_parallel_steps", wt.parallel_steps);
    // The worst tick's serial fraction in TIME, beside the same tick's body counts. Both are 0
    // without --island-profile, and then nothing below is printed -- an absent measurement reads as
    // absent rather than as a zero.
    ledger.set("physics.worst_tick_island_ns_total", wt.island_ns_total);
    ledger.set("physics.worst_tick_island_ns_largest", wt.island_ns_largest);
    // The per-STEP figures, which are the ones that bound anything. A tick's steps run in
    // sequence, so only a single step's (largest / total) is an Amdahl serial fraction.
    ledger.set("physics.worst_step_island_ns_total", demo.session.worst_step_island_ns);
    ledger.set("physics.worst_step_island_ns_largest", demo.session.worst_step_largest_ns);
    ledger.set("physics.worst_step_active_islands", demo.session.worst_step_active_islands);
    ledger.set("physics.worst_step_largest_bodies", demo.session.worst_step_largest_bodies);
    ledger.set("physics.worst_step_awake_bodies", demo.session.worst_step_awake_bodies);
    report.set_ledger(ledger);

    if (g_island_profile && demo.session.worst_step_island_ns > 0) {
        const auto& S = demo.session;
        // THE NUMBER: the most expensive solve STEP in the run, and what fraction of it was its
        // single biggest island. Per step, because that is the one parallel region -- see the
        // comment on worst_step_islands for why a per-tick ratio is not a bound.
        const double frac = static_cast<double>(S.worst_step_largest_ns) /
                            static_cast<double>(S.worst_step_island_ns);
        const double body_frac = (S.worst_step_awake_bodies > 0)
                                     ? static_cast<double>(S.worst_step_largest_bodies) /
                                           static_cast<double>(S.worst_step_awake_bodies)
                                     : 0.0;
        std::printf("  islands   : worst SOLVE STEP -- %llu us of island solve across %u active "
                    "islands, biggest island %llu us\n",
                    static_cast<unsigned long long>(S.worst_step_island_ns / 1000),
                    S.worst_step_active_islands,
                    static_cast<unsigned long long>(S.worst_step_largest_ns / 1000));
        std::printf("  islands   : SERIAL FRACTION %.4f in TIME vs %.4f in BODIES (%u of %u) -- "
                    "island-parallel ceiling %.3fx in time, %.3fx in bodies\n",
                    frac,
                    body_frac,
                    S.worst_step_largest_bodies,
                    S.worst_step_awake_bodies,
                    (frac > 0.0) ? 1.0 / frac : 0.0,
                    (body_frac > 0.0) ? 1.0 / body_frac : 0.0);
        // The two ceilings differing is the whole point: the body one is where the project's
        // recorded 1.09x came from, and only the time one is an Amdahl bound on the solve.
        std::printf("  islands   : worst TICK %llu totals (NOT a serial fraction -- %u sequential "
                    "steps): %llu us island solve, biggest single island %llu us\n",
                    static_cast<unsigned long long>(wt.tick - measure_from_tick),
                    wt.steps,
                    static_cast<unsigned long long>(wt.island_ns_total / 1000),
                    static_cast<unsigned long long>(wt.island_ns_largest / 1000));
        // Is the worst solve STEP in the worst solve TICK? "Halve this step" only moves the tail if
        // so. The tick compared against is `worst_tick_solve` -- the tick with the largest summed
        // physics step time, which is what the sample already tracks; it is NOT keyed on the
        // `sim.block` zone (the zone sink keeps no per-tick index), so a sim.block p99 tick can
        // still differ from both.
        std::printf(
            "  islands   : worst solve STEP ran in tick %llu; worst solve TICK is %llu -- %s\n",
            static_cast<unsigned long long>(S.worst_step_tick - measure_from_tick),
            static_cast<unsigned long long>(wt.tick - measure_from_tick),
            (S.worst_step_tick == wt.tick) ? "SAME tick" : "DIFFERENT ticks");
        const auto& isl = S.worst_step_islands;
        if (!isl.empty()) {
            std::printf("  islands   : the most expensive single STEP, %zu active islands\n",
                        isl.size());
            std::printf("  islands   :   island  bodies  constraints   solve_us   ns_per_body"
                        "   warm_us  velocity_us  store_us  position_us\n");
            for (const physics::IslandSolveCost& e : isl) {
                if (e.solve_ns == 0 && e.bodies == 0) {
                    continue;
                }
                std::printf(
                    "  islands   :   %6u  %6u  %11u  %9llu  %12llu  %8llu  %11llu  %8llu  %11llu\n",
                    e.island,
                    e.bodies,
                    e.constraints,
                    static_cast<unsigned long long>(e.solve_ns / 1000),
                    static_cast<unsigned long long>(e.bodies > 0 ? e.solve_ns / e.bodies : 0),
                    static_cast<unsigned long long>(e.warm_ns / 1000),
                    static_cast<unsigned long long>(e.velocity_ns / 1000),
                    static_cast<unsigned long long>(e.store_ns / 1000),
                    static_cast<unsigned long long>(e.position_ns / 1000));
            }
            // The number the phase split exists for: of the biggest island's solve, how much is the
            // velocity sweep, and what ONE velocity iteration costs. The 8 below is
            // `kVelocityIterations` from engine/physics/src/solver.hpp, which is a PRIVATE header
            // the sample cannot include -- so it is duplicated here and must be changed with it.
            // A stale copy would not fail to build, it would quietly misprice the one number this
            // line exists to report.
            const physics::IslandSolveCost* big = &isl.front();
            for (const physics::IslandSolveCost& e : isl) {
                if (e.solve_ns > big->solve_ns) {
                    big = &e;
                }
            }
            if (big->solve_ns > 0) {
                const double sn = static_cast<double>(big->solve_ns);
                std::printf(
                    "  islands   : biggest island %u: velocity %.1f%% of solve (warm %.1f%%, "
                    "store %.1f%%, position %.1f%%); ONE velocity iteration = %.1f us "
                    "(velocity_us / 8)\n",
                    big->island,
                    100.0 * static_cast<double>(big->velocity_ns) / sn,
                    100.0 * static_cast<double>(big->warm_ns) / sn,
                    100.0 * static_cast<double>(big->store_ns) / sn,
                    100.0 * static_cast<double>(big->position_ns) / sn,
                    static_cast<double>(big->velocity_ns) / 1000.0 / 8.0);
            }
        }
    }

    core::PerfGate gate;
    gate.at_most("frame", core::PerfStat::P99, 16.6)
        .at_most("frame", core::PerfStat::Max, 33.0)
        .at_most("frame.collapse", core::PerfStat::Max, 33.0)
        // ADR-0035's OTHER ratified collapse number, gated for the first time (m17.3c).
        .at_most("sim.collapse", core::PerfStat::Max, 12.0)
        .at_most("sim.block", core::PerfStat::P99, 6.0)
        // THE ATTRIBUTION RATCHET (m17.3c). A rule on the remainder is what stops the frame from
        // becoming un-attributable again: add a subsystem without a zone and its cost lands here,
        // where a number is watching, instead of vanishing into a frame total that still adds up.
        //
        // Two of the three limits are structural rather than measured, and they are honest about
        // it. `frame.unaccounted` is the loop's own bookkeeping between two stopwatches and
        // `frame.render.unaccounted` is `app.step`'s outside its four zones — both should be a
        // fraction of a millisecond, so 1.0 is generous by design: it must not fail on noise, only
        // on something real arriving unnamed. The sim's residual got the ratified sim budget
        // itself, which says the weakest useful thing — no unnamed remainder may be as large as
        // the entire simulation is allowed to be. Since p2-perf that budget is divided between the
        // three sim residuals (below), so together they still cannot exceed it.
        .at_most("frame.unaccounted", core::PerfStat::P99, 1.0)
        .at_most("frame.render.unaccounted", core::PerfStat::P99, 1.0)
        // Split three ways at p2-perf (ADR-0077) when the one residual became three; 2.0 apiece
        // is the old 6.0 divided, not each granted the whole of it.
        .at_most("sim.block.unaccounted", core::PerfStat::P99, 2.0)
        .at_most("sim.client.unaccounted", core::PerfStat::P99, 2.0)
        .at_most("sim.server.unaccounted", core::PerfStat::P99, 2.0)
        .require_samples("frame", 200)
        .require_samples("frame.collapse", 45)
        .require_samples("sim.collapse", 45)
        .require_samples("frame.unaccounted", 200)
        .require_samples("frame.render.unaccounted", 200)
        .require_samples("sim.block.unaccounted", 200)
        .require_samples("sim.client.unaccounted", 200)
        .require_samples("sim.server.unaccounted", 200)
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
    // The blocking submit split three ways, against the GPU's own pass total (p2-perf).
    if (const auto sub = report.distribution("frame.submit.per_frame")) {
        const auto wait = report.distribution("rhi.submit.wait.per_frame");
        const auto queue = report.distribution("rhi.submit.queue.per_frame");
        const auto gpu = report.distribution("gpu.passes");
        std::printf("  submit    p99 %.2f  max %.2f ms  = queue p99 %.2f + fence wait p99 %.2f "
                    "(max %.2f); gpu passes p99 %.2f  max %.2f ms\n",
                    sub->p99_ms,
                    sub->max_ms,
                    queue ? queue->p99_ms : 0.0,
                    wait ? wait->p99_ms : 0.0,
                    wait ? wait->max_ms : 0.0,
                    gpu ? gpu->p99_ms : 0.0,
                    gpu ? gpu->max_ms : 0.0);
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
    // WHERE THE SIMULATION WENT (m17.3b). Until this brick the answer was "nowhere in particular":
    // the engine's only profile zones were the Application's stage hooks, which this demo does not
    // use, so `sim.block`'s p99 had no breakdown of any kind and ADR-0035 §6's narrowphase
    // prediction was quoted as measured without ever being one.
    //
    // Ranked by the PER-FRAME total rather than the per-call one, because a frame steps the sim
    // several times and each step runs every stage: a per-call percentile times a call count is not
    // a per-frame percentile. Chosen by suffix rather than by a hardcoded list of stage names, so a
    // zone added anywhere in the engine shows up here without this sample being edited — the same
    // reason the work ledger prints itself whole.
    {
        std::vector<std::pair<std::string_view, double>> per_frame_zones;
        for (const std::string_view name : report.timelines()) {
            constexpr std::string_view kSuffix = ".per_frame";
            if (name.size() <= kSuffix.size() || !name.ends_with(kSuffix))
                continue;
            if (const auto d = report.distribution(name))
                per_frame_zones.emplace_back(name, d->p99_ms);
        }
        std::sort(per_frame_zones.begin(), per_frame_zones.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        if (!per_frame_zones.empty()) {
            std::printf("  where the CPU frame went (per-frame totals, p99, top %zu of %zu):\n",
                        std::min<std::size_t>(per_frame_zones.size(), 6),
                        per_frame_zones.size());
            for (std::size_t i = 0; i < per_frame_zones.size() && i < 6; ++i) {
                std::printf("    %-34.*s %8.3f ms\n",
                            static_cast<int>(per_frame_zones[i].first.size()),
                            per_frame_zones[i].first.data(),
                            per_frame_zones[i].second);
            }
        }
    }

    // WHAT THE FRAME COULD NOT NAME (m17.3c). Printed next to the breakdown it is the complement
    // of, because a decomposition read without its residual is the one that looks complete.
    {
        static constexpr std::string_view kResiduals[] = {"frame.unaccounted",
                                                          "frame.render.unaccounted",
                                                          "sim.block.unaccounted",
                                                          "sim.client.unaccounted",
                                                          "sim.server.unaccounted"};
        std::printf("  what the frame could NOT name (residual = parent - its named parts):\n");
        for (const std::string_view name : kResiduals) {
            const auto d = report.distribution(name);
            if (!d) {
                std::printf(
                    "    %-26.*s NOT RECORDED\n", static_cast<int>(name.size()), name.data());
                continue;
            }
            // `min` is in there deliberately: a NEGATIVE residual is not noise, it means the
            // declared tree is wrong — a part that is not actually inside its parent, or counted
            // twice — and nothing else in this printout would show it.
            std::printf("    %-26.*s p50 %7.3f  p99 %7.3f  max %7.3f  min %7.3f ms\n",
                        static_cast<int>(name.size()),
                        name.data(),
                        d->p50_ms,
                        d->p99_ms,
                        d->max_ms,
                        d->min_ms);
        }
        if (report.accounting_gaps() != 0) {
            std::printf("    (!) %llu accounting gaps — a declared part had no sample for its "
                        "frame, so every residual above is an OVERESTIMATE\n",
                        static_cast<unsigned long long>(report.accounting_gaps()));
        }
        if (zones.foreign_zones() != 0) {
            std::printf("    (!) %llu zone closes dropped from another thread — this report is "
                        "short by that many measurements\n",
                        static_cast<unsigned long long>(zones.foreign_zones()));
        }
    }

    std::printf("  worst frame #%llu at %.2f ms\n",
                static_cast<unsigned long long>(report.worst_frame().index),
                report.worst_frame().ms);
    // The CPU half of that frame, which the report carried nowhere until m17.3c: the worst frame is
    // the one a human opens after a failure, and it had a per-pass GPU breakdown and no answer at
    // all to "what was the CPU doing".
    {
        std::vector<core::ZoneTotal> worst_zones = report.worst_frame().zones;
        std::sort(worst_zones.begin(),
                  worst_zones.end(),
                  [](const core::ZoneTotal& a, const core::ZoneTotal& b) { return a.ms > b.ms; });
        for (std::size_t i = 0; i < worst_zones.size() && i < 4; ++i) {
            std::printf("    %-26s %8.3f ms\n", worst_zones[i].name.c_str(), worst_zones[i].ms);
        }
    }
    if (!timestamps_seen) {
        std::printf("  (this device reports no GPU timestamps — the per-pass table is empty)\n");
    }
    // A frame declaring more passes than the pool can bracket leaves the rest UNTIMED, and an
    // unattributed pass is indistinguishable in the report from a pass that cost nothing — the
    // exact confusion m17.3 exists to end. Printed unconditionally as a ratio rather than only on
    // overflow: "36 of 128" is how the next reader sees the headroom shrinking before it runs out,
    // which is the failure this line exists to prevent rather than to announce.
    if (opt.pipelined > 1) {
        // `on_post_submit` — the only hook that can see the graph itself — does not fire on the
        // pipelined path, so this run genuinely does not know. Say that, rather than print the 0
        // an unmeasured counter happens to hold: an unmeasured number that looks measured is the
        // failure this whole milestone is about.
        std::printf("  passes: not measured on the pipelined path (pool brackets %u)\n",
                    render::RenderGraph::max_timed_passes());
    } else {
        std::printf("  passes: %zu declared at peak, timestamp pool brackets %u\n",
                    max_passes_declared,
                    render::RenderGraph::max_timed_passes());
    }
    if (max_passes_declared > render::RenderGraph::max_timed_passes()) {
        std::printf("  (!) the peak frame OVERFLOWS the pool — everything past the first %u passes "
                    "is UNATTRIBUTED GPU time\n",
                    render::RenderGraph::max_timed_passes());
    }
    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());

    if (opt.out != nullptr) {
        std::FILE* f = std::fopen(opt.out, "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "  could not write %s\n", opt.out);
            return 1;
        }
        // The artefact carries what stdout warns of: a reader of the file cannot see the console.
        report.set_foreign_zones(zones.foreign_zones());
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

// The streamed-world proof uses the same Demo, Session tick and collision setup as live play.
// It keeps the charge disabled so the travelling floor, rather than rubble, is what it measures.
int run_world_smoke(const std::filesystem::path& cooked, std::string_view scene_path) {
    app::AppConfig cfg{};
    cfg.gpu = false;
    cfg.tick_hz = 60.0;
    Demo demo(cfg);
    demo.scripted_charge = false;
    demo.streamed_world = true;
    if (!demo.start(cooked, scene_path))
        return 1;

    bool okay = true;
    auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::fprintf(stderr, "world-smoke: FAILED %s\n", label);
            okay = false;
        }
    };
    check(demo.play_world->server().source.config().seed ==
                  demo.play_world->client().source.config().seed &&
              demo.play_world->server().source.config().version ==
                  demo.play_world->client().source.config().version,
          "peer seed/version agreement");

    // At the authored street centre, the slab must be the first downward static hit. The
    // procedural sample is deliberately below its top face.
    physics::BodyId street{};
    demo.session.server.world.query<ground::GroundBody>().for_each(
        [&](ecs::Entity, ground::GroundBody& g) { street = g.body; });
    physics::RayHit street_hit;
    physics::QueryFilter statics;
    statics.dynamics = false;
    const auto& c = demo.play_world->config();
    const physics::Ray street_ray{
        {0.5f * (c.street_min_x + c.street_max_x), 50.0f, 0.5f * (c.street_min_z + c.street_max_z)},
        {0.0f, -1.0f, 0.0f},
        100.0f};
    check(street.is_valid() &&
              demo.session.server.physics.raycast(street_ray, street_hit, statics) &&
              street_hit.body == street,
          "original street is first downward hit");

    replication::InputCommand west{};
    west.yaw = 1.57079632679f; // engine forward = -X
    west.move_y = 1.0f;
    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float min_client_x = std::numeric_limits<float>::infinity();
    float min_visual_x = std::numeric_limits<float>::infinity();
    float min_predicted_gap = std::numeric_limits<float>::infinity();
    float min_visual_gap = std::numeric_limits<float>::infinity();
    float max_visual_offset = 0.0f;
    auto travel = [&](int ticks) {
        for (int i = 0; i < ticks && okay; ++i) {
            demo.step_sim(west);
            check(!demo.session.world_stopped, "collision barrier remained ready");
            if (demo.session.server.avatars.empty())
                continue;
            const auto* s = demo.session.server.world.get<gameplay::CharacterState>(
                demo.session.server.avatars.front());
            if (!s)
                continue;
            min_x = std::min(min_x, s->position.x);
            min_y = std::min(min_y, s->position.y);
            check(std::isfinite(s->position.y) && s->position.y > -5.0f,
                  "player stayed on supported ground");
            const auto* character = demo.session.server.world.get<gameplay::CharacterConfig>(
                demo.session.server.avatars.front());
            physics::RayHit support;
            // A fast capsule may be launched well above a hill. Cast from above its current
            // position all the way below the world's minimum support height, so a legitimate
            // airborne tick does not turn a range-limited query into "missing ground".
            const float ray_top = std::max(s->position.y + 64.0f, 64.0f);
            const physics::Ray support_ray{
                {s->position.x, ray_top, s->position.z}, {0.0f, -1.0f, 0.0f}, ray_top + 64.0f};
            const bool found_support =
                demo.session.server.physics.raycast(support_ray, support, statics);
            if (!found_support) {
                const auto tx = static_cast<std::int32_t>(std::floor(s->position.x / 64.0f));
                const auto tz = static_cast<std::int32_t>(std::floor(s->position.z / 64.0f));
                const auto key = terrain_collision::TileKey{tx, tz, c.version};
                const auto sample = assets::procedural_terrain_sample(
                    c,
                    static_cast<std::int64_t>(std::floor(s->position.x)),
                    static_cast<std::int64_t>(std::floor(s->position.z)));
                std::fprintf(
                    stderr,
                    "world-smoke: support miss tick %llu at (%.9g, %.9g, %.9g), "
                    "tile (%d,%d) state %u body %u, sample %u, "
                    "installed %llu requests %llu failures %llu\n",
                    static_cast<unsigned long long>(demo.session.tick_index),
                    s->position.x,
                    s->position.y,
                    s->position.z,
                    tx,
                    tz,
                    static_cast<unsigned>(demo.play_world->server().collision.tile_state(key)),
                    demo.play_world->server().collision.tile_body(key).index,
                    sample ? static_cast<unsigned>(*sample) : 0u,
                    static_cast<unsigned long long>(
                        demo.play_world->server().collision.counters().installs),
                    static_cast<unsigned long long>(
                        demo.play_world->server().collision.counters().requests),
                    static_cast<unsigned long long>(
                        demo.play_world->server().collision.counters().load_failures));
            }
            check(character != nullptr && found_support,
                  "authoritative player has static support beneath current XZ");
            if (character && found_support) {
                const float feet = s->position.y - character->radius - character->half_height;
                check(feet >= support.point.y - 0.1f,
                      "capsule feet never pass through current static surface");
            }
            const auto local_avatar = demo.session.client.local_player();
            const auto* client_cfg =
                local_avatar.is_valid()
                    ? demo.session.client.world.get<gameplay::CharacterConfig>(local_avatar)
                    : nullptr;
            if (client_cfg && demo.session.client.predictor.seeded()) {
                const auto& predicted = demo.session.client.predictor.state();
                const auto visual = demo.session.client.predictor.visual_position();
                min_client_x = std::min(min_client_x, predicted.position.x);
                min_visual_x = std::min(min_visual_x, visual.x);
                max_visual_offset =
                    std::max(max_visual_offset, core::length(visual - predicted.position));
                const auto check_client_support = [&](core::Vec3 at, bool is_visual) {
                    const float top = std::max(at.y + 64.0f, 64.0f);
                    physics::RayHit hit;
                    const bool found = demo.session.client.physics.raycast(
                        physics::Ray{{at.x, top, at.z}, {0.0f, -1.0f, 0.0f}, top + 64.0f},
                        hit,
                        statics);
                    check(found,
                          is_visual ? "client visual terrain support exists"
                                    : "client predicted terrain support exists");
                    if (found) {
                        const float feet = at.y - client_cfg->half_height - client_cfg->radius;
                        if (is_visual) {
                            min_visual_gap = std::min(min_visual_gap, feet - hit.point.y);
                            check(feet >= hit.point.y - 0.1f,
                                  "client visual feet stay above terrain");
                            const float eye = demo.session.client.camera_transform().translation.y;
                            check(eye >= hit.point.y + 0.05f,
                                  "client camera stays above visual terrain");
                        } else {
                            min_predicted_gap = std::min(min_predicted_gap, feet - hit.point.y);
                            check(feet >= hit.point.y - 0.1f,
                                  "client predicted feet stay above terrain");
                        }
                    }
                };
                check_client_support(predicted.position, false);
                check_client_support(visual, true);
            }
        }
    };
    travel(150); // handshakes, then walking speed beyond the west end of the old slab
    check(min_x < static_cast<float>(c.street_min_x) - 2.0f,
          "walk-speed traversal crossed original slab");
    check(min_client_x < static_cast<float>(c.street_min_x) - 2.0f &&
              min_visual_x < static_cast<float>(c.street_min_x) - 2.0f,
          "client predicted and visual traversal crossed original slab");

    // This is an actual character configuration, consumed by gameplay and prediction. It is not
    // a camera teleport or a fabricated collision query.
    demo.session.server.character.max_speed = 100.0f;
    demo.session.server.character.accel = 500.0f;
    for (ecs::Entity e : demo.session.server.avatars) {
        if (auto* ch = demo.session.server.world.get<gameplay::CharacterConfig>(e)) {
            ch->max_speed = 100.0f;
            ch->accel = 500.0f;
        }
    }
    const ecs::Entity local = demo.session.client.local_player();
    if (auto* ch = local.is_valid()
                       ? demo.session.client.world.get<gameplay::CharacterConfig>(local)
                       : nullptr) {
        ch->max_speed = 100.0f;
        ch->accel = 500.0f;
    }
    demo.session.client.config.max_speed = 100.0f;
    demo.session.client.config.accel = 500.0f;
    const float stress_start = min_x;
    travel(360);
    check(min_x < stress_start - 300.0f && min_x < -300.0f,
          "100m/s control traversed hundreds of negative metres");
    check(min_client_x < stress_start - 280.0f && min_visual_x < stress_start - 280.0f,
          "client predicted and visual control traversed negative terrain");
    const core::Vec3 remote{min_x, 60.0f, 0.0f};
    physics::RayHit remote_hit;
    check(demo.session.server.physics.raycast(
              physics::Ray{remote, {0.0f, -1.0f, 0.0f}, 120.0f}, remote_hit, statics),
          "negative-coordinate terrain answers a physics ray");
    physics::RayHit missing_hit;
    check(!demo.session.server.physics.raycast(
              physics::Ray{{8193.0f, 64.0f, 8193.0f}, {0.0f, -1.0f, 0.0f}, 128.0f},
              missing_hit,
              statics),
          "missing support negative control remains observable");

    const auto& sc = demo.play_world->server().collision.counters();
    const auto& cc = demo.play_world->client().collision.counters();
    check(sc.evictions > 0 && cc.evictions > 0, "both collision peers evicted old tiles");
    check(sc.tiles_installed <= 256 && cc.tiles_installed <= 256 &&
              sc.peak_bytes_resident < 4ull * 1024ull * 1024ull &&
              cc.peak_bytes_resident < 4ull * 1024ull * 1024ull,
          "collision tile and byte gauges plateaued");
    check(demo.play_world->server().source.stats().refused_requests == 0 &&
              demo.play_world->client().source.stats().refused_requests == 0,
          "no source pool or coordinate refusal");

    // Inject two server-authoritative escape states; each must return to a supported spawn before
    // gameplay consumes another input. This is the explicit prototype safety boundary.
    if (!demo.session.server.avatars.empty()) {
        auto* s = demo.session.server.world.get<gameplay::CharacterState>(
            demo.session.server.avatars.front());
        s->position.x = block_play::PlayWorld::kPracticalLimit + 1.0f;
        demo.step_sim({});
        check(!demo.session.world_stopped && !block_play::needs_recovery(s->position),
              "practical X limit recovered safely");
        s->position.y = block_play::PlayWorld::kRecoveryFloor - 1.0f;
        demo.step_sim({});
        check(!demo.session.world_stopped && !block_play::needs_recovery(s->position) &&
                  demo.play_world->stats().recoveries >= 2,
              "absolute floor recovered safely");
    } else {
        check(false, "authoritative player spawned");
    }
    std::printf("world-smoke: min x %.1f m, min y %.1f m, server %llu installs/%llu evictions, "
                "client %llu installs/%llu evictions, recoveries %llu, client min x %.1f m, "
                "visual min x %.1f m, predicted/visual min feet gaps %.3f/%.3f m, "
                "max visual offset %.3f m\n",
                min_x,
                min_y,
                static_cast<unsigned long long>(sc.installs),
                static_cast<unsigned long long>(sc.evictions),
                static_cast<unsigned long long>(cc.installs),
                static_cast<unsigned long long>(cc.evictions),
                static_cast<unsigned long long>(demo.play_world->stats().recoveries),
                min_client_x,
                min_visual_x,
                min_predicted_gap,
                min_visual_gap,
                max_visual_offset);
    return okay ? 0 : 1;
}

// The GPU half of the streamed-world proof. --world-smoke has no device, so it never reaches the
// rolling terrain ring. This route starts the same streamed Demo with the charge off, then drives
// PlayWorld::recenter / pass / TerrainPass through the real render callback at sparse checkpoints
// (a full frame per checkpoint, not thousands), plus a dense recenter-only sweep. Every claim is
// structural: counters and pixel properties with margins, never a golden image.
int run_world_gpu_smoke(const std::filesystem::path& cooked, std::string_view scene_path) {
    constexpr std::uint32_t kW = 640;
    constexpr std::uint32_t kH = 360;
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kW, kH};
    cfg.tick_hz = 60.0;
    Demo demo(cfg);
    demo.scripted_charge = false;
    demo.streamed_world = true;
    if (demo.app.device() == nullptr) {
        std::fprintf(stderr, "world-gpu-smoke: no Vulkan device — skipped\n");
        return std::getenv("RIME_REQUIRE_VULKAN") != nullptr ? 1 : 0;
    }
    if (!demo.start(cooked, scene_path))
        return 1;
    demo.app.on_render([&demo](app::FrameContext& ctx) { demo.render(ctx); });
    if (auto* camera = demo.session.client.world.get<render::Camera>(demo.visuals->camera))
        play_sunset::configure_camera(*camera);

    bool okay = true;
    auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::fprintf(stderr, "world-gpu-smoke: FAILED %s\n", label);
            okay = false;
        }
    };
    block_play::PlayWorld& pw = *demo.play_world;
    constexpr float kEyeY = 60.0f; // above the 32 m hills, above the -54 m recovery floor

    // Pose the camera, draw `frames` real frames, return the readback of the last one.
    auto draw = [&](float x, float z, float pitch_deg, int frames) {
        gameplay::FirstPersonView v{};
        v.yaw = 1.57079632679f; // engine forward = -X, down the street's long axis
        v.pitch = pitch_deg * 0.0174532925f;
        v.eye_height = 0.0f;
        demo.visuals->authored_camera = gameplay::eye_transform(v, {x, kEyeY, z});
        demo.use_authored_camera = true;
        demo.app.run_frames(static_cast<std::uint32_t>(frames));
        std::vector<std::uint8_t> px;
        if (render::RenderGraph* graph = demo.app.graph()) {
            const rhi::TextureHandle tex = graph->physical(demo.visuals->last_ldr);
            if (tex.is_valid())
                px = read_rgba8(*demo.app.device(), tex, kW, kH);
        }
        demo.app.finish_gpu();
        return px;
    };

    struct Band {
        double luma = 0, sd = 0, black = 0;
        double rgb[3] = {0, 0, 0};
    };

    auto band = [&](const std::vector<std::uint8_t>& px, double r0, double r1) {
        Band b;
        const auto y0 = static_cast<std::uint32_t>(r0 * kH),
                   y1 = static_cast<std::uint32_t>(r1 * kH);
        double sum = 0, sq = 0, n = 0, dark = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = 0; x < kW; ++x) {
                const std::uint8_t* p = &px[(static_cast<std::size_t>(y) * kW + x) * 4];
                const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                sum += l;
                sq += l * l;
                n += 1;
                dark += (p[0] < 6 && p[1] < 6 && p[2] < 6) ? 1 : 0;
                for (int c = 0; c < 3; ++c)
                    b.rgb[c] += p[c];
            }
        b.luma = sum / n;
        b.sd = std::sqrt(std::max(0.0, sq / n - b.luma * b.luma));
        b.black = dark / n;
        for (double& c : b.rgb)
            c /= n;
        return b;
    };

    const struct {
        float x, z;
    } kCentres[] = {{0, 0},
                    {-64, 0},
                    {-128, 32},
                    {-512, -96},
                    {-2048, 160},
                    {-512, -96},
                    {0, 0},
                    {2048, -1024},
                    {4096, 3000},
                    {2048, -1024},
                    {-2048, 160},
                    {0, 0}};

    const char* ppm = g_ppm.empty() ? nullptr : g_ppm.c_str();
    int index = 0;
    for (const auto& c : kCentres) {
        const auto before = pw.stats();
        auto px = draw(c.x, c.z, -15.0f, 2);
        check(!demo.session.world_stopped, "world not stopped");
        check(px.size() == static_cast<std::size_t>(kW) * kH * 4, "readback size");
        if (px.size() != static_cast<std::size_t>(kW) * kH * 4)
            break;
        const Band sky = band(px, 0.0, 0.08), ground = band(px, 0.70, 1.0);
        const double diff = std::abs(sky.rgb[0] - ground.rgb[0]) +
                            std::abs(sky.rgb[1] - ground.rgb[1]) +
                            std::abs(sky.rgb[2] - ground.rgb[2]);
        const auto& s = pw.stats();
        std::printf(
            "world-gpu-smoke: [%2d] centre (%7.0f,%7.0f) draws %zu resident %u gpu %.1f MiB "
            "ground luma %.1f sd %.1f black %.3f | sky luma %.1f | rgb-diff %.1f\n",
            index,
            c.x,
            c.z,
            pw.draws().size(),
            s.resident,
            static_cast<double>(s.gpu_bytes) / 1048576.0,
            ground.luma,
            ground.sd,
            ground.black,
            sky.luma,
            diff);
        check(!pw.draws().empty(), "terrain draws nonempty");
        check(s.resident <= 304 && s.peak_resident <= 304, "resident <= 304");
        check(s.peak_gpu_bytes <= block_play::PlayWorld::kMaxGpuBytes, "gpu bytes <= 96 MiB");
        check(ground.black < 0.02, "no black void below the horizon");
        check(ground.luma > 8.0 && ground.sd > 1.0, "ground band is lit and not flat");
        check(diff > 6.0, "ground differs from sky");
        if (index == 0 && ppm)
            write_ppm(ppm, px, kW, kH);
        // A second draw at the unchanged centre must change nothing: no upload, eviction, recenter.
        const auto after = pw.stats();
        (void)draw(c.x, c.z, -15.0f, 2);
        const auto& again = pw.stats();
        check(again.uploads == after.uploads && again.evictions == after.evictions &&
                  again.recenters == after.recenters && again.resident == after.resident,
              "unchanged centre is a no-op");
        (void)before;
        ++index;
    }

    // SENSITIVITY CONTROL: at the farthest centre, look at the sky. The lower band must then stop
    // looking like ground, which is what shows the ground metric can fail at all.
    {
        const auto ground_px = draw(4096, 3000, -15.0f, 1);
        const auto sky_px = draw(4096, 3000, 25.0f, 1);
        if (!ground_px.empty() && !sky_px.empty()) {
            const Band g = band(ground_px, 0.70, 1.0), k = band(sky_px, 0.70, 1.0);
            const double d = std::abs(g.rgb[0] - k.rgb[0]) + std::abs(g.rgb[1] - k.rgb[1]) +
                             std::abs(g.rgb[2] - k.rgb[2]);
            std::printf("world-gpu-smoke: control lower-band ground vs skyward diff %.1f\n", d);
            check(d > 6.0, "sensitivity control: skyward view differs in the lower band");
        }
    }

    // Dense recenter-only sweep: the real recenter + mark_drawn at 16 m steps there and back,
    // across zero, with both reversals. No frame is rendered; the draw list is what is checked.
    const auto sweep_from = pw.stats();
    auto sweep = [&](float from, float to, float step) {
        for (float x = from; step > 0 ? x <= to : x >= to; x += step) {
            if (!pw.recenter({x, kEyeY, 0.5f * x})) {
                check(false, "sweep recenter accepted");
                return;
            }
            check(!pw.draws().empty() && pw.stats().resident <= 304, "sweep draws/resident");
            pw.mark_drawn();
        }
    };
    sweep(4096, -2048, -16.0f);
    sweep(-2048, 2048, 16.0f);
    sweep(2048, -64, -16.0f);
    const auto& fin = pw.stats();
    check(fin.uploads > sweep_from.uploads && fin.evictions > sweep_from.evictions,
          "sweep uploaded and evicted");
    check(fin.refusals == 0, "zero refusals");
    check(pw.server().source.stats().refused_requests == 0 &&
              pw.client().source.stats().refused_requests == 0,
          "zero collision source refusals");
    check(fin.peak_resident <= 304 && fin.peak_gpu_bytes <= block_play::PlayWorld::kMaxGpuBytes,
          "final peak bounds");
    check(fin.uploads > 0 && fin.evictions > 0 && fin.recenters > 0,
          "uploads, evictions, recenters");
    std::printf("world-gpu-smoke: uploads %llu evictions %llu recenters %llu refusals %llu peak "
                "resident %u peak gpu %.1f MiB\n",
                static_cast<unsigned long long>(fin.uploads),
                static_cast<unsigned long long>(fin.evictions),
                static_cast<unsigned long long>(fin.recenters),
                static_cast<unsigned long long>(fin.refusals),
                fin.peak_resident,
                static_cast<double>(fin.peak_gpu_bytes) / 1048576.0);
    demo.app.finish_gpu();
    return okay ? 0 : 1;
}

// Real GPU proof for the opt-in first-person art path. It compares readback with and without the
// overlay, then swaps to M4 and checks both visible coverage and fixed GPU residency. No window,
// input tape or world mutation is needed to make the two models draw.
int run_viewmodel_smoke(const std::filesystem::path& cooked, std::string_view scene_path) {
    constexpr std::uint32_t kW = 640;
    constexpr std::uint32_t kH = 360;
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kW, kH};
    cfg.tick_hz = 60.0;
    Demo demo(cfg);
    demo.scripted_charge = false;
    demo.viewmodel_enabled = true;
    // Stream the world, exactly as --play does. Without this the proof renders only the authored
    // street slab, whose finite edge leaves a band of sky-below-horizon -- correctly black, since
    // the atmosphere kills transmittance for rays that hit the planet -- across the frame. That
    // reads as a rendering bug and is really a weapon proof measuring a world no player sees.
    demo.streamed_world = true;
    demo.environment_enabled = true;
    if (!demo.app.device()) {
        std::fprintf(stderr, "viewmodel-smoke: no Vulkan device — skipped\n");
        return std::getenv("RIME_REQUIRE_VULKAN") != nullptr ? 1 : 0;
    }
    // Skip rather than fail when the art is absent, the same way the missing-Vulkan case above
    // does. This check has to come BEFORE start(), because start() treats absent art as benign and
    // quietly clears viewmodel_enabled -- without this the proof below would read a frame with no
    // model in it and still call every claim satisfied.
    if (!std::filesystem::exists(g_viewmodel_content / "manifest.txt")) {
        std::fprintf(stderr, "viewmodel-smoke: no cooked viewmodel art -- skipped\n");
        return std::getenv("RIME_REQUIRE_VIEWMODEL") != nullptr ? 1 : 0;
    }
    if (!demo.start(cooked, scene_path))
        return 1;
    if (!demo.viewmodel) {
        std::fprintf(stderr, "viewmodel-smoke: the viewmodel did not load\n");
        return 1;
    }
    // The same PLAY-ONLY evening preset run_play applies (play_sunset.hpp): the authored sun on
    // both peers, the view faced down the street at it, and the matching lowered exposure. Without
    // it this proof renders the DEFAULT rig -- a bright sky-dominated scene pointed at open ground
    // -- in which the sun contributes so little that killing every shadow in the frame moves 1.39%
    // of pixels by at most 10/255, against 19.8% and 99/255 in the block's own headless view. A
    // weapon is lit by whatever the player is standing in, so the proof has to stand there too.
    if (scene_path.empty()) {
        play_sunset::apply(demo.session.server.world);
        play_sunset::apply(demo.session.client.world);
    } // Saved maps own their lighting: Run must not silently replace an editor's sun edits.
    demo.session.client.view.yaw = play_sunset::kViewYaw;
    demo.session.client.view.pitch = play_sunset::kViewPitch;
    if (auto* camera = demo.session.client.world.get<render::Camera>(demo.visuals->camera)) {
        play_sunset::configure_camera(*camera);
    }
    if (demo.visuals) {
        demo.visuals->renderer.set_sky(play_sunset::sky(Visuals::authored_sky()));
    }
    demo.app.on_render([&demo](app::FrameContext& ctx) { demo.render(ctx); });
    for (int i = 0; i < 30; ++i)
        demo.step_sim({}); // replicate/seed the local camera
    if (!demo.warm_scope_view())
        return 1;

    bool okay = true;
    const auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::fprintf(stderr, "viewmodel-smoke: FAILED %s\n", label);
            okay = false;
        }
    };
    // Compare the ray with the actual client camera using the SAME capsule anchor: network
    // smoothing can legitimately separate client/server positions, but never their eye offsets.
    gameplay::CharacterState eye_state{};
    eye_state.position = demo.session.client.eye_anchor();
    gameplay::CharacterInput eye_input{};
    eye_input.pressed = gameplay::kActionFire;
    const auto eye_aim = gameplay::character_aim(eye_state, eye_input, demo.session.server.weapon);
    gameplay::ShotResult eye_shot{};
    (void)gameplay::step_weapon({},
                                eye_input,
                                demo.session.server.weapon,
                                eye_aim,
                                demo.session.server.physics,
                                {},
                                &eye_shot);
    check(eye_shot.fired &&
              core::length(eye_shot.shot.origin -
                           demo.session.client.camera_transform().translation) <= .001f,
          "shot origin equals camera eye to one millimetre at the same capsule pose");
    check(demo.visuals->scope_renderer && !demo.visuals->scope_renderer->taa_history_allocated(),
          "scope loading primes resources without retaining a stale aiming image");
    const auto capture = [&]() {
        demo.app.run_frames(4);
        std::vector<std::uint8_t> pixels;
        if (render::RenderGraph* graph = demo.app.graph()) {
            const auto tex = graph->physical(demo.visuals->last_ldr);
            if (tex.is_valid())
                pixels = read_rgba8(*demo.app.device(), tex, kW, kH);
        }
        demo.app.finish_gpu();
        return pixels;
    };
    demo.viewmodel_enabled = false;
    demo.app.run_frames(64); // fill DDGI/temporal histories before comparing model pixels
    const auto baseline = capture();
    const auto baseline_control = capture();
    demo.viewmodel_enabled = true;
    // Appearing invalidates the overlay's TAA history by design (the identity epoch), so the first
    // frames after a state change are deliberately un-converged. Measuring there would read
    // convergence as drift: under the evening rig it put the unchanged-control at 37965 px when the
    // bright default rig had shown 632. Let the temporal passes settle, THEN measure.
    demo.app.run_frames(48);
    const auto scar = capture();
    const auto scar_control = capture();
    const auto initial_epoch = demo.viewmodel->identity_epoch();
    const auto meshes = demo.viewmodel->resident_meshes();
    const auto materials = demo.viewmodel->resident_materials();
    const auto textures = demo.viewmodel->resident_textures();
    check(demo.viewmodel->selected() == block_play::PlayViewmodel::Weapon::Scar &&
              demo.viewmodel->selected_draw_count() > 0,
          "SCAR-H is the default resident draw");
    demo.viewmodel->select(block_play::PlayViewmodel::Weapon::M4);
    demo.app.run_frames(48); // same reason: the swap resets history too
    const auto m4 = capture();
    if (!g_ppm.empty() && !scar.empty() && !m4.empty()) {
        write_ppm((g_ppm + ".scar.ppm").c_str(), scar, kW, kH);
        write_ppm((g_ppm + ".m4.ppm").c_str(), m4, kW, kH);
    }
    float max_grip_error = 0.0f;
    float max_grip_angle = 0.0f;
    float max_skin_motion = 0.0f;
    bool grips_valid = true;
    for (auto weapon : {block_play::PlayViewmodel::Weapon::Scar,
                        block_play::PlayViewmodel::Weapon::M4,
                        block_play::PlayViewmodel::Weapon::Sniper}) {
        demo.viewmodel->select(weapon);
        for (int step = 0; step < 8; ++step) {
            demo.viewmodel->advance(1.0f / 60.0f, {4, 0, 2}, step % 3 == 0);
            demo.app.run_frames(1);
            const auto& stats = demo.viewmodel->arm_stats();
            grips_valid = grips_valid && stats.valid_input && stats.clamped_targets == 0;
            for (unsigned side = 0; side < 2; ++side) {
                max_grip_error = std::max(max_grip_error, stats.wrist_position_error[side]);
                max_grip_angle = std::max(max_grip_angle, stats.wrist_orientation_error[side]);
            }
            max_skin_motion = std::max(max_skin_motion, stats.max_vertex_displacement);
        }
    }
    check(grips_valid && max_grip_error < .0001f && max_grip_angle < .002f,
          "real weighted arms keep both grip frames through bob, recoil and swaps");
    check(max_skin_motion > .001f &&
              demo.visuals->renderer.rejected_first_person_deformations() == 0,
          "IK visibly deforms actual vertices, with no refused uploads");
    // Exercise the live event fold and the SAME sample controls as --play. Calling set_aiming
    // directly missed both the captured-pointer gate and the rifles' ignored aim input.
    platform::Input aim_devices;
    gameplay::FirstPersonView aim_view{};
    gameplay::InputBindings aim_bindings{};
    const auto control_frame = [&](std::span<const platform::Event> events) {
        (void)gameplay::map_frame_input(aim_devices, events, aim_view, aim_bindings);
        demo.viewmodel->apply_controls(aim_devices, events);
        demo.viewmodel->advance(1.0f / 60.0f, {}, false);
    };
    const auto settle_controls = [&]() {
        for (int i = 0; i < 64; ++i)
            control_frame({}); // held buttons must survive frames containing no events
    };
    for (const auto key : {platform::Key::Num1, platform::Key::Num2, platform::Key::Num3}) {
        platform::Event down{};
        down.type = platform::EventType::KeyDown;
        down.key.key = key;
        auto up = down;
        up.type = platform::EventType::KeyUp;
        const std::array tap{down, up};
        control_frame(tap);
        const auto expected = key == platform::Key::Num1 ? block_play::PlayViewmodel::Weapon::Scar
                              : key == platform::Key::Num2
                                  ? block_play::PlayViewmodel::Weapon::M4
                                  : block_play::PlayViewmodel::Weapon::Sniper;
        check(demo.viewmodel->selected() == expected,
              "a complete weapon-selection tap in one frame selects the requested weapon");
    }
    platform::Event right_down{};
    right_down.type = platform::EventType::MouseButton;
    right_down.button.button = platform::MouseButton::Right;
    right_down.button.down = true;
    auto right_up = right_down;
    right_up.button.down = false;
    platform::Event motion{};
    motion.type = platform::EventType::MouseMove;
    motion.mouse_move.dx = 12.0f;
    const auto eye_pose = demo.session.client.camera_transform();
    const auto weapon_origin = [&]() {
        for (const auto& draw : demo.viewmodel->pose(eye_pose))
            if (draw.mesh == demo.viewmodel->selected_mesh())
                return core::transform_point(draw.model, {});
        return core::Vec3{};
    };
    for (bool drag : {false, true}) {
        aim_bindings.look_requires_drag = drag;
        for (auto key : {platform::Key::Num1, platform::Key::Num2, platform::Key::Num3}) {
            platform::Event select{};
            select.type = platform::EventType::KeyDown;
            select.key.key = key;
            control_frame(std::span(&select, 1));
            const auto hip_origin = weapon_origin();
            const float initial_yaw = aim_view.yaw;
            // In fallback, motion deliberately arrives BEFORE the button in the event batch.
            const std::array press =
                drag ? std::array{motion, right_down} : std::array{right_down, motion};
            control_frame(press);
            settle_controls();
            const auto aim_origin = weapon_origin();
            if (!drag && key != platform::Key::Num3 && !g_ppm.empty()) {
                demo.app.run_frames(48);
                const auto aimed = capture();
                const auto suffix = key == platform::Key::Num1 ? ".scar-aim.ppm" : ".m4-aim.ppm";
                write_ppm((g_ppm + suffix).c_str(), aimed, kW, kH);
            }
            check(aim_devices.mouse_down(platform::MouseButton::Right) &&
                      demo.viewmodel->aim_fraction() > .99f &&
                      core::length(aim_origin - hip_origin) > .08f && aim_view.yaw != initial_yaw,
                  "right hold aims every weapon and preserves look in locked and drag modes");
            const auto& stats = demo.viewmodel->arm_stats();
            check(stats.valid_input && stats.clamped_targets == 0 &&
                      stats.wrist_position_error[0] < .0001f &&
                      stats.wrist_position_error[1] < .0001f,
                  "each held aim pose remains inside the rigged arms' reachable workspace");
            select.type = platform::EventType::KeyUp;
            const std::array release{right_up, select};
            control_frame(release);
            settle_controls();
            check(demo.viewmodel->aim_fraction() < .001f,
                  "right release returns every weapon to its hip pose");
        }
        platform::Event q{};
        q.type = platform::EventType::KeyDown;
        q.key.key = platform::Key::Q;
        control_frame(std::span(&q, 1));
        settle_controls();
        check(demo.viewmodel->aim_fraction() > .99f, "Q remains a held aim alternative");
        q.type = platform::EventType::KeyUp;
        control_frame(std::span(&q, 1));
        settle_controls();
        check(demo.viewmodel->aim_fraction() < .001f, "Q release ends aiming");
    }
    const auto closed_pupil_frames = demo.scope_feed_frames;
    (void)capture();
    check(!demo.viewmodel->scope_feed_visible(demo.session.client.camera_transform()) &&
              demo.scope_feed_frames == closed_pupil_frames,
          "closed hip-fire pupil keeps glass without rendering an invisible optical world");
    control_frame(std::span(&right_down, 1));
    for (int i = 0; i < 120; ++i)
        control_frame({});
    const auto* parent_camera = demo.session.client.world.get<render::Camera>(demo.visuals->camera);
    const float parent_fov = parent_camera ? parent_camera->fov_y : 0.0f;
    demo.app.run_frames(48);
    const auto sniper = capture();
    const auto& aimed_stats = demo.viewmodel->arm_stats();
    check(aimed_stats.valid_input && aimed_stats.clamped_targets == 0 &&
              aimed_stats.wrist_position_error[0] < .0001f &&
              aimed_stats.wrist_position_error[1] < .0001f,
          "ADS remains within both arms' reachable workspace");
    check(demo.scope_feed_frames > closed_pupil_frames && demo.visuals->scope_renderer &&
              parent_camera && parent_camera->fov_y == parent_fov && sniper.size() == scar.size(),
          "independent optical renderer draws without changing the parent FOV");
    if (demo.visuals->scope_renderer) {
        const auto& main_vp = demo.visuals->renderer.view_proj_unjittered();
        const auto& optic_vp = demo.visuals->scope_renderer->view_proj_unjittered();
        const auto optic_world = demo.viewmodel->optic_world();
        const auto project = [](const core::Mat4& vp, core::Vec3 point) {
            const auto clip = vp * core::Vec4{point.x, point.y, point.z, 1.0f};
            return core::Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
        };
        // Project a real target pair at 100 m, then place its optical image onto the physical
        // ocular and project THAT through the parent camera. This measures the apparent zoom
        // the player sees, including aperture size; a feed-camera focal ratio does not.
        const auto axis = core::transform_point(optic_world, {0, 0, -100});
        const auto target = core::transform_point(optic_world, {0, 1, -100});
        const float direct_span = std::abs(project(main_vp, target).y - project(main_vp, axis).y);
        const float radius = block_play::PlayViewmodel::kScopeLensRadius;
        const auto image_axis =
            core::transform_point(optic_world, {0, -radius * project(optic_vp, axis).y, 0});
        const auto image_target =
            core::transform_point(optic_world, {0, -radius * project(optic_vp, target).y, 0});
        const float optical_span =
            std::abs(project(main_vp, image_target).y - project(main_vp, image_axis).y);
        const float measured_zoom = optical_span / direct_span;
        check(std::abs(measured_zoom - block_play::PlayViewmodel::kScopeMagnification) < .02f,
              "real target projected through the physical ocular measures 5x apparent zoom");
        std::printf("viewmodel-smoke: physical ocular apparent magnification %.5fx\n",
                    measured_zoom);
    }
    if (!g_ppm.empty() && !sniper.empty())
        write_ppm((g_ppm + ".sniper.ppm").c_str(), sniper, kW, kH);
    if (!g_ppm.empty() && demo.visuals->last_scope_ldr.is_valid()) {
        auto* graph = demo.app.graph();
        const auto pixels =
            read_rgba8(*demo.app.device(), graph->physical(demo.visuals->last_scope_ldr), 512, 512);
        write_ppm((g_ppm + ".optical.ppm").c_str(), pixels, 512, 512);
        const auto eye_local =
            core::transform_point(core::inverse(demo.viewmodel->optic_world()),
                                  demo.session.client.camera_transform().translation);
        std::printf("viewmodel-smoke: eye in optic %.6f %.6f %.6f\n",
                    eye_local.x,
                    eye_local.y,
                    eye_local.z);
    }
    std::printf("viewmodel-smoke: grip error %.6f m, angle %.6f rad, skin motion %.6f m\n",
                max_grip_error,
                max_grip_angle,
                max_skin_motion);
    demo.scope_transmission = 0.0f;
    demo.app.run_frames(48); // same image/pose/reflections, only transmitted input removed
    const auto no_transmission = capture();
    std::size_t optical_pixels = 0, optical_changed = 0;
    if (sniper.size() == static_cast<std::size_t>(kW) * kH * 4 &&
        no_transmission.size() == sniper.size()) {
        const float radius = .75f *
                             (block_play::PlayViewmodel::kScopeLensRadius /
                              block_play::PlayViewmodel::kScopeEyeRelief) /
                             std::tan(parent_fov * .5f) * kH * .5f;
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const float dx = static_cast<float>(x) - kW * .5f;
                const float dy = static_cast<float>(y) - kH * .5f;
                if (dx * dx + dy * dy > radius * radius)
                    continue;
                const std::size_t p = (static_cast<std::size_t>(y) * kW + x) * 4;
                int delta = 0;
                for (int c = 0; c < 3; ++c)
                    delta += std::abs(int(sniper[p + c]) - int(no_transmission[p + c]));
                ++optical_pixels;
                optical_changed += delta >= 30 ? 1u : 0u;
            }
        }
    }
    check(optical_pixels > 100 && optical_changed > optical_pixels / 4,
          "transmitted optical input visibly reaches the real ocular through main depth and TAA");
    std::printf("viewmodel-smoke: optical transmission control changes %zu / %zu lens pixels\n",
                optical_changed,
                optical_pixels);
    demo.scope_transmission = 1.0f;
    demo.viewmodel_enabled = false;
    demo.app.run_frames(48);
    const auto baseline_again = capture(); // temporal/lighting drift control
    check(demo.viewmodel->identity_epoch() > initial_epoch &&
              demo.viewmodel->selected_draw_count() > 0,
          "M4 swap invalidates first-person history and draws");
    check(meshes == 11 && materials >= 2 && textures >= 4 &&
              demo.viewmodel->resident_meshes() == meshes &&
              demo.viewmodel->resident_materials() == materials &&
              demo.viewmodel->resident_textures() == textures,
          "both models and maps were loaded once and stayed resident");

    const auto changed = [&](const std::vector<std::uint8_t>& a,
                             const std::vector<std::uint8_t>& b) {
        std::size_t count = 0;
        if (a.size() != static_cast<std::size_t>(kW) * kH * 4 || b.size() != a.size())
            return count;
        for (std::size_t pixel = 0; pixel < a.size() / 4; ++pixel) {
            const std::size_t p = pixel * 4;
            const int rgb = std::abs(int(a[p]) - int(b[p])) +
                            std::abs(int(a[p + 1]) - int(b[p + 1])) +
                            std::abs(int(a[p + 2]) - int(b[p + 2]));
            count += rgb >= 30 ? 1u : 0u;
        }
        return count;
    };
    const auto scar_coverage = changed(baseline, scar);
    const auto swap_coverage = changed(scar_control, m4);
    const auto baseline_drift = changed(baseline, baseline_control);
    const auto scar_drift = changed(scar, scar_control);
    const auto after_overlay_residual = changed(baseline, baseline_again);
    std::printf("viewmodel-smoke: SCAR coverage %zu px, M4 swap %zu px, "
                "baseline drift %zu px, SCAR drift %zu px, removal residual %zu px, "
                "meshes/materials/textures %zu/%zu/%zu\n",
                scar_coverage,
                swap_coverage,
                baseline_drift,
                scar_drift,
                after_overlay_residual,
                meshes,
                materials,
                textures);
    check(baseline.size() == static_cast<std::size_t>(kW) * kH * 4 &&
              scar.size() == baseline.size() && scar_control.size() == baseline.size() &&
              m4.size() == baseline.size() && baseline_again.size() == baseline.size() &&
              baseline_control.size() == baseline.size(),
          "all six frames read back");
    check(scar_coverage > static_cast<std::size_t>(kW) * kH / 200,
          "SCAR changes at least 0.5 percent of the frame");
    check(swap_coverage > static_cast<std::size_t>(kW) * kH / 200,
          "M4 swap changes at least 0.5 percent of the frame");
    check(scar_coverage > baseline_drift * 3 && swap_coverage > scar_drift * 3,
          "model coverage and swap exceed their unchanged controls");

    // A matrix ratio alone cannot prove that the feed reaches the lens at that scale. Render
    // a real magenta target through BOTH paths and measure its pixel silhouette after TAA.
    // Six metres keeps the target in the empty street, ahead of the weapon and nearby walls.
    render::PbrMaterialDesc target_material{};
    target_material.base_color[0] = target_material.base_color[1] = target_material.base_color[2] =
        0.0f;
    target_material.emissive[0] = target_material.emissive[2] = 8.0f;
    const auto target_mesh =
        demo.visuals->meshes.add(render::make_cube(.5f), "optical proof target");
    const auto target_mat = demo.visuals->materials.add(target_material);
    core::Transform target_pose = eye_pose;
    target_pose.translation = core::transform_point(core::to_matrix(eye_pose), {.22f, .12f, -6.0f});
    target_pose.scale = {.16f, .16f, .015f};
    const auto target_entity =
        demo.session.client.world.spawn_with(ecs::WorldTransform{target_pose},
                                             render::MeshRef{target_mesh},
                                             render::MaterialRef{target_mat});
    demo.development_hud = false;
    demo.app.run_frames(48);
    const auto direct_target = capture();
    demo.viewmodel_enabled = true;
    demo.viewmodel->set_aiming(true);
    for (int i = 0; i < 64; ++i)
        demo.viewmodel->advance(1.0f / 60.0f, {}, false);
    demo.app.run_frames(48);
    const auto scoped_target = capture();
    const auto target_width = [&](const auto& pixels) {
        int left = static_cast<int>(kW), right = -1;
        if (pixels.size() != kW * kH * 4)
            return 0;
        for (unsigned y = kH / 4; y < kH * 3 / 4; ++y)
            for (unsigned x = kW / 4; x < kW * 3 / 4; ++x) {
                const auto p = (static_cast<std::size_t>(y) * kW + x) * 4;
                if (pixels[p] > 100 && pixels[p + 2] > 80 &&
                    int(pixels[p]) > int(pixels[p + 1]) + 60 &&
                    int(pixels[p + 2]) > int(pixels[p + 1]) + 30) {
                    left = std::min(left, static_cast<int>(x));
                    right = std::max(right, static_cast<int>(x));
                }
            }
        return right >= left ? right - left + 1 : 0;
    };
    const int direct_width = target_width(direct_target);
    const int scoped_width = target_width(scoped_target);
    const float pixel_zoom = direct_width > 0 ? float(scoped_width) / float(direct_width) : 0;
    check(direct_width >= 5 && scoped_width >= 30 && pixel_zoom > 4.3f && pixel_zoom < 5.8f,
          "rendered target silhouette is visibly magnified about 5x through the real lens");
    std::printf("viewmodel-smoke: rendered target %d -> %d pixels (%.3fx); eye relief %.0f mm\n",
                direct_width,
                scoped_width,
                pixel_zoom,
                block_play::PlayViewmodel::kScopeEyeRelief * 1000.0f);
    if (!g_ppm.empty()) {
        write_ppm((g_ppm + ".target-direct.ppm").c_str(), direct_target, kW, kH);
        write_ppm((g_ppm + ".target-scoped.ppm").c_str(), scoped_target, kW, kH);
    }
    (void)demo.session.client.world.despawn(target_entity);
    // Replay an accepted shot through the actual optic/arm pose. The angular witness is
    // independent of translation: a moving ocular alone cannot prove muzzle rise. A second
    // identical input sequence must reproduce every pose, including the hashed yaw/roll.
    const auto scoped_recoil = [&]() {
        std::array<core::Mat4, 37> poses{};
        demo.viewmodel->select(block_play::PlayViewmodel::Weapon::M4);
        demo.viewmodel->select(block_play::PlayViewmodel::Weapon::Sniper);
        demo.viewmodel->set_aiming(true);
        demo.viewmodel->advance(1.0f, {}, false);
        for (std::size_t i = 0; i < poses.size(); ++i) {
            if (i != 0)
                demo.viewmodel->advance(1.0f / 60.0f, {}, i == 1);
            (void)demo.viewmodel->pose(eye_pose);
            poses[i] = demo.viewmodel->optic_world();
            check(!demo.viewmodel->cycling() && !demo.viewmodel->take_case_ejection(),
                  "scoped recoil never starts the deferred bolt or extracts a case");
            const auto& arms = demo.viewmodel->arm_stats();
            check(arms.valid_input && arms.clamped_targets == 0,
                  "scoped recoil keeps both arm targets reachable");
        }
        return poses;
    };
    const auto shot_poses = scoped_recoil();
    const auto replay_poses = scoped_recoil();
    const auto optic_angle = [&](const core::Mat4& pose) {
        const auto a = core::transform_vector(shot_poses.front(), {0, 0, -1});
        const auto b = core::transform_vector(pose, {0, 0, -1});
        return std::atan2(core::length(core::cross(a, b)), core::dot(a, b));
    };
    float peak_angle = 0.0f;
    for (std::size_t i = 0; i < shot_poses.size(); ++i) {
        peak_angle = std::max(peak_angle, optic_angle(shot_poses[i]));
        check(core::approx_eq(shot_poses[i], replay_poses[i], 1e-6f),
              "identical scoped shot inputs reproduce the same optic pose");
    }
    const float recovery_angle = optic_angle(shot_poses.back());
    const float recovery_position = core::length(core::transform_point(shot_poses.back(), {}) -
                                                 core::transform_point(shot_poses.front(), {}));
    check(peak_angle > .00872665f,
          "accepted scoped shot kicks optic axis by more than 0.5 degrees");
    check(recovery_angle < .00174533f && recovery_position < .001f && demo.viewmodel->busy() &&
              !demo.viewmodel->cycling() && demo.viewmodel->aim_fraction() > .99f &&
              demo.viewmodel->scope_feed_visible(eye_pose),
          "scoped optic recovers within 0.1 degrees and 1 mm by 0.6 seconds, bolt still deferred");
    std::printf("viewmodel-smoke: scoped recoil peak %.6f rad, at 0.6 s %.6f rad / %.6f m\n",
                peak_angle,
                recovery_angle,
                recovery_position);
    // The render helper is the same seam used by the parent camera and the scoped optic.
    // Sample every weapon's full recovery, and independently observe the immutable aim vector.
    for (const auto weapon : {block_play::PlayViewmodel::Weapon::Scar,
                              block_play::PlayViewmodel::Weapon::M4,
                              block_play::PlayViewmodel::Weapon::Sniper}) {
        demo.viewmodel->select(weapon);
        demo.viewmodel->set_aiming(true);
        demo.viewmodel->advance(1.0f, {}, false);
        const auto simulation_view = demo.session.client.view;
        const auto aim = gameplay::look_direction(simulation_view);
        float peak_pitch = 0, recovered_pitch = 0;
        for (unsigned frame = 0; frame < 36; ++frame) {
            demo.viewmodel->advance(kDt, {}, frame == 0);
            const auto rendered =
                core::rotate(demo.render_camera_transform().rotation, core::Vec3{0, 0, -1});
            recovered_pitch = std::abs(std::asin(rendered.y) - std::asin(aim.y));
            peak_pitch = std::max(peak_pitch, recovered_pitch);
            check(core::length(gameplay::look_direction(demo.session.client.view) - aim) == 0 &&
                      demo.session.client.view.yaw == simulation_view.yaw &&
                      demo.session.client.view.pitch == simulation_view.pitch,
                  "view punch never changes simulation aim");
        }
        check(peak_pitch > .001f && recovered_pitch < .001f,
              "render camera pitch kicks and recovers by 0.6 seconds for every weapon");
        std::printf("viewmodel-smoke: camera weapon %u peak %.6f recovery %.6f rad\n",
                    static_cast<unsigned>(weapon),
                    peak_pitch,
                    recovered_pitch);
    }
    demo.viewmodel->set_aiming(false);
    demo.viewmodel->advance(0.0f, {}, false);
    check(demo.viewmodel->cycling() && !demo.viewmodel->take_case_ejection(),
          "scope release starts the deferred bolt exactly once");
    demo.viewmodel->advance(block_play::PlayWeaponMotion::kExtractionSeconds, {}, false);
    check(demo.viewmodel->take_case_ejection() && !demo.viewmodel->take_case_ejection(),
          "deferred scoped bolt emits exactly one case at extraction");
    demo.viewmodel->advance(2.0f, {}, false);
    // Sweep every frame of each action through the REAL rig before any image is read. One
    // rendered sample can say only that a claim failed; this says which term and when. The arm
    // solve and the weapon pose are camera-local, so pose() with an identity camera needs no GPU
    // and measures exactly what render() hands the renderer.
    //
    // The deltas are against the idle pose solved immediately before the action, NOT against
    // the bind mesh: PlayArmIk's max_vertex_displacement is measured from bind on every solve,
    // so it is already large at idle (the rig is re-seated on the shoulder) and cannot by itself
    // show that an action moved anything. The idle CONTROL sweep -- the same clock with no
    // action -- must read zero, which proves the action deltas are motion and not noise.
    using VmWeapon = block_play::PlayViewmodel::Weapon;
    const auto weapon_name = [](VmWeapon w) {
        return w == VmWeapon::Scar ? "SCAR" : w == VmWeapon::M4 ? "M4" : "MSR";
    };

    struct ActionSweep {
        int frames = 0, busy = 0, invalid = 0, clamped = 0;
        float first_clamp = -1.0f; // seconds into the action
        float wrist_miss[2]{};     // requested-vs-reached wrist, metres; above zero only if clamped
        float min_skin = 1e9f, arm_delta = 0.0f, muzzle_delta = 0.0f;
        // The reach limiter's own record: frames it shortened a hand path, the smallest share of
        // the authored path kept, and the longest hand travel actually applied (weapon metres).
        int limited = 0;
        int contact_frames = 0;
        float contact_gap = 0;
        float retained[2]{1.0f, 1.0f}, hand_travel[2]{};
    };

    const auto sweep = [&](float seconds, bool fire_first, float measure_from) {
        ActionSweep r;
        const core::Transform camera{};
        (void)demo.viewmodel->pose(camera);
        std::vector<render::MeshVertex> idle;
        if (!demo.viewmodel->deformations().empty()) {
            const auto v = demo.viewmodel->deformations().front().current;
            idle.assign(v.begin(), v.end());
        }
        const auto idle_muzzle = demo.viewmodel->muzzle_world(camera);
        constexpr float dt = 1.0f / 60.0f;
        for (; static_cast<float>(r.frames) * dt < seconds; ++r.frames) {
            demo.viewmodel->advance(dt, {}, fire_first && r.frames == 0);
            const float t = static_cast<float>(r.frames + 1) * dt; // action time after advance
            r.busy += demo.viewmodel->busy() ? 1 : 0;
            (void)demo.viewmodel->pose(camera);
            const auto& st = demo.viewmodel->arm_stats();
            if (demo.viewmodel->magazine_grasped()) {
                ++r.contact_frames;
                r.contact_gap = std::max(r.contact_gap, demo.viewmodel->magazine_contact_gap());
            }
            r.invalid += st.valid_input ? 0 : 1;
            if (st.clamped_targets != 0) {
                ++r.clamped;
                if (r.first_clamp < 0.0f)
                    r.first_clamp = t;
            }
            for (unsigned side = 0; side < 2; ++side)
                r.wrist_miss[side] = std::max(r.wrist_miss[side], st.wrist_position_error[side]);
            r.min_skin = std::min(r.min_skin, st.max_vertex_displacement);
            const auto& reach = demo.viewmodel->reach_stats();
            r.limited += reach.retained[0] < 1.0f || reach.retained[1] < 1.0f ? 1 : 0;
            for (unsigned side = 0; side < 2; ++side) {
                r.retained[side] = std::min(r.retained[side], reach.retained[side]);
                r.hand_travel[side] = std::max(r.hand_travel[side], reach.applied_offset[side]);
            }
            if (t < measure_from || demo.viewmodel->deformations().empty())
                continue;
            const auto v = demo.viewmodel->deformations().front().current;
            for (std::size_t i = 0; i < std::min(v.size(), idle.size()); ++i)
                r.arm_delta = std::max(r.arm_delta,
                                       core::length(core::Vec3{v[i].px - idle[i].px,
                                                               v[i].py - idle[i].py,
                                                               v[i].pz - idle[i].pz}));
            r.muzzle_delta = std::max(
                r.muzzle_delta, core::length(demo.viewmodel->muzzle_world(camera) - idle_muzzle));
        }
        return r;
    };
    const auto report_sweep = [&](const char* what, VmWeapon w, const ActionSweep& r) {
        std::printf("viewmodel-smoke: %s %s sweep %d frames, busy %d, invalid %d, clamped %d "
                    "(first %.3f s), wrist miss L %.2f R %.2f mm, min bind skin %.4f m, arm "
                    "delta %.4f m, muzzle delta %.4f m; reach-limited %d frames, path kept L %.3f "
                    "R %.3f, hand travel L %.4f R %.4f m\n",
                    weapon_name(w),
                    what,
                    r.frames,
                    r.busy,
                    r.invalid,
                    r.clamped,
                    r.first_clamp,
                    r.wrist_miss[0] * 1000.0f,
                    r.wrist_miss[1] * 1000.0f,
                    r.min_skin,
                    r.arm_delta,
                    r.muzzle_delta,
                    r.limited,
                    r.retained[0],
                    r.retained[1],
                    r.hand_travel[0],
                    r.hand_travel[1]);
    };
    for (const auto weapon : {VmWeapon::Scar, VmWeapon::M4, VmWeapon::Sniper}) {
        demo.viewmodel->select(weapon == VmWeapon::M4 ? VmWeapon::Scar : VmWeapon::M4);
        demo.viewmodel->select(weapon);
        demo.viewmodel->set_aiming(false);
        demo.viewmodel->advance(1.0f, {}, false);
        const auto limits = demo.viewmodel->reach_limits();
        std::printf("viewmodel-smoke: %s measured reach L %.4f..%.4f R %.4f..%.4f m\n",
                    weapon_name(weapon),
                    limits[0][0],
                    limits[0][1],
                    limits[1][0],
                    limits[1][1]);
        const auto control = sweep(.5f, false, 0.0f);
        report_sweep("idle-control", weapon, control);
        check(control.busy == 0 && control.invalid == 0 && control.clamped == 0 &&
                  control.arm_delta < 1e-6f && control.muzzle_delta < 1e-6f &&
                  control.limited == 0 && control.hand_travel[0] == 0.0f &&
                  control.hand_travel[1] == 0.0f,
              "idle control sweep reads no action motion");
        check(demo.viewmodel->begin_reload(), "idle weapon accepts a swept reload");
        const float reload_seconds = weapon == VmWeapon::Sniper
                                         ? block_play::PlayWeaponMotion::kSniperReloadSeconds
                                         : block_play::PlayWeaponMotion::kRifleReloadSeconds;
        const auto reload = sweep(reload_seconds + .2f, false, 0.0f);
        report_sweep("reload", weapon, reload);
        check(reload.invalid == 0 && reload.clamped == 0,
              "every reload frame keeps both hands on reachable, valid targets");
        std::printf("viewmodel-smoke: %s magazine contact %d frames, max gap %.3f mm\n",
                    weapon_name(weapon),
                    reload.contact_frames,
                    reload.contact_gap * 1000);
        check(reload.contact_frames > 30 && reload.contact_gap < 1e-4f,
              "support wrist follows the detached magazine throughout grasp and reseating");
        check(reload.busy >= static_cast<int>(reload_seconds * 60.0f) - 1 &&
                  !demo.viewmodel->busy() && reload.arm_delta > .03f && reload.muzzle_delta > .05f,
              "the swept reload moves both the weapon and the skinned arms, then ends");
        // Non-vacuity of the reach limiter: a path shortened to nothing would satisfy the reach
        // claim above by never moving the hand. The support hand must still travel 3 cm or more
        // along its authored path toward the magazine, ON TOP of following the lowered gun.
        check(reload.hand_travel[0] >= .03f,
              "the support hand still visibly reaches toward the magazine within arm reach");
    }
    // The bolt is measured after the main spring kick has settled (its envelope is under 1%
    // by 0.4 s), so the right hand's reach and pull dominate the motion. Sniper is
    // still selected and idle after the loop above.
    {
        const auto bolt = sweep(block_play::PlayWeaponMotion::kBoltSeconds + .1f, true, .4f);
        report_sweep("bolt", VmWeapon::Sniper, bolt);
        check(bolt.invalid == 0 && bolt.clamped == 0,
              "every bolt frame keeps both hands on reachable, valid targets");
        check(bolt.busy >=
                      static_cast<int>(block_play::PlayWeaponMotion::kBoltSeconds * 60.0f) - 1 &&
                  !demo.viewmodel->busy() && bolt.arm_delta > .03f && bolt.hand_travel[1] >= .03f &&
                  demo.viewmodel->take_case_ejection() && !demo.viewmodel->take_case_ejection(),
              "the swept bolt moves the skinned right arm, ends, and ejects exactly one case");
    }
    // Exercise actions through the real rig and uploaded deformation stream. The pure timeline
    // tests cannot detect a reload that advances correctly but never reaches rendered vertices.
    for (const auto weapon : {block_play::PlayViewmodel::Weapon::Scar,
                              block_play::PlayViewmodel::Weapon::M4,
                              block_play::PlayViewmodel::Weapon::Sniper}) {
        demo.viewmodel->select(block_play::PlayViewmodel::Weapon::M4);
        demo.viewmodel->select(weapon);
        demo.viewmodel->set_aiming(false);
        demo.viewmodel->advance(1.0f, {}, false);
        demo.app.run_frames(48);
        const auto idle_action = capture();
        check(demo.viewmodel->begin_reload(), "idle weapon accepts a reload action");
        demo.viewmodel->advance(.8f, {}, false);
        const auto reload_action = capture();
        const auto& arms = demo.viewmodel->arm_stats();
        // Project the real action wrist and weapon with the viewport camera used by the
        // rendered frame. Image differences alone also pass when the action leaves the screen.
        const auto projected_action = [&]() {
            const auto& vp = demo.visuals->renderer.view_proj_unjittered();
            const auto in_lower_view = [&](core::Vec3 world, float max_x) {
                const auto clip = vp * core::Vec4{world.x, world.y, world.z, 1.0f};
                if (!(clip.w > 0.0f))
                    return false;
                const float x = clip.x / clip.w;
                const float y = clip.y / clip.w;
                std::printf("viewmodel-smoke: %s reload projected x %.3f y %.3f depth %.3f m\n",
                            weapon_name(weapon),
                            x,
                            y,
                            clip.w);
                // Vulkan's projection flips Y: positive NDC Y is below the screen centre.
                return std::isfinite(x) && std::isfinite(y) && std::abs(x) < max_x && y > -.45f &&
                       y < .92f;
            };
            // Reference reloads can put the muzzle outside the frame. The magazine and actual
            // manipulating hand are the action that must remain visible.
            return in_lower_view(demo.viewmodel->magazine_contact_world(), .9f) &&
                   in_lower_view(demo.viewmodel->wrist_world()[0], .9f);
        };
        bool framed_action = projected_action();
        unsigned phase = 0;
        for (const float step : {.45f, .45f}) {
            demo.viewmodel->advance(step, {}, false);
            const auto phase_pixels = capture();
            if (!g_ppm.empty())
                write_ppm((g_ppm + "." + weapon_name(weapon) + "-reload-phase-" +
                           std::to_string(++phase) + ".ppm")
                              .c_str(),
                          phase_pixels,
                          kW,
                          kH);
            framed_action = framed_action && projected_action();
        }
        // Every term of the claim below, so a failure names itself instead of needing a rerun.
        std::printf("viewmodel-smoke: %s rendered reload: reloading %d, valid %d, clamped %u, "
                    "bind skin %.4f m, wrist miss L %.2f R %.2f mm, changed %zu px\n",
                    weapon_name(weapon),
                    demo.viewmodel->reloading() ? 1 : 0,
                    arms.valid_input ? 1 : 0,
                    static_cast<unsigned>(arms.clamped_targets),
                    arms.max_vertex_displacement,
                    arms.wrist_position_error[0] * 1000.0f,
                    arms.wrist_position_error[1] * 1000.0f,
                    static_cast<std::size_t>(changed(idle_action, reload_action)));
        check(demo.viewmodel->reloading() && arms.valid_input && arms.clamped_targets == 0 &&
                  arms.max_vertex_displacement > .001f && changed(idle_action, reload_action) > 100,
              "reload visibly moves the actual weapon and reachable skinned arms");
        check(framed_action,
              "reload magazine and support wrist stay in the lower viewport across the action");
        if (!g_ppm.empty())
            write_ppm((g_ppm + (weapon == VmWeapon::Sniper ? ".sniper-reload.ppm"
                                : weapon == VmWeapon::M4   ? ".m4-reload.ppm"
                                                           : ".scar-reload.ppm"))
                          .c_str(),
                      reload_action,
                      kW,
                      kH);
        demo.viewmodel->advance(3.0f, {}, false);
        check(!demo.viewmodel->busy(), "reload finishes in elapsed time");
    }
    demo.viewmodel->advance(.35f, {}, true);
    const auto bolt_action = capture();
    check(demo.viewmodel->cycling() && demo.viewmodel->arm_stats().valid_input &&
              demo.viewmodel->arm_stats().clamped_targets == 0 &&
              demo.viewmodel->take_case_ejection() && !demo.viewmodel->take_case_ejection(),
          "accepted sniper shot reaches bolt extraction with exactly one case");
    if (!g_ppm.empty())
        write_ppm((g_ppm + ".sniper-bolt.ppm").c_str(), bolt_action, kW, kH);
    check(demo.visuals->renderer.rejected_first_person_deformations() == 0 &&
              demo.viewmodel->resident_meshes() == meshes &&
              demo.viewmodel->resident_materials() == materials &&
              demo.viewmodel->resident_textures() == textures,
          "actions reuse resident resources and valid deformation uploads");
    // Use the SAME live tick seam as --play, through its real transport and GameplayServer.
    // A pure WeaponConfig test cannot catch configuring the spawn template while the live
    // avatar keeps its old automatic component. Disable damage only after the visual proofs.
    for (const auto avatar : demo.session.server.avatars)
        if (auto* weapon = demo.session.server.world.get<gameplay::WeaponConfig>(avatar)) {
            weapon->damage = 0;
            weapon->impulse = 0;
        }
    demo.viewmodel->select(block_play::PlayViewmodel::Weapon::M4);
    demo.viewmodel->select(block_play::PlayViewmodel::Weapon::Sniper);
    const auto live_tick = [&](bool held, bool pressed) {
        gameplay::CharacterInput input{};
        input.pitch = 1.3f; // skyward; this witness must not collapse the already-verified scene
        input.held = held ? gameplay::kActionFire : 0;
        input.pressed = pressed ? gameplay::kActionFire : 0;
        demo.step_play(input);
        demo.viewmodel->advance(1.0f / 60.0f, {}, demo.shot_presented);
        demo.shot_presented = false;
    };
    const auto accepted_before = demo.session.shots_fired;
    for (int i = 0; i < 120; ++i)
        live_tick(true, i == 0);
    check(demo.session.shots_fired == accepted_before + 1,
          "live sniper accepts only one shot while the trigger stays held for two seconds");
    for (unsigned i = 0; i < Demo::kSettleRounds; ++i)
        live_tick(false, false);
    for (unsigned i = 0; i < Demo::kSettleRounds; ++i)
        live_tick(true, i == 0);
    check(demo.session.shots_fired == accepted_before + 2,
          "a fresh sniper press after cooldown accepts the next shot");
    demo.viewmodel->advance(2.0f, {}, false);
    check(demo.viewmodel->begin_reload(), "live sniper starts a reload after bolt completion");
    for (int i = 0; i < 60; ++i)
        live_tick(true, i == 0);
    check(demo.viewmodel->reloading() && demo.session.shots_fired == accepted_before + 2,
          "live reload blocks held and pressed fire through the server transport");
    demo.app.finish_gpu();
    return okay ? 0 : 1;
}

// The playable environment is repository-native content, so unlike the optional weapon art this
// proof runs on every clone. Perturb actual material/sky/reflection inputs after settling TAA;
// counts alone would pass if every new mesh were behind the camera or every texture stayed unbound.
int run_environment_smoke(const std::filesystem::path& cooked, std::string_view scene_path) {
    constexpr std::uint32_t kW = 640, kH = 360;
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kW, kH};
    Demo demo(cfg);
    demo.scripted_charge = false;
    demo.streamed_world = true;
    demo.environment_enabled = true;
    demo.development_hud = false;
    demo.session.server.weapon.automatic = true;
    if (!demo.app.device())
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0;
    if (!demo.start(cooked, scene_path) || !demo.environment)
        return 1;
    for (int i = 0; i < 30; ++i)
        demo.step_sim({});
    play_sunset::apply(demo.session.client.world);
    play_sunset::apply(demo.session.server.world);
    if (auto* c = demo.session.client.world.get<render::Camera>(demo.visuals->camera))
        play_sunset::configure_camera(*c);
    demo.visuals->renderer.set_sky(play_sunset::sky(Visuals::authored_sky()));
    demo.use_authored_camera = true;
    demo.app.on_render([&](app::FrameContext& ctx) { demo.render(ctx); });
    auto pose = [&](core::Vec3 at, float yaw, float pitch) {
        gameplay::FirstPersonView view{};
        view.eye_height = 0;
        view.yaw = yaw;
        view.pitch = pitch;
        demo.visuals->authored_camera = gameplay::eye_transform(view, at);
    };
    auto capture = [&]() {
        demo.app.run_frames(48);
        const auto tex = demo.app.graph()->physical(demo.visuals->last_ldr);
        auto px = read_rgba8(*demo.app.device(), tex, kW, kH);
        demo.app.finish_gpu();
        return px;
    };
    bool okay = true;
    auto check = [&](bool value, const char* label) {
        if (!value) {
            std::fprintf(stderr, "environment-smoke: FAILED %s\n", label);
            okay = false;
        }
    };
    auto changed = [&](const auto& a, const auto& b) {
        std::size_t n = 0;
        if (a.size() != kW * kH * 4 || b.size() != a.size())
            return n;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int delta = 0;
            for (unsigned c = 0; c < 3; ++c)
                delta = std::max(delta, std::abs(static_cast<int>(a[i + c]) - b[i + c]));
            n += delta > 5 ? 1 : 0;
        }
        return n;
    };
    const auto& s = demo.environment->stats();
    check(s.trees == 26 && s.grass_tufts > 3000 && s.grass_tufts <= 3600 && s.puddles == 7 &&
              s.ponds == 1 && s.wall_materials == 8 && s.textures == 24,
          "bounded complete content");
    pose({-6, 1.85f, 0}, play_sunset::kViewYaw, -.05f);
    const auto street = capture();
    const auto control = capture();
    check(street.size() == kW * kH * 4, "street readback");
    if (!g_ppm.empty())
        write_ppm((g_ppm + ".street.ppm").c_str(), street, kW, kH);

    std::vector<std::pair<render::MaterialId, render::PbrMaterialDesc>> walls;
    for (unsigned t = 0; t < blockkit::kTintCount; ++t) {
        for (auto id : {demo.visuals->palette.facade[t], demo.visuals->palette.band[t]}) {
            auto d = demo.visuals->materials.get(id);
            walls.emplace_back(id, d);
            d.base_color_texture = {};
            d.normal_texture = {};
            d.metallic_roughness_texture = {};
            demo.visuals->materials.update(id, d);
        }
    }
    const auto plain = capture();
    const auto texture_effect = changed(street, plain), static_control = changed(street, control);
    check(texture_effect > 1000 && texture_effect > static_control * 3,
          "visible wall textures exceed settled drift");
    for (const auto& [id, d] : walls)
        demo.visuals->materials.update(id, d);

    pose({-3, 2.5f, -2}, play_sunset::kViewYaw, -.32f);
    const auto wet = capture();
    const auto wet_control = capture();
    auto lighting = demo.visuals->renderer.lighting();
    const auto original = lighting;
    lighting.ssr_enabled = false;
    demo.visuals->renderer.set_lighting(lighting);
    const auto no_ssr = capture();
    demo.visuals->renderer.set_lighting(original);
    const auto water_id = demo.environment->water_material();
    const auto water_desc = demo.visuals->materials.get(water_id);
    auto rough_water = water_desc;
    rough_water.roughness = 1;
    rough_water.normal_texture = {};
    rough_water.metallic_roughness_texture = {};
    demo.visuals->materials.update(water_id, rough_water);
    const auto no_gloss = capture();
    demo.visuals->materials.update(water_id, water_desc);
    if (wet.size() != kW * kH * 4 || wet_control.size() != wet.size() ||
        no_ssr.size() != wet.size() || no_gloss.size() != wet.size()) {
        check(false, "water control readbacks");
        return 1;
    }
    // Isolate pixels known to belong to the first puddle. Comparing the whole scene could be
    // satisfied by the existing asphalt reflection even if the new water never drew. The core
    // circle fits INSIDE every rotated outline. Invert each pixel ray onto the known water
    // plane, so each counted sample is a unique pixel, not several projections onto one pixel.
    // Its response must also survive the water-material-only sensitivity check.
    const auto inv_vp = core::inverse(demo.visuals->renderer.view_proj_unjittered());
    const auto eye = demo.visuals->authored_camera.translation;
    std::size_t water_pixels = 0, reflection_pixels = 0, water_drift = 0;
    for (unsigned y = 0; y < kH; ++y) {
        for (unsigned x = 0; x < kW; ++x) {
            const core::Vec4 p =
                inv_vp * core::Vec4{(x + .5f) * 2 / kW - 1, (y + .5f) * 2 / kH - 1, .5f, 1};
            const core::Vec3 ray = core::Vec3{p.x / p.w, p.y / p.w, p.z / p.w} - eye;
            if (ray.y >= -1e-6f)
                continue;
            const auto point = eye + ray * ((.022f - eye.y) / ray.y);
            const float dx = point.x - 1, dz = point.z + 2.6f;
            if (dx * dx + dz * dz > .3f * .3f)
                continue;
            const auto i = (static_cast<std::size_t>(y) * kW + x) * 4;
            int d = 0, c = 0, m = 0;
            for (unsigned k = 0; k < 3; ++k) {
                d = std::max(d, std::abs(static_cast<int>(wet[i + k]) - no_ssr[i + k]));
                c = std::max(c, std::abs(static_cast<int>(wet[i + k]) - wet_control[i + k]));
                m = std::max(m, std::abs(static_cast<int>(wet[i + k]) - no_gloss[i + k]));
            }
            ++water_pixels;
            reflection_pixels += d > 5 && m > 5 ? 1 : 0;
            water_drift += c > 5 ? 1 : 0;
        }
    }
    check(water_pixels > 100 && reflection_pixels > water_pixels / 5 &&
              reflection_pixels > water_drift * 3,
          "puddle reflection responds above unchanged control");
    if (!g_ppm.empty())
        write_ppm((g_ppm + ".puddles.ppm").c_str(), wet, kW, kH);

    pose({15, 2.8f, 26}, play_sunset::kViewYaw, -.14f);
    const auto park = capture();
    if (!g_ppm.empty())
        write_ppm((g_ppm + ".park.ppm").c_str(), park, kW, kH);
    const auto sky = demo.visuals->renderer.sky();
    auto clear = sky;
    clear.clouds_enabled = false;
    demo.visuals->renderer.set_sky(clear);
    const auto cloudless = capture();
    const auto cloud_effect = changed(park, cloudless);
    check(cloud_effect > 1000, "visible cloud layer responds to its input");
    check(demo.play_world->stats().refusals == 0 && !demo.session.world_stopped,
          "textured terrain remains resident and valid");
    std::printf("environment-smoke: textures %zu px / static %zu; water reflections %zu/%zu / "
                "static %zu; clouds %zu; trees %u grass %u puddles %u ponds %u\n",
                texture_effect,
                static_control,
                reflection_pixels,
                water_pixels,
                water_drift,
                cloud_effect,
                s.trees,
                s.grass_tufts,
                s.puddles,
                s.ponds);

    // Prove the same accepted-shot path used by live play without optional weapon art. A clean
    // clone must still shoot, and cooldown refusal must not spawn another visual event.
    demo.use_authored_camera = false;
    demo.visuals->renderer.set_sky(play_sunset::sky(Visuals::authored_sky()));
    (void)capture(); // settle the live camera before the input tape
    const auto entities_before = demo.session.client.world.entity_count();
    const auto shots_before = demo.session.shots_fired;
    gameplay::CharacterInput trigger{};
    trigger.yaw = demo.session.client.view.yaw;
    trigger.pitch = demo.session.client.view.pitch;
    trigger.pressed = trigger.held = gameplay::kActionFire;
    // The in-process demo still sends over its real transport: server PreSim drains before
    // client PreSim sends. Pump the bounded wire latency, then inspect ACCEPTED events. Two
    // consecutive pressed commands exercise the actual server's cooldown, not a raw local edge.
    demo.step_sim(gameplay_net::to_input_command(trigger));
    demo.step_sim(gameplay_net::to_input_command(trigger));
    trigger.pressed = trigger.held = 0;
    for (int tick = 0; tick < 8; ++tick)
        demo.step_sim(gameplay_net::to_input_command(trigger));
    check(demo.session.shots_fired == shots_before + 1 && demo.shot_presented,
          "live transport accepts one shot and refuses the consecutive cooldown input");
    demo.app.run_frames(1);
    const auto fired =
        read_rgba8(*demo.app.device(), demo.app.graph()->physical(demo.visuals->last_ldr), kW, kH);
    demo.app.finish_gpu();
    check(demo.session.client.world.entity_count() == entities_before,
          "firing reuses its resident entity pool");
    const auto expired = capture();
    const auto unchanged = capture();
    const auto firing_pixels = changed(fired, expired);
    const auto firing_drift = changed(expired, unchanged);
    check(firing_pixels > 50 && firing_pixels > firing_drift * 3,
          "accepted muzzle/tracer feedback is visible and expires above static drift");
    std::printf(
        "environment-smoke: accepted firing feedback %zu pixels / drift %zu; fixed resident pool\n",
        firing_pixels,
        firing_drift);
    if (!g_ppm.empty())
        write_ppm((g_ppm + ".firing.ppm").c_str(), fired, kW, kH);
    const auto automatic_before = demo.session.shots_fired;
    trigger.held = gameplay::kActionFire;
    for (int tick = 0; tick < 18; ++tick) {
        trigger.pressed = tick == 0 ? gameplay::kActionFire : 0;
        demo.step_sim(gameplay_net::to_input_command(trigger));
    }
    trigger.pressed = trigger.held = 0;
    for (int tick = 0; tick < 8; ++tick)
        demo.step_sim(gameplay_net::to_input_command(trigger));
    check(demo.session.shots_fired == automatic_before + 3,
          "held trigger fires three accepted rounds in 18 ticks, then release stops fire");

    // ── PlayWeaponFx invariants, on a throwaway ECS world + physics world ──────────────────────
    // The pixel checks above prove something drew; they cannot tell a camera-following flash from
    // an orphaned one, or prove a case actually falls. This block drives a second, isolated
    // PlayWeaponFx instance (so it cannot perturb the live demo world/entity-pool checks already
    // run above) against real WorldTransform/MeshRef/PointLight state and a real raycast, reusing
    // the live app's device and the demo's visuals registries as the spec asks rather than opening
    // a second GPU mesh registry for two low-poly shapes nobody else needs.
    {
        ecs::World fx_world;
        ecs::register_transform_components(fx_world);
        render::register_render_components(fx_world);

        // A minimal physics world: one static floor for the casing raycast and one static target
        // for the impact decal's body-relative anchor. No network, no bodies the real scene owns.
        physics::PhysicsWorld fx_physics;
        physics::BodyDesc floor_desc{};
        floor_desc.motion = physics::MotionType::Static;
        floor_desc.shape.type = physics::ShapeType::Box;
        floor_desc.shape.half_extents = {50.0f, 0.5f, 50.0f};
        floor_desc.position = {0.0f, -0.5f, 0.0f};
        const auto floor_body = fx_physics.create_body(floor_desc);
        physics::BodyDesc target_desc{};
        target_desc.motion = physics::MotionType::Static;
        target_desc.shape.type = physics::ShapeType::Box;
        target_desc.shape.half_extents = {1.0f, 1.0f, 1.0f};
        target_desc.position = {4.0f, 1.0f, 0.0f};
        const auto target_body = fx_physics.create_body(target_desc);
        check(floor_body.is_valid() && target_body.is_valid(),
              "weapon fx probe: isolated bodies created");
        physics::BodyDesc rubble{};
        rubble.shape.type = physics::ShapeType::Box;
        rubble.shape.half_extents = {.2f, .2f, .2f};
        rubble.mass = 5;
        rubble.position = {10, 2, 0};
        const auto rubble_body = fx_physics.create_body(rubble);
        push_loose_hit(fx_physics, rubble_body, {10, 2.1f, 0}, {250, 0, 0});
        physics::BodyState pushed{};
        check(fx_physics.get_body_state(rubble_body, pushed) &&
                  std::abs(pushed.linear_velocity.x - 5) < .001f &&
                  core::length(pushed.angular_velocity) > .01f,
              "bullet impulse wakes dynamic rubble with bounded momentum and contact torque");
        physics::BodyState static_before{}, static_after{};
        const bool static_read = fx_physics.get_body_state(target_body, static_before);
        push_loose_hit(fx_physics, target_body, {4, 1, 0}, {250, 0, 0});
        check(static_read && fx_physics.get_body_state(target_body, static_after) &&
                  core::length(static_after.linear_velocity - static_before.linear_velocity) == 0,
              "bullet impulse leaves static scenery immovable");

        // Materials are the stable key into a known entity's role: fire()/pose() assign
        // material_ids_[0..4] to flash/tracer/spark/impact/case respectively (play_weapon_fx.cpp),
        // and MaterialRegistry::add is a plain push_back, so the five ids this fresh instance mints
        // are exactly the next five indices after whatever the live demo already registered. That
        // is read off material.hpp's `add`, not guessed, and it is the "helper via ECS snapshot"
        // the spec allows in place of exposing new API purely for this test.
        const auto first_fx_material =
            static_cast<render::MaterialId>(demo.visuals->materials.size());
        block_play::PlayWeaponFx fx(
            *demo.app.device(), demo.visuals->meshes, demo.visuals->materials);
        check(fx.initialize(), "weapon fx probe: isolated instance initializes");
        const render::MaterialId flash_mat = first_fx_material + 0;
        const render::MaterialId tracer_mat = first_fx_material + 1;
        const render::MaterialId spark_mat = first_fx_material + 2;
        const render::MaterialId impact_mat = first_fx_material + 3;
        const render::MaterialId case_mat = first_fx_material + 4;

        // Find the one active entity wearing a given material, if any -- a query over the fixed
        // pool rather than reaching into PlayWeaponFx's private Shot/Impact/Case arrays, which the
        // spec asks to avoid touching.
        auto find_by_material = [&](render::MaterialId wanted) {
            ecs::Entity found{};
            fx_world.query<ecs::WorldTransform, render::MeshRef, render::MaterialRef>().for_each(
                [&](ecs::Entity e,
                    ecs::WorldTransform&,
                    render::MeshRef& mesh,
                    render::MaterialRef& mat) {
                    if (mat.material == wanted && mesh.mesh != render::kInvalidMeshId)
                        found = e;
                });
            return found;
        };
        auto transform_of = [&](ecs::Entity e) {
            auto* t = fx_world.get<ecs::WorldTransform>(e);
            return t ? t->value.translation : core::Vec3{std::nanf(""), 0, 0};
        };
        auto finite3 = [](core::Vec3 v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const auto inverse_safe_effects = [&]() {
            bool safe = true;
            fx_world.query<ecs::WorldTransform, render::MeshRef>().for_each(
                [&](ecs::Entity, ecs::WorldTransform& transform, render::MeshRef& mesh) {
                    if (mesh.mesh == render::kInvalidMeshId)
                        return;
                    // The renderer computes inverse-transpose normal matrices. Keep this
                    // checked-build contract visible in Release too: millimetre geometry is
                    // valid, but a tiny instance determinant trips the inverse's guard.
                    const float det = core::determinant(core::to_matrix(transform.value));
                    safe &= std::isfinite(det) && std::abs(det) > core::kEpsilon;
                });
            return safe;
        };

        block_play::PlayWeaponFx::ViewPose view_a{};
        view_a.eye = {0, 1.2f, -4};
        view_a.direction = {1, 0, 0.1f};
        view_a.muzzle = {0.2f, 1.1f, -3.9f};
        view_a.ejection_point = {-0.05f, 1.05f, -3.95f};
        view_a.velocity = {2, 0, 0}; // the player's own motion, carried into the case's launch

        // One accepted shot that both hits a surface (spark + impact) and names the target body,
        // posed once under view_a.
        fx.fire(view_a.eye,
                view_a.direction,
                view_a.muzzle,
                {4, 1.0f, 0},
                true,
                {-1, 0, 0},
                target_body,
                &fx_physics);
        fx.pose(fx_world, &fx_physics, &view_a);
        check(inverse_safe_effects(), "weapon fx probe: shot transforms satisfy checked inverse");
        const auto flash_a = find_by_material(flash_mat);
        const auto flash_pos_a = transform_of(flash_a);
        const auto tracer_pos_a = transform_of(find_by_material(tracer_mat));
        const auto impact_pos_a = transform_of(find_by_material(impact_mat));
        const auto scar_entity = find_by_material(impact_mat);
        const auto* scar_mesh_ref = fx_world.get<render::MeshRef>(scar_entity);
        const auto* scar_pose = fx_world.get<ecs::WorldTransform>(scar_entity);
        check(scar_mesh_ref && scar_pose &&
                  demo.visuals->meshes.get(scar_mesh_ref->mesh).local_min.z ==
                      demo.visuals->meshes.get(scar_mesh_ref->mesh).local_max.z &&
                  core::dot(core::rotate(scar_pose->value.rotation, core::Vec3{0, 0, 1}),
                            core::Vec3{-1, 0, 0}) > .999f,
              "scar geometry is flat and faces the hit normal");
        check(demo.visuals->materials.get(tracer_mat).emissive[0] > 1,
              "accepted tracer uses emissive radiance");
        check(flash_a != ecs::Entity{} && finite3(transform_of(flash_a)),
              "weapon fx probe: flash entity is live under the first view pose");
        check(fx_world.entity_count() == block_play::PlayWeaponFx::kEntityCount,
              "weapon fx probe: the first pose binds exactly the fixed resident pool");

        ecs::Entity muzzle_light{};
        fx_world.query<ecs::WorldTransform, render::PointLight>().for_each(
            [&](ecs::Entity e, ecs::WorldTransform& at, render::PointLight& light) {
                if (light.intensity > 0 && core::length(at.value.translation - flash_pos_a) < 1e-4f)
                    muzzle_light = e;
            });
        check(muzzle_light.is_valid(), "weapon fx probe: a live light accompanies the flash");
        // Change only the view at a fixed shot age. Advancing time here would also move the
        // tracer along its world path, confounding camera motion with projectile presentation.
        block_play::PlayWeaponFx::ViewPose view_b = view_a;
        view_b.eye.x += 0.3f;
        view_b.muzzle = {0.5f, 1.15f, -3.85f};
        view_b.direction = {1, 0, 0.2f};
        fx.pose(fx_world, &fx_physics, &view_b);
        const auto flash_pos_b = transform_of(find_by_material(flash_mat));
        const auto tracer_pos_b = transform_of(find_by_material(tracer_mat));
        const auto impact_pos_b = transform_of(find_by_material(impact_mat));
        const float flash_moved = core::length(flash_pos_b - flash_pos_a);
        check(flash_moved > 0.1f,
              "weapon fx probe: flash follows the moved view pose within one 16 ms frame");
        check(core::length(tracer_pos_b - tracer_pos_a) < 1e-4f,
              "weapon fx probe: accepted tracer world path is unaffected by the view move");
        check(core::length(impact_pos_b - impact_pos_a) < 1e-4f,
              "weapon fx probe: accepted impact world position is unaffected by the view move");
        check(core::length(transform_of(muzzle_light) - flash_pos_b) < 1e-4f,
              "weapon fx probe: the live light follows the current flash pose");
        fx.advance(.005f);
        fx.pose(fx_world, &fx_physics, &view_b);
        const auto travelled = transform_of(find_by_material(tracer_mat)) - tracer_pos_b;
        const auto accepted_path = core::normalize(core::Vec3{4, 1, 0} - view_a.muzzle);
        check(core::dot(travelled, accepted_path) > 1.0f &&
                  core::length(core::cross(travelled, accepted_path)) < 1e-4f,
              "weapon fx probe: elapsed time advances the tracer along its accepted world path");

        // Case ejection: one queued case, posed under a view with real velocity/ejection data. Its
        // first pose must land exactly at the ejection point with a velocity that carries the
        // player's own motion (view->velocity), not just the weapon's local kick. Deliberately the
        // FIRST case this isolated instance ever ejects: every case entity shares `case_mat` (all
        // 32 ring slots do), so `find_by_material` can only name ONE of them unambiguously while
        // exactly one is active -- this must run before the overflow stress below puts several
        // more into flight.
        fx.queue_case();
        fx.pose(fx_world, &fx_physics, &view_a);
        check(inverse_safe_effects(), "weapon fx probe: case transforms satisfy checked inverse");
        const auto case_entity = find_by_material(case_mat);
        check(case_entity != ecs::Entity{} &&
                  core::length(transform_of(case_entity) - view_a.ejection_point) < 1e-4f,
              "weapon fx probe: a queued case is ejected at the view's ejection point");
        const float start_height = transform_of(case_entity).y;
        const float floor_top = floor_desc.position.y + floor_desc.shape.half_extents.y;
        float min_height = start_height;
        // Fall under gravity, bounce once off the floor, then settle -- never sink through it. The
        // view's +2.2 m/s vertical kick (play_weapon_fx.cpp's `pose`) makes it rise ~0.25 m before
        // falling, so a worked trajectory (v0=2.2, g=9.81, drop to y=0 from y=1.05) puts first
        // contact at ~0.74 s and the settle a few tenths later; 100 ticks at 16 ms (1.6 s) covers
        // both bounces with margin and stays well under kCaseLifetime's 3 s, so age never expires
        // the mesh before it settles.
        bool saw_descent = false, saw_rise_after_floor = false;
        for (int tick = 0; tick < 100; ++tick) {
            fx.advance(0.016f);
            fx.pose(fx_world, &fx_physics, &view_a);
            const float y = transform_of(case_entity).y;
            saw_descent |= y < start_height - 0.02f;
            min_height = std::min(min_height, y);
            if (y > min_height + 0.002f && min_height < start_height - 0.02f)
                saw_rise_after_floor = true; // the one visible bounce
        }
        check(saw_descent, "weapon fx probe: the ejected case falls under gravity");
        check(min_height > floor_top - 0.01f,
              "weapon fx probe: the case never sinks through the floor");
        check(saw_rise_after_floor,
              "weapon fx probe: the case bounces once off the floor before settling");
        const auto settled_height = transform_of(case_entity).y;
        fx.advance(0.1f);
        fx.pose(fx_world, &fx_physics, &view_a);
        check(
            std::fabs(transform_of(case_entity).y - settled_height) < 1e-4f,
            "weapon fx probe: a settled case stays at rest rather than sinking through the floor");

        // Pool growth: fire far past the ring size, and queue far past the case ring size WITHOUT a
        // valid-view pose to drain it (the 2-arg `pose()` overload -- no view at all, the
        // documented fallback for "old callers"). `queue_case()` clamps at kCaseSlots rather than
        // growing, and neither overflow may add a single entity/mesh/material to the fixed pool.
        for (std::size_t i = 0; i < block_play::PlayWeaponFx::kSlots * 3; ++i)
            fx.fire(view_a.eye, view_a.direction, view_a.muzzle, {4, 1, 0}, false);
        fx.pose(fx_world, &fx_physics, &view_a);
        for (std::size_t i = 0; i < block_play::PlayWeaponFx::kCaseSlots * 2; ++i)
            fx.queue_case();
        fx.pose(fx_world, &fx_physics); // no view: queued cases stay queued, not ejected
        check(fx_world.entity_count() == block_play::PlayWeaponFx::kEntityCount,
              "weapon fx probe: burst overflow and a flooded, undrained case queue grow no pool");

        // Expiry: run the clock past every shot lifetime and re-pose; the flash/tracer/spark
        // entities must drop back to kInvalidMeshId rather than keep showing a stale pose. (This
        // valid-view pose also drains the queue above -- harmless, nothing after this reads a case
        // by material again.)
        fx.advance(block_play::PlayWeaponFx::kSparkLifetime + 0.01f);
        fx.pose(fx_world, &fx_physics, &view_a);
        check(find_by_material(flash_mat) == ecs::Entity{} &&
                  find_by_material(tracer_mat) == ecs::Entity{} &&
                  find_by_material(spark_mat) == ecs::Entity{},
              "weapon fx probe: expired shot parts hide their mesh");

        // Invalid dt/view handling: neither may propagate a NaN into any posed transform. advance()
        // guards dt with isfinite()+>0 (negative/zero/NaN are all no-ops); a NaN view fails
        // pose()'s own `valid_view` check and falls back to each shot's recorded muzzle/direction
        // instead.
        fx.advance(std::nanf(""));
        fx.advance(-1.0f);
        fx.advance(0.0f);
        block_play::PlayWeaponFx::ViewPose bad_view = view_a;
        bad_view.eye.x = std::nanf("");
        fx.pose(fx_world, &fx_physics, &bad_view);
        bool any_nan = false;
        fx_world.query<ecs::WorldTransform>().for_each(
            [&](ecs::WorldTransform& t) { any_nan |= !finite3(t.value.translation); });
        check(!any_nan, "weapon fx probe: a NaN view/dt never reaches a posed WorldTransform");

        // Surface responses share the bounded path but must differ in both dust and light.
        // A quiet interval isolates each material; metal/water must never emit masonry dust.
        using Surface = block_play::PlayWeaponFx::Surface;
        const auto resource_count = demo.visuals->materials.size();
        for (const auto surface : {Surface::Concrete,
                                   Surface::Brick,
                                   Surface::Plaster,
                                   Surface::Soil,
                                   Surface::Metal,
                                   Surface::Water}) {
            fx.advance(block_play::PlayWeaponFx::kDustLifetime + .1f);
            fx.pose(fx_world, &fx_physics, &view_a);
            fx.fire(view_a.eye,
                    view_a.direction,
                    view_a.muzzle,
                    {4, 1, 0},
                    true,
                    {-1, 0, 0},
                    target_body,
                    &fx_physics,
                    surface,
                    false);
            fx.pose(fx_world, &fx_physics, &view_a);
            std::vector<render::GpuParticle> dust;
            fx.append_smoke(dust);
            const bool dusty = surface != Surface::Metal && surface != Surface::Water;
            check(dusty ? !dust.empty() : dust.empty(),
                  "masonry and soil produce dust; metal sparks and water splash do not");
            if (surface == Surface::Brick || surface == Surface::Soil)
                check(!dust.empty() && dust.front().color_alpha[0] > dust.front().color_alpha[2],
                      "brick and soil dust carry warm material color");
            unsigned lights = 0;
            fx_world.query<render::PointLight>().for_each(
                [&](const render::PointLight& light) { lights += light.intensity > 0 ? 1u : 0u; });
            check(lights == (surface == Surface::Metal ? 2u : 1u),
                  "only metal impacts add spark lighting beyond the muzzle light");
            check(inverse_safe_effects(), "every surface effect has a valid normal matrix");
        }
        for (unsigned i = 0; i < 100; ++i)
            fx.fire(view_a.eye,
                    view_a.direction,
                    view_a.muzzle,
                    {4, 1, 0},
                    true,
                    {-1, 0, 0},
                    target_body,
                    &fx_physics,
                    Surface::Brick,
                    false);
        fx.pose(fx_world, &fx_physics, &view_a);
        std::vector<render::GpuParticle> dust;
        fx.append_smoke(dust);
        check(dust.size() <= block_play::PlayWeaponFx::kDustSlots &&
                  fx_world.entity_count() == block_play::PlayWeaponFx::kEntityCount &&
                  demo.visuals->materials.size() == resource_count,
              "sustained impacts keep particle, entity and material pools bounded");
        fx.advance(1.0f);
        dust.clear();
        fx.append_smoke(dust);
        check(!dust.empty(), "impact dust still drifts one second after the hit");
        fx.advance(block_play::PlayWeaponFx::kDustLifetime);
        dust.clear();
        fx.append_smoke(dust);
        check(dust.empty(), "impact dust fully expires");

        std::printf("environment-smoke: weapon fx probe flash-move %.3f m / case fall %.3f m, "
                    "min height %.3f\n",
                    static_cast<double>(flash_moved),
                    static_cast<double>(start_height - min_height),
                    static_cast<double>(min_height));
    }
    return okay ? 0 : 1;
}

// ── --play: walk around it ──────────────────────────────────────────────────────────────────────
// A separate opt-in proof freezes simulation while toggling its presentation. It exercises the
// same consumer/render path as play; the existing environment proof keeps its original inputs.
int run_effects_smoke(const std::filesystem::path& cooked, std::string_view scene_path) {
    constexpr std::uint32_t width = 640, height = 360;
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {width, height};
    g_effects_preview = true;
    Demo demo(cfg);
    demo.scripted_charge = false;
    demo.streamed_world = true;
    demo.environment_enabled = true;
    demo.development_hud = false;
    if (!demo.app.device())
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0;
    if (!demo.start(cooked, scene_path) || !demo.effects)
        return 1;
    play_sunset::apply(demo.session.client.world);
    play_sunset::apply(demo.session.server.world);
    demo.use_authored_camera = true;
    gameplay::FirstPersonView view{};
    view.eye_height = 0;
    view.yaw = play_sunset::kViewYaw;
    view.pitch = -.03f;
    demo.visuals->authored_camera = gameplay::eye_transform(view, {-6, 1.85f, 0});
    demo.visuals->renderer.set_taa_resolve_enabled(false);
    std::vector<double> smoke_gpu_ms;
    demo.app.on_frame_timings(
        [&](std::uint64_t, std::span<const render::RenderGraph::PassTiming> timings) {
            for (const auto& timing : timings)
                if (timing.name == "fx-smoke")
                    smoke_gpu_ms.push_back(timing.gpu_ms);
        });
    demo.app.on_render([&](app::FrameContext& ctx) { demo.render(ctx); });
    for (int i = 0; i < 180; ++i)
        demo.step_sim({});
    const auto capture = [&]() {
        demo.app.run_frames(24);
        auto pixels = read_rgba8(
            *demo.app.device(), demo.app.graph()->physical(demo.visuals->last_ldr), width, height);
        demo.app.finish_gpu();
        return pixels;
    };
    const auto server_hash = demo.session.server.physics.world_hash();
    const auto client_hash = demo.session.client.physics.world_hash();
    demo.effects_visible = false;
    const auto off = capture();
    const auto control = capture();
    demo.effects_visible = true;
    const auto on = capture();
    std::sort(smoke_gpu_ms.begin(), smoke_gpu_ms.end());
    if (!smoke_gpu_ms.empty())
        std::printf("effects-smoke: diagnostic GPU fx-smoke %zu frames p50 %.6f p99 %.6f ms\n",
                    smoke_gpu_ms.size(),
                    smoke_gpu_ms[smoke_gpu_ms.size() / 2],
                    smoke_gpu_ms.back());
    const auto difference = [&](const auto& a, const auto& b) {
        std::size_t n = 0;
        if (a.size() != width * height * 4 || b.size() != a.size())
            return n;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int d = 0;
            for (unsigned c = 0; c < 3; ++c)
                d = std::max(d, std::abs(static_cast<int>(a[i + c]) - b[i + c]));
            n += d > 5 ? 1u : 0u;
        }
        return n;
    };
    const auto changed = difference(on, control), drift = difference(off, control);
    const auto& particles = demo.effects->field();
    const bool okay = changed > drift + 100 && particles.count() > 0 &&
                      particles.count() <= block_play::PlayEffects::kParticleCapacity &&
                      demo.visuals->renderer.smoke_particles_dropped() == 0 &&
                      demo.session.server.physics.world_hash() == server_hash &&
                      demo.session.client.physics.world_hash() == client_hash;
    std::printf("effects-smoke: changed=%zu drift=%zu live=%zu requested=%llu spawned=%llu "
                "capacity-drops=%llu render-drops=%llu state-unchanged=%d result=%s\n",
                changed,
                drift,
                particles.count(),
                static_cast<unsigned long long>(particles.stats().requested_particles),
                static_cast<unsigned long long>(particles.stats().spawned_particles),
                static_cast<unsigned long long>(particles.stats().dropped_capacity),
                static_cast<unsigned long long>(demo.visuals->renderer.smoke_particles_dropped()),
                demo.session.server.physics.world_hash() == server_hash &&
                    demo.session.client.physics.world_hash() == client_hash,
                okay ? "PASS" : "FAIL");
    if (!g_ppm.empty()) {
        write_ppm((g_ppm + ".off.ppm").c_str(), control, width, height);
        write_ppm((g_ppm + ".on.ppm").c_str(), on, width, height);
    }
    return okay ? 0 : 1;
}

// ── Shimmer trace (diagnosis, env-gated: RIME_SHIMMER_TRACE=<csv path>) ─────────────────────────
// The owner reported tree shadows on walls and ground that "shimmer" while the camera only turns
// or strafes. Shadow-map probes at fixed world points were stable, so the change must enter later,
// on the SCREEN. This measures it there: a scripted, frame-indexed camera path (so every
// configuration is rendered from IDENTICAL poses -- a mouse-driven sweep never repeats), after each
// frame the LDR output is read back and fixed WORLD points are reprojected with the frame's own
// unjittered view-projection to pixels; the 15x15 mean luma around each is one CSV column.
// A point whose world position never changes but whose pixel value does is exactly "shimmer".
// RIME_SHIMMER_OFF is a comma list that disables candidate stages one at a time:
//   taa, jitter, ssr, ddgi, shadows (control: removes the signal), gifade, clouds.
void run_shimmer_trace(Demo& demo, const char* csv_path) {
    constexpr std::uint32_t kW = kWidth, kH = kHeight;
    const std::string off = std::getenv("RIME_SHIMMER_OFF") ? std::getenv("RIME_SHIMMER_OFF") : "";
    const auto has = [&](const char* t) {
        return ("," + off + ",").find(std::string(",") + t + ",") != std::string::npos;
    };
    auto& r = demo.visuals->renderer;
    auto lighting = r.lighting();
    if (has("ssr"))
        lighting.ssr_enabled = false;
    if (has("ddgi"))
        lighting.ddgi_enabled = false;
    if (has("shadows"))
        lighting.shadows_enabled = false;
    if (has("gifade"))
        lighting.ddgi_volume_fade = false;
    // Diagnostic knobs for the DDGI field:
    // RIME_SHIMMER_DDGI="hyst=0.97,spacing=2,count=16,rays=64,dist=20".
    if (const char* knobs = std::getenv("RIME_SHIMMER_DDGI")) {
        const std::string k = knobs;
        const auto get = [&](const char* key, float fallback) {
            const auto at = ("," + k).find(std::string(",") + key + "=");
            return at == std::string::npos
                       ? fallback
                       : static_cast<float>(std::atof(k.c_str() + at + std::strlen(key) + 1));
        };
        lighting.ddgi_hysteresis = get("hyst", lighting.ddgi_hysteresis);
        lighting.ddgi_probe_spacing = get("spacing", lighting.ddgi_probe_spacing);
        lighting.ddgi_max_trace_distance = get("dist", lighting.ddgi_max_trace_distance);
        lighting.ddgi_rays_per_probe =
            static_cast<std::uint32_t>(get("rays", float(lighting.ddgi_rays_per_probe)));
        const auto c = static_cast<std::uint32_t>(get("count", 0));
        if (c != 0)
            lighting.ddgi_probe_count_x = lighting.ddgi_probe_count_y =
                lighting.ddgi_probe_count_z = c;
    }
    r.set_lighting(lighting);
    if (has("jitter"))
        r.set_temporal_jitter_enabled(false);
    if (has("taa"))
        r.set_taa_resolve_enabled(false);
    if (has("clouds")) {
        auto sky = r.sky();
        sky.clouds_enabled = false;
        r.set_sky(sky);
    }
    // Sensitivity control for the shadow probe (RIME_SHIMMER_SUN_NUDGE_DEG): turn the one sun by a
    // known small angle about world Y, so a probe that claims "stable to 5e-5" can be shown to
    // MOVE when the world really does move by this much.
    if (const char* nudge = std::getenv("RIME_SHIMMER_SUN_NUDGE_DEG")) {
        const core::Quat dq = core::quat_from_axis_angle(
            {0, 1, 0}, static_cast<float>(std::atof(nudge)) * 0.0174532925f);
        for (ecs::World* w : {&demo.session.server.world, &demo.session.client.world})
            w->query<render::DirectionalLight>().for_each(
                [&](ecs::Entity e, render::DirectionalLight&) {
                    if (auto* lt = w->get<ecs::LocalTransform>(e))
                        lt->value.rotation = dq * lt->value.rotation;
                    if (auto* wt = w->get<ecs::WorldTransform>(e))
                        wt->value.rotation = dq * wt->value.rotation;
                });
    }
    std::vector<core::Vec3> pts;
    for (float y : {2.0f, 3.8f, 5.2f})
        for (float z : {-6.0f, -8.0f, -10.0f})
            pts.push_back({11.84f, y, z});
    for (float x : {6.0f, 7.5f, 8.5f, 9.5f, 10.5f, 11.2f})
        for (float z : {-4.5f, -6.0f, -8.0f, -10.0f, -12.0f, -15.0f})
            pts.push_back({x, 0.012f, z});
    const char* env_pitch = std::getenv("RIME_SHIMMER_PITCH");
    const float pitch = env_pitch ? static_cast<float>(std::atof(env_pitch)) : 0.4475f;
    const bool flip = std::getenv("RIME_SHIMMER_FLIPY") != nullptr;
    constexpr float kBaseYaw = -0.6435f;
    constexpr int kWarm = 200, kStatic = 120, kYaw = 240, kStrafe = 120;
    std::FILE* csv = std::fopen(csv_path, "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "frame,phase,yaw,x");
    for (std::size_t i = 0; i < pts.size(); ++i)
        std::fprintf(csv, ",p%zu_luma,p%zu_u,p%zu_v", i, i, i);
    std::fprintf(csv, "\n");
    const auto project = [&](const core::Mat4& vp, core::Vec3 p, double& u, double& v) {
        const core::Vec4 c = vp * core::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.05f)
            return false;
        u = (c.x / c.w * .5 + .5) * kW;
        v = (c.y / c.w * .5 + .5) * kH;
        if (flip)
            v = kH - v;
        return u >= 8 && v >= 8 && u < kW - 8 && v < kH - 8;
    };
    const int total = kWarm + kStatic + kYaw + kStrafe;
    for (int f = 0; f < total; ++f) {
        const int t = f - kWarm;
        float yaw = kBaseYaw,
              x = std::getenv("RIME_SHIMMER_X0")
                      ? static_cast<float>(std::atof(std::getenv("RIME_SHIMMER_X0")))
                      : 9.0f;
        int phase = 0;
        if (t >= kStatic && t < kStatic + kYaw) {
            phase = 1;
            yaw += 0.2f * std::sin(6.2831853f * static_cast<float>(t - kStatic) / 120.0f);
        } else if (t >= kStatic + kYaw) {
            phase = 2;
            x += 0.75f *
                 (1.0f - std::cos(6.2831853f * static_cast<float>(t - kStatic - kYaw) / 120.0f));
        }
        gameplay::FirstPersonView v{};
        v.yaw = yaw;
        v.pitch = pitch;
        v.eye_height = 0.0f;
        demo.visuals->authored_camera = gameplay::eye_transform(v, {x, 1.6f, -5.0f});
        demo.use_authored_camera = true;
        demo.app.run_frames(1);
        if (t < 0)
            continue;
        const rhi::TextureHandle tex = demo.app.graph()->physical(demo.visuals->last_ldr);
        const auto px = read_rgba8(*demo.app.device(), tex, kW, kH);
        const core::Mat4 vp = r.view_proj_unjittered();
        std::fprintf(csv, "%d,%d,%.5f,%.4f", t, phase, double(yaw), double(x));
        auto marked = px;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            double u = -1, vv = -1, luma = -1;
            if (project(vp, pts[i], u, vv)) {
                const int cx = static_cast<int>(u), cy = static_cast<int>(vv);
                double sum = 0;
                // 15x15 window: a 3x3 mean follows individual brick/mortar texels as the point
                // slides by a sub-pixel, which is texture aliasing, not lighting shimmer.
                for (int dy = -7; dy <= 7; ++dy)
                    for (int dx = -7; dx <= 7; ++dx) {
                        const std::uint8_t* q =
                            &px[(static_cast<std::size_t>(cy + dy) * kW + (cx + dx)) * 4];
                        sum += 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
                    }
                luma = sum / 225.0;
                for (int d = -6; d <= 6; ++d) {
                    auto* a = &marked[(static_cast<std::size_t>(cy) * kW +
                                       std::clamp(cx + d, 0, int(kW) - 1)) *
                                      4];
                    auto* b =
                        &marked[(static_cast<std::size_t>(std::clamp(cy + d, 0, int(kH) - 1)) * kW +
                                 cx) *
                                4];
                    a[0] = b[0] = 255;
                    a[1] = a[2] = b[1] = b[2] = 0;
                }
            } else {
                u = vv = -1;
            }
            std::fprintf(csv, ",%.3f,%.1f,%.1f", luma, u, vv);
        }
        std::fprintf(csv, "\n");
        if (t == 0 || t == kStatic + 60)
            write_ppm(
                (std::string(csv_path) + (t == 0 ? ".f0.ppm" : ".f1.ppm")).c_str(), marked, kW, kH);
    }
    std::fclose(csv);
    demo.app.finish_gpu();
}

int run_play(const std::filesystem::path& cooked, std::string_view scene_path, unsigned frames) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    cfg.windowed = true;
    cfg.window_title = "Rime — the block";
    cfg.window_size = {kWidth, kHeight};
    // On the measured Wayland/NVIDIA path FIFO acquisition periodically waited an extra
    // refresh despite ~5 ms CPU work. Prefer the RHI's tear-free latest-frame presentation;
    // unsupported surfaces retain FIFO. This changes presentation, not simulation cadence.
    cfg.vsync = false;

    Demo demo(cfg, g_network_impairment);
    std::printf("play network: impairment %s (one-way %u ms, loss %.0f%%)\n",
                g_network_impairment ? "ON" : "OFF",
                g_network_impairment ? kOneWayMs : 0,
                g_network_impairment ? double(kLossRate * 100) : 0.0);
    demo.scripted_charge = false; // interactive: wait for F, not the tape
    demo.session.server.weapon.automatic = true;
    demo.streamed_world = true;
    demo.building_support_enabled = true;
    demo.environment_enabled = true;
    demo.viewmodel_enabled = true;
    demo.player_help = true;
    if (demo.app.device() == nullptr) {
        std::fprintf(stderr, "99-the-block: --play needs a Vulkan device\n");
        return 1;
    }
    if (!demo.start(cooked, scene_path)) {
        return 1;
    }

    // PLAY-ONLY evening preset (play_sunset.hpp): turn the one authored sun ahead of the player on
    // both peers, and face the live view down the street at it. Done before the live intent is
    // built below, so the first frame already looks at the sun.
    if (scene_path.empty()) {
        play_sunset::apply(demo.session.server.world);
        play_sunset::apply(demo.session.client.world);
    } // Saved maps own their lighting: Run must not silently replace an editor's sun edits.
    // The initial look: the authored spawn's heading when the map has one, else the sun-facing
    // preset. Decided once, here, and read back below so the handshake input cannot disagree.
    if (demo.session.client.spawn.status == block_play::SpawnStatus::Valid) {
        demo.session.client.view.yaw = demo.session.client.spawn.placement.yaw;
        demo.session.client.view.pitch = demo.session.client.spawn.placement.pitch;
    } else {
        demo.session.client.view.yaw = play_sunset::kViewYaw;
        demo.session.client.view.pitch = play_sunset::kViewPitch;
    }
    if (auto* camera = demo.session.client.world.get<render::Camera>(demo.visuals->camera)) {
        // The outer 8x8 LOD5 square guarantees at least 4 km of ground around the eye,
        // including immediately before a recenter. Show several kilometres of that cover.
        play_sunset::configure_camera(*camera);
    }
    if (demo.visuals && scene_path.empty()) {
        // The sky's solar radiance is not coupled to the light's colour, so match it here and
        // lower the exposure for a sun-facing view (see play_sunset::sky).
        demo.visuals->renderer.set_sky(play_sunset::sky(Visuals::authored_sky()));
    }

    // The device->intent map's bindings, owned by main so the render callback below can read the
    // look scheme the window actually granted.
    // The requested sniper is the live default; nonmagnifying rifle sights remain selectable.
    if (demo.viewmodel)
        demo.viewmodel->select(block_play::PlayViewmodel::Weapon::Sniper);
    if (demo.viewmodel) {
        // Complete the local startup handshake before warming its replicated render scene.
        gameplay::CharacterInput idle{};
        idle.yaw = demo.session.client.view.yaw;
        idle.pitch = demo.session.client.view.pitch;
        for (unsigned i = 0; i < 30; ++i)
            demo.step_sim(gameplay_net::to_input_command(idle));
        if (!demo.warm_scope_view())
            return 1;
    }
    if (demo.session.server.spawn.status == block_play::SpawnStatus::Valid &&
        !demo.session.server.avatars.empty() && demo.session.client.predictor.seeded()) {
        // A resolved marker alone does not prove the server placed its avatar there or that
        // the client received it. Observe both live poses after the existing startup handshake.
        const auto* authoritative = demo.session.server.world.get<gameplay::CharacterState>(
            demo.session.server.avatars.front());
        const auto predicted = demo.session.client.eye_anchor();
        if (authoritative)
            std::printf("99-the-block: authored player ready: authority (%.3f, %.3f, %.3f), "
                        "predicted (%.3f, %.3f, %.3f), look (%.4f, %.4f)\n",
                        double(authoritative->position.x),
                        double(authoritative->position.y),
                        double(authoritative->position.z),
                        double(predicted.x),
                        double(predicted.y),
                        double(predicted.z),
                        double(demo.session.client.view.yaw),
                        double(demo.session.client.view.pitch));
    }
    if (!g_mute && demo.app.windowed())
        demo.audio_output.start(); // smoke/headless paths never open a device
    gameplay::InputBindings bindings{};

    if (demo.app.windowed()) {
        // ASK for the cursor, then believe the ANSWER (m15.5). A compositor that does not advertise
        // pointer constraints hands back a weaker mode, and a build that assumed otherwise would
        // put the player in free-look with a cursor still free to walk off the window — a camera
        // that stops steering mid-turn, blamed on the camera. The control scheme and the help text
        // both come from what was actually granted. Asking is not getting, and it is not getting
        // YET either. On Wayland a surface can only lock a pointer that is already over it, so this
        // request is routinely refused at startup and granted a moment later when the mouse enters
        // the window — which is why the answer is re-read every frame below rather than believed
        // once here.
        const platform::CursorMode cursor =
            demo.app.window()->set_cursor_mode(platform::CursorMode::Locked);
        bindings.look_requires_drag = gameplay::look_requires_drag_for(cursor);
        std::printf("99-the-block: presenting in a window.\n"
                    "  WASD move · %s to look · LEFT MOUSE fires (rifles hold, sniper click)\n"
                    "  1 SCAR-H · 2 M4A1 · 3 MSR / Schmidt & Bender PM II\n"
                    "  Q / RIGHT MOUSE aims · R reload animation · F detonates · ESC exits\n",
                    bindings.look_requires_drag
                        // Not "this display cannot lock": at startup the pointer is usually not
                        // over the window yet, and on Wayland that alone is enough to refuse. The
                        // scheme switches by itself the moment the lock is granted, so the text
                        // says what is true now and what will change rather than diagnosing the
                        // platform from one early answer.
                        ? "hold RIGHT-DRAG (free look engages if the cursor locks)"
                        : "MOVE THE MOUSE");
    } else {
        std::printf("99-the-block: no display available; running the script headless instead.\n");
    }

    // The live intent, rebuilt every FRAME from devices and sampled by the tick below. The frame
    // and the tick run at different rates, so the tick reads the latest fold rather than doing its
    // own — which is also what keeps the yaw the mover receives the one the player is looking
    // along.
    rhi::Extent2D play_extent = cfg.render_extent;
    block_play::PlayInput live;
    bool quit = false;
    const float look_sensitivity = demo.session.client.view.sensitivity;

    demo.app.add_sim_stage(app::Application::SimStage::PreSim, [&](ecs::World&, double) {
        auto input = live.tick();
        demo.step_play(input);
    });
    demo.app.on_render([&](app::FrameContext& ctx) {
        play_extent = ctx.extent;
        // The look scheme follows the cursor mode the window ACTUALLY holds, every frame. It is
        // live state: the lock is granted when the pointer enters, and dropped again whenever the
        // window loses focus (a background app must not keep the pointer). Deciding once at startup
        // would leave the player in drag-look for a session that locked a moment later, or — worse
        // — in free-look after the lock was released behind their back.
        if (platform::Window* win = demo.app.window()) {
            bindings.look_requires_drag = gameplay::look_requires_drag_for(win->cursor_mode());
        }
        demo.session.client.view.sensitivity =
            look_sensitivity * (demo.viewmodel && demo.viewmodel->scoped()
                                    ? 1.0f - .8f * demo.viewmodel->aim_fraction()
                                    : 1.0f);
        const gameplay::FrameIntent intent = gameplay::map_frame_input(
            demo.session.client.devices, ctx.input, demo.session.client.view, bindings);
        live.frame(intent.character, ctx.input, bindings.fire, gameplay::kActionFire);
        if (intent.quit) {
            quit = true;
            demo.app.request_quit();
        }
        if (demo.session.client.devices.key_pressed(platform::Key::F)) {
            (void)demo.fire_charge_aimed();
        }
        if (demo.viewmodel) {
            // map_frame_input has already folded this frame's events and applied mouse motion.
            // Read the SAME held button in both locked and right-drag fallback modes.
            demo.viewmodel->apply_controls(demo.session.client.devices, ctx.input);
            // Presentation follows predicted motion and accepted server shots; neither bob nor
            // weapon choice enters CharacterInput or the replicated simulation state.
            const auto velocity = demo.session.client.predictor.seeded()
                                      ? demo.session.client.predictor.state().velocity
                                      : core::Vec3{};
            demo.viewmodel->advance(
                static_cast<float>(ctx.frame_dt), velocity, demo.shot_presented);
            if (demo.weapon_fx && demo.viewmodel->take_case_ejection())
                demo.weapon_fx->queue_case();
            demo.shot_presented = false;
        }
        demo.render(ctx);
    });

    if (const char* trace = std::getenv("RIME_SHIMMER_TRACE")) {
        run_shimmer_trace(demo, trace);
    } else if (demo.app.windowed()) {
        if (frames)
            demo.app.run_frames(static_cast<int>(frames));
        else
            (void)demo.app.run();
    } else {
        demo.app.run_frames(static_cast<int>(frames ? frames : g_ticks));
    }
    std::printf("99-the-block: %llu frames, %llu ticks%s\n",
                static_cast<unsigned long long>(demo.app.frame_index()),
                static_cast<unsigned long long>(demo.app.tick_count()),
                quit ? " (ESC)" : "");
    demo.app.finish_gpu(); // first-person resources retire before Visuals and their registries
    if (!g_ppm.empty() && demo.visuals && demo.visuals->last_ldr.is_valid()) {
        const auto pixels = read_rgba8(*demo.app.device(),
                                       demo.app.graph()->physical(demo.visuals->last_ldr),
                                       play_extent.width,
                                       play_extent.height);
        write_ppm(g_ppm.c_str(), pixels, play_extent.width, play_extent.height);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode {
        Headless,
        Play,
        Perf,
        WorldSmoke,
        WorldGpuSmoke,
        ViewmodelSmoke,
        EnvironmentSmoke,
        EffectsSmoke
    } mode = Mode::Headless;
    PerfOptions perf{};
    unsigned play_frames = 0;
    std::string write_city;
    std::filesystem::path content_override;
    bool explicit_mode = false;
    bool scene_info = false;
    std::filesystem::path cooked = RIME_BLOCK_COOKED_DIR;
    std::string scene_path; // --scene: run a scene someone authored, not the generated one (m14.4)

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--help" || a == "-h") {
            std::printf("the_block --play [--scene file.rscene] [--frames N] [--ppm file.ppm]\n"
                        "          [--network-impairment] [--mute]\n"
                        "          --content dir | --cooked dir --viewmodel-content dir\n"
                        "          --write-city new-file.rscene | --scene-info | --headless\n"
                        "A copied development bundle starts windowed play with no arguments.\n"
                        "WASD move, mouse look, left mouse fire, Q/right mouse aim, R reload, "
                        "1/2/3 weapons, Esc quit.\n");
            return 0;
        }
        if (a == "--play" || a == "--headless" || a == "--perf" || a == "--world-smoke" ||
            a == "--world-gpu-smoke" || a == "--viewmodel-smoke" || a == "--environment-smoke" ||
            a == "--effects-smoke")
            explicit_mode = true;
        if (a == "--scene-info") {
            scene_info = true;
        } else if (a == "--write-city" && i + 1 < argc) {
            write_city = argv[++i];
        } else if (a == "--content" && i + 1 < argc) {
            content_override = argv[++i];
        } else if (a == "--viewmodel-content" && i + 1 < argc) {
            g_viewmodel_content = argv[++i];
        } else if (a == "--headless") {
            mode = Mode::Headless;
        } else if (a == "--play") {
            mode = Mode::Play;
        } else if (a == "--world-smoke") {
            mode = Mode::WorldSmoke;
        } else if (a == "--world-gpu-smoke") {
            mode = Mode::WorldGpuSmoke;
        } else if (a == "--viewmodel-smoke") {
            mode = Mode::ViewmodelSmoke;
        } else if (a == "--environment-smoke") {
            mode = Mode::EnvironmentSmoke;
        } else if (a == "--effects-smoke") {
            mode = Mode::EffectsSmoke;
        } else if (a == "--network-impairment") {
            g_network_impairment = true;
        } else if (a == "--mute") {
            g_mute = true;
        } else if (a == "--effects-preview") {
            g_effects_preview = true;
        } else if (a == "--perf") {
            mode = Mode::Perf;
        } else if (a == "--frames" && i + 1 < argc) {
            perf.frames = std::atoi(argv[++i]);
            if (perf.frames < 1 || perf.frames > 1000000) {
                std::fprintf(stderr, "--frames must be 1..1000000\n");
                return 2;
            }
            play_frames = static_cast<unsigned>(perf.frames);
        } else if (a == "--pipelined" && i + 1 < argc) {
            // Checked as a SIGNED int before the cast. `--pipelined -1` used to become 4294967295,
            // which is not merely a big ring: `in_flight_.size() >= headless_in_flight_` is then
            // permanently false, so nothing is ever retired, the in-flight deque grows without
            // bound, and every submission's timestamp pool leaks along with it. A flag that turns a
            // typo into an unbounded allocation is the parser's bug, not the caller's.
            const int depth = std::atoi(argv[++i]);
            if (depth < 1 || depth > static_cast<int>(kMaxPipelineDepth)) {
                std::fprintf(stderr,
                             "99-the-block: --pipelined must be 1..%u (got '%s')\n",
                             kMaxPipelineDepth,
                             argv[i]);
                return 2;
            }
            perf.pipelined = static_cast<std::uint32_t>(depth);
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
        } else if (a == "--island-profile") {
            g_island_profile = true;
        }
    }

    if (!write_city.empty()) {
        if (std::filesystem::exists(write_city)) {
            std::fprintf(stderr, "city generator refuses to overwrite %s\n", write_city.c_str());
            return 2;
        }
        ecs::World authored;
        register_all(authored);
        blockkit::BlockParams params;
        if (const auto text = platform::read_file(cooked / "manifest.txt")) {
            if (const auto manifest = assets::Manifest::parse(
                    {reinterpret_cast<const char*>(text->data()), text->size()}))
                params.ground_material = blockkit::ground_material_id(*manifest);
        }
        (void)blockkit::assemble(authored, params);
        const auto expanded = block_play::city::expand(authored);
        // A saved city owns its environment. The editor and full runtime both consume this
        // Sky and authored directional light; no runtime-only preset overwrites the file.
        render::Sky sky;
        sky.intensity =
            .06f; // calibrated scene exposure: physical sky light must not wash out the map
        (void)authored.spawn_with(sky);
        if (!scene::save_scene_file(authored, write_city))
            return 1;
        ecs::World loaded;
        register_all(loaded);
        const auto result = scene::load_scene_file(loaded, write_city);
        if (!result.ok || result.skipped_components ||
            loaded.entity_count() != authored.entity_count())
            return 1;
        std::printf("city authored: %zu entities, %u added buildings, %u lamps, %u barriers; "
                    "strict reload 0 skipped\n",
                    authored.entity_count(),
                    expanded.buildings_added,
                    expanded.lamps,
                    expanded.barriers);
        return 0;
    }
    // A moved development binary must find its own content, never silently use the checkout's
    // compiled directories. In-tree tools retain their existing defaults and self-check mode.
    std::error_code content_error;
    const auto executable = platform::executable_path();
    const bool in_tree =
        std::filesystem::equivalent(executable.parent_path(), RIME_GAME_BINARY_DIR, content_error);
    if (!content_override.empty() || !in_tree) {
        app::ContentSearch search;
        search.explicit_dir = content_override;
        search.required = "city.rscene";
        const auto root = app::resolve_content_root(search);
        if (!root.ok) {
            std::fprintf(stderr, "%s\n", app::describe_content_failure(root).c_str());
            return 2;
        }
        cooked = root.dir / "block";
        g_viewmodel_content = root.dir / "viewmodels";
        if (scene_path.empty())
            scene_path = (root.dir / "city.rscene").string();
        if (!explicit_mode)
            mode = Mode::Play;
        std::printf("city content: %s\n", root.dir.string().c_str());
    }

    if (scene_info) {
        if (scene_path.empty()) {
            std::fprintf(stderr, "--scene-info needs --scene or bundle content\n");
            return 2;
        }
        ecs::World authored;
        register_all(authored);
        const auto loaded = scene::load_scene_file(authored, scene_path);
        if (!loaded.ok || loaded.skipped_components) {
            std::fprintf(stderr, "scene-info strict load failed: %s\n", loaded.error.c_str());
            return 2;
        }
        Peer peer;
        authored.query<destruction::Destructible>().for_each([&](ecs::Entity, const auto& d) {
            peer.city_patterns |= d.asset == block_play::city::kLampAsset ||
                                  d.asset == block_play::city::kBarrierAsset;
        });
        // Exercise the actual game's pattern registration and strict stand-up, without a device
        // or a chaotic collapse threshold. The same measured digest is printed by live startup.
        if (!peer.register_patterns(cooked) ||
            !peer.stand_up(destruction::Authority::Local, true, scene_path, 0))
            return 2;
        std::printf("city scene-info: %zu entities, %zu destructibles, digest 0x%016llx, "
                    "city-patterns=%d, no GPU\n",
                    peer.measured.entities,
                    peer.measured.destructibles,
                    static_cast<unsigned long long>(peer.measured.placement_digest),
                    peer.city_patterns);
        return 0;
    }

    if (mode == Mode::Perf) {
        // A short probe run (scripts/perf.sh uses --frames 12) must not try to fire the charge
        // after the run has ended, nor demand a collapse window it cannot fill.
        perf.charge_frame = std::min(perf.charge_frame, perf.frames / 2);
        perf.collapse = std::min(perf.collapse, perf.frames - perf.charge_frame);
        return run_perf(cooked, scene_path, perf);
    }
    if (mode == Mode::Play) {
        return run_play(cooked, scene_path, play_frames);
    }
    if (mode == Mode::WorldSmoke) {
        return run_world_smoke(cooked, scene_path);
    }
    if (mode == Mode::WorldGpuSmoke) {
        return run_world_gpu_smoke(cooked, scene_path);
    }
    if (mode == Mode::ViewmodelSmoke) {
        return run_viewmodel_smoke(cooked, scene_path);
    }
    if (mode == Mode::EnvironmentSmoke) {
        return run_environment_smoke(cooked, scene_path);
    }
    if (mode == Mode::EffectsSmoke) {
        return run_effects_smoke(cooked, scene_path);
    }
    return run_headless(cooked, scene_path);
}
