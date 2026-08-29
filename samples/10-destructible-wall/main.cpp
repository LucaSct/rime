// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 10-destructible-wall — Milestone 8's "done when": a cooked destructible wall stands on the
// ground, takes a hit, sheds part of itself as tumbling debris, and drives one destruction event
// stream out to three unaware consumers (a VFX dust puff, the null audio backend, and gameplay
// tallies). It closes M8 the way 08-gltf-zoo closed M6: the milestone ends in a runnable proof, not
// a compile.
//
// The whole M8 pipeline in one sample:
//   • cook   — `rime fracture --size 4 3 0.3 --parts 60 --seed 11 --out <dir> --name wall` writes a
//              Destructible RMA1 asset (seeded-Voronoi part hulls + a bond/anchor graph). A CTest
//              fixture runs it before the self-check.
//   • stand  — engine/destruction registers the pattern ONCE (each part a convex hull, the whole a
//              compound) and spawns an instance: ONE static compound body (ADR-0029 §1). Per-part
//              RENDER LEAVES draw it — the leaves M8.2/8.3 deferred to here, where a device exists.
//   • break  — damage (a hitscan, a thrown CCD marble, or a blast) erodes parts; a support solve
//              finds the unanchored remainder; the fracture BODY SWAP re-registers the anchored
//              stump and spawns each detached island as a dynamic body (ADR-0029 §2).
//   • fan-out— one event stream (PartDamaged/PartDied/IslandDetached/DebrisSettled, M8.4) feeds the
//              dust field, the audio log, and gameplay — none of which destruction knows about.
//   • settle — debris fall, come to rest, and (with the M8.5 lifecycle on) freeze, so the physics
//              stores stay bounded.
//
// Run it:   build/dev/bin/destructible_wall --headless [--cooked <dir>] [--ppm out.ppm]
//           build/dev/bin/destructible_wall --serve [--cooked <dir>] [--host 0.0.0.0] [--port 9100]
//           build/dev/bin/destructible_wall --perf [--cooked <dir>] [--out r.json] [--baseline b]
//
// The headless self-check is the CI-gated done-when and is GPU-FREE at its core: the destruction
// simulation is verified with no device (it runs on every OS), and the pixel proof runs only where
// a Vulkan device is present (lavapipe on Linux CI).
//
// --perf is the hardware report (m12.0-perf / ADR-0035 §2b) and is NOT a CTest target — lavapipe
// has no wall clock worth gating. Where 11-lit-rooms --perf measures a render-bound frame, this one
// measures the SIMULATION under a collapse: the fracture tick is the exact moment that must not
// hitch, and it is the moment a mean would hide.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/destructible_asset.hpp"
#include "rime/audio/audio.hpp"
#include "rime/audio/bank.hpp"
#include "rime/audio/mixer.hpp"
#include "rime/audio/wav.hpp"
#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"
#include "rime/vfx/dust.hpp"

namespace {

using namespace rime;

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720;

#ifndef RIME_WALL_COOKED_DIR
#define RIME_WALL_COOKED_DIR "cooked"
#endif

// Sound ids the sample and its (null) audio backend agree on — gameplay's vocabulary until the
// asset pipeline mints hashed ids (au1). One is enough for v1: a chunk of concrete letting go.
constexpr audio::SoundId kSoundImpactConcrete = 1;

// ── Cooked hull part → a render mesh ─────────────────────────────────────────────────────────────
// A destructible part is a convex polyhedron in CSR form (per-face CCW vertex loops). Triangulate
// each face as a fan and give the three vertices the face's FLAT normal (the geometric normal of a
// convex face is exact — a smooth normal would lie about a faceted chunk of rubble). Positions are
// COM-centred already (the cook re-centred them), which is exactly the local frame a render leaf's
// transform expects.
render::CpuMesh hull_to_cpu_mesh(const assets::DestructiblePart& part) {
    render::CpuMesh mesh;
    std::uint32_t base = 0; // running offset into face_indices
    for (const std::uint32_t count : part.face_counts) {
        if (count >= 3) {
            const core::Vec3 v0 = part.vertices[part.face_indices[base]];
            const core::Vec3 v1 = part.vertices[part.face_indices[base + 1]];
            const core::Vec3 v2 = part.vertices[part.face_indices[base + 2]];
            core::Vec3 nrm = core::cross(v1 - v0, v2 - v0);
            const float len = core::length(nrm);
            nrm = len > 1e-8f ? nrm * (1.0f / len) : core::Vec3{0.0f, 1.0f, 0.0f};
            const auto push = [&](core::Vec3 p) {
                render::MeshVertex v;
                v.px = p.x;
                v.py = p.y;
                v.pz = p.z;
                v.nx = nrm.x;
                v.ny = nrm.y;
                v.nz = nrm.z;
                mesh.vertices.push_back(v);
            };
            // Fan v0–v(k)–v(k+1): each triangle its own three vertices, so the flat normal is not
            // shared/averaged across faces (independent triangles, faceted look).
            for (std::uint32_t k = 1; k + 1 < count; ++k) {
                const auto i = static_cast<std::uint32_t>(mesh.vertices.size());
                push(v0);
                push(part.vertices[part.face_indices[base + k]]);
                push(part.vertices[part.face_indices[base + k + 1]]);
                mesh.indices.push_back(i);
                mesh.indices.push_back(i + 1);
                mesh.indices.push_back(i + 2);
            }
        }
        base += count;
    }
    render::compute_tangents(mesh); // the forward pipeline is always-tangented (M6.4)
    return mesh;
}

// The volume-weighted COM of a set of parts, in the destructible frame — the local origin a debris
// body sits at (register_compound re-centred its children on exactly this point), so a part's
// offset inside its debris body is `part.com − this`.
core::Vec3 subset_com(const assets::DestructibleAsset& asset,
                      std::span<const std::uint32_t> parts) {
    core::Vec3 c{0.0f, 0.0f, 0.0f};
    float vol = 0.0f;
    for (const std::uint32_t p : parts) {
        c = c + asset.parts[p].com * asset.parts[p].volume;
        vol += asset.parts[p].volume;
    }
    return vol > 1e-8f ? c * (1.0f / vol) : c;
}

// ── The destruction fan-out consumers (M8.4, ADR-0029 §7) ────────────────────────────────────────
// Three systems read ONE event span and none knows the others exist (guardrail 2): a dust field
// blooms a puff where a part broke, the null audio backend logs an impact, and gameplay tallies
// what happened. This is the seam the whole engine's feedback loops (VFX, sound, score, AI) hang
// off.
struct Listeners {
    vfx::DustField dust;
    audio::NullAudioBackend audio;

    // m13.4: the REAL mixer, fed the same events as the null backend and no more. Optional and
    // borrowed — the sample's self-check still asserts on `audio.log()`, so wiring a mixer in
    // cannot move a single number the proof depends on. When it is null this is exactly the M8.4
    // sample it has always been.
    audio::Mixer* mixer = nullptr;

    void sound(audio::SoundId id, core::Vec3 at, float gain) {
        audio.play(kSoundImpactConcrete, at, gain); // the M8.4 log, unchanged
        if (mixer != nullptr) {
            mixer->play(id, at, gain);
        }
    }

    std::uint32_t parts_damaged = 0;
    std::uint32_t parts_died = 0;
    std::uint32_t islands_detached = 0;
    std::uint32_t debris_settled = 0;
    std::uint32_t debris_evicted = 0; // m13.2b: rubble past the VISUAL budget

    void consume(std::span<const destruction::DestructionEvent> events) {
        using K = destruction::DestructionEventKind;
        for (const destruction::DestructionEvent& e : events) {
            const core::Vec3 lo = e.world_bounds.min;
            const core::Vec3 hi = e.world_bounds.max;
            const core::Vec3 centre = (lo + hi) * 0.5f;
            switch (e.kind) {
                case K::PartDamaged:
                    ++parts_damaged;
                    break;
                case K::PartDied:
                    ++parts_died;
                    dust.emit_burst(lo, hi, 0.6f);
                    sound(audio::sound::kPartBreak, centre, std::min(1.0f, e.magnitude));
                    break;
                case K::IslandDetached:
                    ++islands_detached;
                    dust.emit_burst(lo, hi, 1.0f);
                    // An island coming free is the big one — the low, long sound.
                    sound(audio::sound::kCollapse, centre, 1.0f);
                    break;
                case K::DebrisSettled:
                    ++debris_settled;
                    // Rubble coming to rest. Quiet on purpose: a settle per chunk is dozens of
                    // events, and at full gain they would drown the break that caused them.
                    if (mixer != nullptr) {
                        mixer->play(audio::sound::kDebrisSettle, centre, 0.35f);
                    }
                    break;
                case K::DebrisEvicted:
                    // ADR-0032 C6 (m13.2b): this rubble has crossed the VISUAL budget. A renderer
                    // drops its leaf here, and a lighting cache invalidates `world_bounds` exactly
                    // as it does for PartDied — same payload, same channel. This sample has no
                    // per-debris leaf to drop, so it only counts; the counter is what makes the
                    // eviction visible instead of silent.
                    ++debris_evicted;
                    break;
            }
        }
    }
};

// ── The simulation (GPU-FREE): stand the wall, hit it, step it ───────────────────────────────────
// Everything the done-when asserts lives here, deviceless — so it runs on every CI OS and is the
// deterministic core the pixel proof only decorates. run_sim() is a PURE function of (asset, cfg):
// the same inputs on any worker count produce the same fingerprints (the M11 contract).
constexpr float kDt = 1.0f / 60.0f;

struct SimConfig {
    int workers = -1; // physics job-system threads; −1 = sequential
    int ticks = 300;  // sim ticks to run after the hit

    // m13.4: write the destruction's own mixdown here (16-bit stereo WAV). Null = no audio work at
    // all, which is the default and what every existing proof runs. The mixer's real proof is the
    // offline one in tests/audio; this is how a HUMAN hears it, driven by the same events.
    const char* wav = nullptr;
};

struct SimResult {
    std::uint64_t state_hash = 0;
    std::uint64_t world_hash = 0;
    std::uint32_t parts_died = 0;
    std::uint32_t islands_detached = 0;
    std::size_t debris_count = 0;
    std::uint32_t audio_calls = 0;
    bool dust_fired = false;     // the dust field bloomed on the break
    bool debris_settled = false; // the world came to rest by `ticks`

    // The WORK the run did (m12.0-perf / ADR-0035 §2a). These are integer counts, not clocks, so
    // they are identical on every machine and CI can gate them on lavapipe forever. They live in
    // SimResult rather than beside it deliberately: `operator==` drives the determinism check
    // below, so the counters are held to the same bit-identical-across-worker-counts standard as
    // the state hash. A counter that wobbled with thread count would be useless as a budget and is
    // now caught by the proof that already exists.
    std::uint32_t ticks_run = 0;
    std::uint32_t bodies_final = 0;
    std::uint32_t awake_final = 0; // must reach 0: the M7.5 sleep payoff, made falsifiable
    std::uint32_t broadphase_pairs_peak = 0;
    std::uint32_t manifolds_peak = 0;
    std::uint32_t contact_points_peak = 0;
    std::uint32_t largest_island_peak = 0; // the serial tail of the parallel solve

    bool operator==(const SimResult&) const = default;
};

// The sample's ledger, assembled by the CONSUMER — `core` knows nothing about physics or
// destruction, and physics knows nothing about ledgers. Gluing the two is the sample's job, the
// same arrow discipline that keeps `vfx` from ever linking `destruction`.
core::WorkLedger make_ledger(const SimResult& r) {
    core::WorkLedger ledger;
    // Insertion order is the reading order: sim, then destruction, then physics.
    ledger.set("sim.ticks_run", r.ticks_run);
    ledger.set("destruction.parts_died", r.parts_died);
    ledger.set("destruction.islands_detached", r.islands_detached);
    ledger.set("destruction.debris_count", static_cast<std::uint64_t>(r.debris_count));
    ledger.set("audio.calls", r.audio_calls);
    ledger.set("physics.bodies_final", r.bodies_final);
    ledger.set("physics.awake_final", r.awake_final);
    ledger.set("physics.broadphase_pairs_peak", r.broadphase_pairs_peak);
    ledger.set("physics.manifolds_peak", r.manifolds_peak);
    ledger.set("physics.contact_points_peak", r.contact_points_peak);
    ledger.set("physics.largest_island_peak", r.largest_island_peak);
    return ledger;
}

// The budget. Ceilings bound the work; FLOORS are the load-bearing half, because every ceiling in
// here is also satisfied by a run that did nothing at all — an intact wall has 0 dead parts, 0
// debris and 0 contacts, and would sail under every upper bound. This is the same vacuity trap
// m11.7 fell into and then made an assertion; here it is a budget.
core::WorkBudget wall_budget(int tick_limit) {
    core::WorkBudget b;
    // Floors: the scenario actually happened.
    b.at_least("destruction.parts_died", 1)
        .at_least("destruction.islands_detached", 1)
        .at_least("destruction.debris_count", 1)
        .at_least("audio.calls", 1)
        // Debris that never touched anything would report zero contacts, which would mean the
        // solver did no work and "it settled" was true only because nothing ever moved.
        .at_least("physics.contact_points_peak", 1);
    // Ceilings: the work stayed bounded.
    b.at_most("physics.awake_final", 0) // everything went to sleep — nothing is still churning
        .at_most("sim.ticks_run", static_cast<std::uint64_t>(tick_limit) - 1); // settled in budget
    return b;
}

// "Cut the legs out": sever a thin seam just above the anchored base, across the width. A few
// overlapping blasts kill that low row, so everything above it loses its anchor and drops as ONE
// big rigid island (a lone part rarely strands a Voronoi neighbour — a full-width seam reliably
// detaches a supported chunk, the M8.3 proof's shape). Severing LOW keeps the killed count small
// and detaches the wall as a single body (which settles fast) rather than pulverising the middle
// into a rubble pile; the modest +Z impulse topples the freed slab forward off the stump.
void hit_base_seam(destruction::DestructionWorld& dw, destruction::InstanceId inst) {
    for (float x = -1.6f; x <= 1.6f; x += 0.5f) {
        dw.apply_damage(inst, core::Vec3{x, -1.0f, 0.0f}, 0.4f, 6.0f, core::Vec3{0, 0, 8.0f});
    }
}

SimResult run_sim(const assets::DestructibleAsset& asset, SimConfig cfg) {
    physics::PhysicsWorld pw;
    destruction::DestructionWorld dw;
    destruction::LifecycleConfig life;
    life.enabled = true; // M8.5: the lifecycle is armed (a cap bounds a runaway debris count)…
    life.freeze_delay_ticks = 100000; // …but the linger outlasts this settle window, so freezing
    life.max_live_debris = 48;        // never churns the pile mid-settle (freeing a body under a
    dw.configure_lifecycle(life); // pile wakes it — the freeze is proven in lifecycle_test.cpp).

    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    // The cooked wall is 4×3×0.3 m centred on the origin (base at y = −1.5); stand it on a ground
    // plane at that base so debris lands and does real solver work.
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    physics::BodyDesc ground;
    ground.motion = physics::MotionType::Static;
    ground.shape.type = physics::ShapeType::Box;
    ground.shape.half_extents = {50.0f, 0.5f, 50.0f};
    ground.position = {0.0f, -2.0f, 0.0f};
    (void)pw.create_body(ground);

    std::unique_ptr<core::JobSystem> js;
    if (cfg.workers >= 0) {
        js = std::make_unique<core::JobSystem>(static_cast<unsigned>(cfg.workers));
        pw.set_job_system(js.get());
    }

    Listeners listeners;

    // m13.4: an optional real mixdown, rendered in LOCKSTEP with the simulation — one tick is one
    // tick's worth of samples, so the WAV's timeline IS the sim's timeline. Nothing about the
    // proof's counters changes; the null backend still logs exactly what it logged at M8.4.
    std::unique_ptr<audio::Mixer> mixer;
    std::vector<float> mixdown;
    const std::size_t frames_per_tick =
        static_cast<std::size_t>(static_cast<double>(audio::kSampleRate) * kDt);
    if (cfg.wav != nullptr) {
        audio::MixerConfig mc;
        mc.reference_distance = 1.5f;
        mc.max_distance = 40.0f;
        mixer = std::make_unique<audio::Mixer>(audio::SoundBank::engine_defaults(), mc);
        // A listener a few metres back from the wall, facing it. Off to one side on purpose: a
        // listener dead in front would pan everything to centre and the stereo field would be a
        // claim nobody could hear.
        audio::Listener l;
        l.position = {-2.0f, 1.0f, 6.0f};
        mixer->set_listener(l);
        listeners.mixer = mixer.get();
    }

    hit_base_seam(dw, inst);
    bool settled = false;
    float peak_dust = 0.0f;
    SimResult peaks;
    int ticks_run = 0;
    for (int t = 0; t < cfg.ticks; ++t) {
        pw.step(kDt);
        dw.update(pw);
        listeners.consume(dw.events()); // the fan-out, every tick
        if (mixer) {
            const std::size_t before = mixdown.size();
            mixdown.resize(before + frames_per_tick * 2u);
            mixer->render(std::span<float>(mixdown).subspan(before));
        }
        peak_dust =
            std::max(peak_dust, listeners.dust.coverage()); // catch the bloom before it ages
        listeners.dust.simulate(kDt);

        // Peaks, not end-of-run values: the collapse is the interesting moment, and by the time
        // the pile is asleep every load counter has fallen back to ~0. A ledger sampled only at
        // rest would report a world that never did anything.
        const physics::WorldStats s = pw.stats();
        peaks.broadphase_pairs_peak = std::max(peaks.broadphase_pairs_peak, s.broadphase_pairs);
        peaks.manifolds_peak = std::max(peaks.manifolds_peak, s.manifolds);
        peaks.contact_points_peak = std::max(peaks.contact_points_peak, s.contact_points);
        peaks.largest_island_peak = std::max(peaks.largest_island_peak, s.largest_island);

        ticks_run = t + 1;
        if (t > 4 && s.awake_bodies == 0) {
            settled = true; // came to rest (step FIRST, then gate — debris are born at rest)
            break;
        }
    }

    if (mixer) {
        // Tail: keep rendering past the last event so the collapse's own decay is in the file
        // rather than cut off at whatever tick the solver happened to fall asleep on.
        for (int i = 0; i < 90 && mixer->voice_count() > 0; ++i) {
            const std::size_t before = mixdown.size();
            mixdown.resize(before + frames_per_tick * 2u);
            mixer->render(std::span<float>(mixdown).subspan(before));
        }
        if (audio::write_wav(cfg.wav, mixdown, audio::kSampleRate, 2)) {
            const audio::MixStats& ms = mixer->stats();
            std::printf("  wrote %s — %.2f s, %llu voices, peak %.2f, %llu clipped\n", cfg.wav,
                        static_cast<double>(mixdown.size() / 2u) / audio::kSampleRate,
                        static_cast<unsigned long long>(ms.voices_started),
                        static_cast<double>(ms.peak),
                        static_cast<unsigned long long>(ms.clipped_samples));
        } else {
            std::fprintf(stderr, "  could not write %s\n", cfg.wav);
        }
    }

    SimResult r;
    const physics::WorldStats final_stats = pw.stats();
    r.ticks_run = static_cast<std::uint32_t>(ticks_run);
    r.bodies_final = final_stats.body_count;
    r.awake_final = final_stats.awake_bodies;
    r.broadphase_pairs_peak = peaks.broadphase_pairs_peak;
    r.manifolds_peak = peaks.manifolds_peak;
    r.contact_points_peak = peaks.contact_points_peak;
    r.largest_island_peak = peaks.largest_island_peak;
    r.state_hash = dw.state_hash();
    r.world_hash = pw.world_hash();
    r.parts_died = listeners.parts_died;
    r.islands_detached = listeners.islands_detached;
    r.debris_count = dw.debris_count();
    r.audio_calls = static_cast<std::uint32_t>(listeners.audio.log().size());
    r.dust_fired = peak_dust > 0.0f; // the dust field bloomed on the break (before it decayed away)
    r.debris_settled = settled;
    return r;
}

// ── The GPU scene: per-part render leaves following the sim
// ─────────────────────────────────────── One render-leaf entity per part draws the wall. Each
// frame every leaf's transform is refreshed from the sim: a standing part rides its instance
// placement (part_placement), a detached part rides its debris body (body pose composed with the
// part's offset inside that body). This is the per-part render representation ADR-0029 §5 names,
// deferred from M8.2/8.3 to here.
struct WallScene {
    app::Application& app;
    const assets::DestructibleAsset& asset;
    physics::PhysicsWorld pw;
    destruction::DestructionWorld dw;
    Listeners listeners;
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    render::SceneRenderer renderer;
    destruction::PatternId pattern{};
    destruction::InstanceId inst{};
    std::vector<ecs::Entity> leaves; // one per part, index = part id
    render::RGTexture last_ldr{};

    WallScene(app::Application& application, const assets::DestructibleAsset& a)
        : app(application), asset(a), meshes(*app.device()),
          renderer(*app.device(), meshes, materials) {
        ecs::World& world = app.world();
        render::register_render_components(world);

        destruction::LifecycleConfig life;
        life.enabled = true;
        life.freeze_delay_ticks = 40;
        life.max_live_debris = 48;
        dw.configure_lifecycle(life);
        pattern = dw.register_pattern(asset, pw);
        inst = dw.spawn(pattern, core::Transform{}, pw);
        physics::BodyDesc ground;
        ground.motion = physics::MotionType::Static;
        ground.shape.type = physics::ShapeType::Box;
        ground.shape.half_extents = {50.0f, 0.5f, 50.0f};
        ground.position = {0.0f, -2.0f, 0.0f};
        (void)pw.create_body(ground);

        build_stage();
        build_leaves();
        refresh_leaves();

        renderer.set_ambient(0.05f, 0.05f, 0.06f);
        app.on_render([this](app::FrameContext& ctx) {
            last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        });
    }

    WallScene(const WallScene&) = delete;
    WallScene& operator=(const WallScene&) = delete;

    void build_stage() {
        using ecs::WorldTransform;
        ecs::World& world = app.world();

        const render::MeshId floor = meshes.add(render::make_plane(20.0f, 8.0f), "floor");
        render::PbrMaterialDesc floor_mat{};
        floor_mat.base_color[0] = 0.35f;
        floor_mat.base_color[1] = 0.35f;
        floor_mat.base_color[2] = 0.38f;
        floor_mat.roughness = 0.9f;
        core::Transform floor_tf{};
        floor_tf.translation = {0.0f, -2.0f, 0.0f};
        (void)world.spawn_with(WorldTransform{floor_tf},
                               render::MeshRef{floor},
                               render::MaterialRef{materials.add(floor_mat)});

        (void)world.spawn_with(
            WorldTransform{camera_transform(0.0f, 0.15f, 12.0f, {0.0f, 0.0f, 0.0f})},
            render::Camera{});

        core::Transform sun_tf{};
        sun_tf.rotation = core::normalize(core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.9f) *
                                          core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.5f));
        (void)world.spawn_with(WorldTransform{sun_tf},
                               render::DirectionalLight{1.0f, 0.96f, 0.88f, 2.4f});
        core::Transform lt{};
        lt.translation = {4.0f, 4.0f, 5.0f};
        (void)world.spawn_with(WorldTransform{lt},
                               render::PointLight{1.0f, 0.9f, 0.8f, 2.0f, 30.0f});
    }

    // One concrete-ish material shared by every part, one mesh + leaf entity per part.
    void build_leaves() {
        using ecs::WorldTransform;
        render::PbrMaterialDesc concrete{};
        concrete.base_color[0] = 0.62f;
        concrete.base_color[1] = 0.60f;
        concrete.base_color[2] = 0.57f;
        concrete.metallic = 0.0f;
        concrete.roughness = 0.85f;
        const render::MaterialId mat = materials.add(concrete);
        leaves.resize(asset.parts.size());
        for (std::uint32_t p = 0; p < asset.parts.size(); ++p) {
            const render::MeshId mesh = meshes.add(hull_to_cpu_mesh(asset.parts[p]), "part");
            leaves[p] = app.world().spawn_with(
                WorldTransform{}, render::MeshRef{mesh}, render::MaterialRef{mat});
        }
    }

    // Each part leaf follows the sim: standing → its instance placement; detached → its debris body
    // pose composed with the part's offset within that body; frozen/gone → left at its last pose
    // (the freeze keeps the visual where the physics body used to be).
    void refresh_leaves() {
        using ecs::WorldTransform;
        for (std::uint32_t p = 0; p < leaves.size(); ++p) {
            if (dw.part_alive(inst, p)) {
                *app.world().get<WorldTransform>(leaves[p]) =
                    WorldTransform{dw.part_placement(inst, p)};
            }
        }
        // Detached parts: walk the debris roster and pose each member from its live body.
        for (std::size_t d = 0; d < dw.debris_count(); ++d) {
            const physics::BodyId body = dw.debris_body(d);
            physics::BodyState st{};
            if (!pw.get_body_state(body, st)) {
                continue; // frozen/absent — leave the leaf at its last pose
            }
            const std::span<const std::uint32_t> parts = dw.debris_parts(d);
            const core::Vec3 com = subset_com(asset, parts);
            for (const std::uint32_t p : parts) {
                if (p >= leaves.size()) {
                    continue;
                }
                core::Transform tf;
                tf.rotation = st.orientation;
                tf.translation =
                    st.position + core::rotate(st.orientation, asset.parts[p].com - com);
                *app.world().get<WorldTransform>(leaves[p]) = WorldTransform{tf};
            }
        }
    }

    void step_and_refresh() {
        pw.step(kDt);
        dw.update(pw);
        listeners.consume(dw.events());
        listeners.dust.simulate(kDt);
        refresh_leaves();
    }

    std::vector<std::uint8_t> readback() {
        return read_rgba8(*app.device(), app.graph()->physical(last_ldr), kWidth, kHeight);
    }

    static core::Transform camera_transform(float yaw, float pitch, float dist, core::Vec3 target) {
        const core::Quat q =
            core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                            core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -pitch));
        core::Transform t{};
        t.rotation = q;
        t.translation = target + core::rotate(q, {0.0f, 0.0f, 1.0f}) * dist;
        return t;
    }

    static std::vector<std::uint8_t>
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
};

// ── Structural pixel proofs (headless, only where a device exists) ───────────────────────────────
bool scene_is_lit(const std::vector<std::uint8_t>& px) {
    std::uint64_t lit = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        if ((px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3 > 25) {
            ++lit;
        }
    }
    return lit > n / 50;
}

std::uint64_t
image_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int thresh) {
    std::uint64_t n = 0;
    const std::size_t px = std::min(a.size(), b.size()) / 4;
    for (std::size_t i = 0; i < px; ++i) {
        const int d = std::abs(a[i * 4] - b[i * 4]) + std::abs(a[i * 4 + 1] - b[i * 4 + 1]) +
                      std::abs(a[i * 4 + 2] - b[i * 4 + 2]);
        if (d > thresh) {
            ++n;
        }
    }
    return n;
}

void write_ppm(const char* path, const std::vector<std::uint8_t>& px) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i) {
        std::fwrite(&px[i * 4], 1, 3, f);
    }
    std::fclose(f);
    std::printf("  wrote %s\n", path);
}

app::AppConfig gpu_config() {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    return cfg;
}

std::filesystem::path default_cooked() {
    return RIME_WALL_COOKED_DIR;
}

std::optional<assets::DestructibleAsset> load_wall(const std::filesystem::path& cooked_dir) {
    const auto bytes = platform::read_file(cooked_dir / "wall.rdest");
    if (!bytes) {
        std::fprintf(
            stderr,
            "10-destructible-wall: no wall.rdest at %s — run `rime fracture --size 4 3 0.3 "
            "--parts 60 --seed 11 --out %s --name wall` first\n",
            cooked_dir.string().c_str(),
            cooked_dir.string().c_str());
        return std::nullopt;
    }
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(*bytes, err);
    if (!asset) {
        const std::string_view msg = assets::to_string(err);
        std::fprintf(stderr,
                     "10-destructible-wall: wall.rdest is malformed: %.*s\n",
                     static_cast<int>(msg.size()),
                     msg.data());
        return std::nullopt;
    }
    return asset;
}

// ── --headless: the M8 done-when ─────────────────────────────────────────────────────────────────
int run_headless(const std::filesystem::path& cooked, const char* ppm, const char* wav) {
    const auto asset = load_wall(cooked);
    if (!asset) {
        return 1;
    }
    std::printf("10-destructible-wall: cooked wall = %zu parts\n", asset->parts.size());

    // (1) The deterministic destruction self-check (GPU-FREE — the real done-when). A generous tick
    // budget lets the debris pile come fully to rest; the loop breaks the moment it does.
    constexpr int kTicks = 1500;
    // Only the BASE run writes audio: the determinism comparisons below re-run the same sim
    // several ways and would otherwise overwrite the file four times for no gain. The mixer does not
    // touch SimResult, so its presence cannot change what those comparisons compare.
    const SimResult base = run_sim(*asset, {-1, kTicks, wav});
    std::printf("  hit the base seam: %u parts died, %u island(s) detached, %zu debris; "
                "audio played %u times; dust %s; settled=%s\n",
                base.parts_died,
                base.islands_detached,
                base.debris_count,
                base.audio_calls,
                base.dust_fired ? "bloomed" : "silent",
                base.debris_settled ? "yes" : "no");

    // The three fan-out consumers all fired off the ONE event stream.
    const bool listeners_fired = base.parts_died > 0 && base.dust_fired && base.audio_calls > 0;
    // Determinism (the M11 contract): the same scenario twice, and across 1/2/4 physics workers,
    // is bit-identical on BOTH fingerprints.
    const bool deterministic =
        run_sim(*asset, {-1, kTicks}) == base && run_sim(*asset, {1, kTicks}) == base &&
        run_sim(*asset, {2, kTicks}) == base && run_sim(*asset, {4, kTicks}) == base;

    // (1b) The WORK LEDGER (m12.0-perf / ADR-0035 §2a). Counts, never clocks — so this assertion
    // means the same thing on lavapipe in CI as on a 3060, and a change that quietly makes the
    // frame do more work fails here without any hardware in the loop.
    const core::WorkLedger ledger = make_ledger(base);
    const auto violations = wall_budget(kTicks).check(ledger);
    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());
    if (!violations.empty()) {
        std::fprintf(
            stderr, "  BUDGET VIOLATIONS:\n%s", core::WorkBudget::format(violations).c_str());
    }

    const bool sim_ok = base.parts_died > 0 && base.islands_detached >= 1 && base.debris_settled &&
                        listeners_fired && deterministic && violations.empty();
    std::printf(
        "  self-check: died>0=%d island>=1=%d settled=%d listeners=%d deterministic=%d budget=%d\n",
        base.parts_died > 0,
        base.islands_detached >= 1,
        base.debris_settled,
        listeners_fired,
        deterministic,
        violations.empty());

    // (2) The pixel proof — only where a device exists: the intact wall renders lit, and the hit
    // visibly changes the image (parts leave, debris tumbles into new pixels).
    bool pixel_ok = true;
    app::Application app(gpu_config());
    if (!app.device()) {
        std::printf("  (no Vulkan device — skipping the pixel proof; the sim self-check stands)\n");
        pixel_ok = std::getenv("RIME_REQUIRE_VULKAN") == nullptr ? true : false;
        if (std::getenv("RIME_REQUIRE_VULKAN")) {
            std::fprintf(stderr, "  RIME_REQUIRE_VULKAN set but no device\n");
        }
    } else {
        WallScene scene(app, *asset);
        for (int i = 0; i < 2; ++i) {
            app.step(app.fixed_dt()); // a couple of frames to a steady intact pose
        }
        const std::vector<std::uint8_t> intact = scene.readback();
        const bool lit = scene_is_lit(intact);
        if (ppm) {
            write_ppm(ppm, intact);
        }

        hit_base_seam(scene.dw, scene.inst);
        for (int i = 0; i < 40; ++i) {
            scene.step_and_refresh();
            app.step(app.fixed_dt());
        }
        const std::vector<std::uint8_t> broken = scene.readback();
        const std::uint64_t changed = image_diff(intact, broken, 24);
        pixel_ok = lit && changed > 1500; // the dropped slab + tumbling debris repaint the image
        std::printf("  pixel proof: intact lit=%d, break changed %llu px on '%s'\n",
                    lit,
                    static_cast<unsigned long long>(changed),
                    app.device()->adapter().name.c_str());
    }

    const bool ok = sim_ok && pixel_ok;
    std::printf("10-destructible-wall: %s\n",
                ok ? "the wall breaks, the debris flies, the events land — M8 green!"
                   : "FAILED self-check");
    return ok ? 0 : 1;
}

// ── --perf: the hardware report, sim-side (m12.0-perf / ADR-0035 §2b) ───────────────────────────
struct PerfOptions {
    int warmup = 30;       // unmeasured: pipeline/descriptor warmup, the intact wall standing still
    int frames = 600;      // 10 s at 60 Hz — p99 is then the 594th frame rather than max
    int break_frame = 60;  // hit early, so the collapse AND the settle both sit inside the run
    int collapse = 120;    // frames after the hit that count as "the collapse window"
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    const char* out = nullptr;
    const char* baseline = nullptr;
};

app::AppConfig perf_config(std::uint32_t w, std::uint32_t h) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {w, h};
    cfg.tick_hz = 60.0;
    return cfg;
}

int run_perf(const std::filesystem::path& cooked, const PerfOptions& opt) {
    const auto asset = load_wall(cooked);
    if (!asset) {
        return 1;
    }
    app::Application app(perf_config(opt.width, opt.height));
    if (!app.device()) {
        std::fprintf(stderr, "10-destructible-wall --perf: no Vulkan device — a perf run needs "
                             "real hardware, and there is nothing here to measure\n");
        return 1;
    }
    WallScene scene(app, *asset);

    core::PerfReport report;
    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = app.device()->adapter();
    fp.gpu = adapter.name;
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    fp.preset = "destruction-collapse";
    fp.width = opt.width;
    fp.height = opt.height;
    report.set_machine(fp);
    report.set_run(core::RunInfo::detect("10-destructible-wall"));

    std::vector<core::PassTiming> passes;
    app.on_post_submit([&](render::RenderGraph& graph, rhi::CommandBuffer& cmd) {
        passes.clear();
        for (const render::RenderGraph::PassTiming& t : graph.resolve_timings(cmd)) {
            passes.push_back(core::PassTiming{std::string(t.name), t.gpu_ms});
        }
    });

    std::printf("10-destructible-wall --perf: %s (%s %s), %ux%u, %zu parts, measuring %d frames…\n",
                fp.gpu.c_str(),
                adapter.driver_name.c_str(),
                adapter.driver_info.c_str(),
                opt.width,
                opt.height,
                asset->parts.size(),
                opt.frames);

    for (int i = 0; i < opt.warmup; ++i) {
        scene.step_and_refresh();
        app.step(app.fixed_dt());
    }

    core::ZoneTimelines zones(report);
    SimResult peaks;
    for (int i = 0; i < opt.frames; ++i) {
        if (i == opt.break_frame) {
            hit_base_seam(scene.dw, scene.inst);
        }
        const core::Stopwatch frame_watch;

        // The simulation is timed EXPLICITLY rather than through a profile zone, because this
        // sample drives physics outside Application's fixed tick (`sim.tick` therefore measures an
        // empty tick here, which the report shows honestly rather than hiding). Two timelines from
        // one measurement: the whole run, and the collapse window on its own.
        const core::Stopwatch sim_watch;
        scene.step_and_refresh();
        const double sim_ms = sim_watch.elapsed_ms();

        app.step(app.fixed_dt());
        const double frame_ms = frame_watch.elapsed_ms();

        report.observe_frame(static_cast<std::uint64_t>(i), frame_ms, passes);
        report.observe("sim.physics", sim_ms);
        if (i >= opt.break_frame && i < opt.break_frame + opt.collapse) {
            report.observe("frame.collapse", frame_ms);
            report.observe("sim.physics.collapse", sim_ms);
        }

        // Peaks, not end-of-run values — by the time the pile is asleep every load counter has
        // fallen back to ~0, and a ledger sampled only at rest describes a world that never did
        // anything (the same reason --headless takes peaks).
        const physics::WorldStats s = scene.pw.stats();
        peaks.broadphase_pairs_peak = std::max(peaks.broadphase_pairs_peak, s.broadphase_pairs);
        peaks.manifolds_peak = std::max(peaks.manifolds_peak, s.manifolds);
        peaks.contact_points_peak = std::max(peaks.contact_points_peak, s.contact_points);
        peaks.largest_island_peak = std::max(peaks.largest_island_peak, s.largest_island);
    }
    zones.stop();

    const physics::WorldStats final_stats = scene.pw.stats();
    peaks.ticks_run = static_cast<std::uint32_t>(opt.frames);
    peaks.bodies_final = final_stats.body_count;
    peaks.awake_final = final_stats.awake_bodies;
    peaks.parts_died = scene.listeners.parts_died;
    peaks.islands_detached = scene.listeners.islands_detached;
    peaks.debris_count = scene.dw.debris_count();
    peaks.audio_calls = static_cast<std::uint32_t>(scene.listeners.audio.log().size());
    report.set_ledger(make_ledger(peaks));

    core::PerfGate gate;
    gate.at_most("frame", core::PerfStat::P99, 16.6)
        .at_most("frame", core::PerfStat::Max, 33.0)
        .at_most("frame.collapse", core::PerfStat::Max, 33.0)
        // ADR-0035's sim budget: the tick must fit inside 60 Hz on its own, or the simulation dies
        // no matter what the GPU manages. The collapse gets a wider max because a fracture tick
        // legitimately does more work — but only twice as much, not ten times.
        .at_most("sim.physics", core::PerfStat::P99, 6.0)
        .at_most("sim.physics.collapse", core::PerfStat::Max, 12.0)
        .require_samples("frame", 200)
        .require_samples("sim.physics", 200)
        .require_samples("frame.collapse", 30)
        .max_regression(0.10);
    // The vacuity guard, and the reason it is not decoration: every ceiling above is also satisfied
    // by an intact wall that never broke — 0 dead parts, 0 debris, 0 contacts, and a very fast
    // frame indeed.
    gate.work()
        .at_least("destruction.parts_died", 1)
        .at_least("destruction.islands_detached", 1)
        .at_least("destruction.debris_count", 1)
        .at_least("physics.contact_points_peak", 1);

    core::PerfReport baseline;
    const core::PerfReport* baseline_ptr = nullptr;
    if (opt.baseline) {
        std::string error;
        if (core::PerfReport::load_file(opt.baseline, baseline, error)) {
            baseline_ptr = &baseline;
        } else {
            std::printf("  (no usable baseline: %s)\n", error.c_str());
        }
    }
    const core::PerfGate::Result result = gate.check(report, baseline_ptr);

    const auto frame = report.distribution("frame");
    const auto sim = report.distribution("sim.physics");
    const auto sim_collapse = report.distribution("sim.physics.collapse");
    if (!frame || !sim) {
        std::fprintf(stderr,
                     "10-destructible-wall --perf: no frames were measured (--frames %d)\n",
                     opt.frames);
        return 1;
    }
    std::printf("  frame     p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (%zu frames)\n",
                frame->p50_ms,
                frame->p95_ms,
                frame->p99_ms,
                frame->max_ms,
                frame->count);
    std::printf("  sim       p50 %.3f  p95 %.3f  p99 %.3f  max %.3f ms\n",
                sim->p50_ms,
                sim->p95_ms,
                sim->p99_ms,
                sim->max_ms);
    if (sim_collapse) {
        std::printf("  sim@break p50 %.3f  p99 %.3f  max %.3f ms  (%zu frames)\n",
                    sim_collapse->p50_ms,
                    sim_collapse->p99_ms,
                    sim_collapse->max_ms,
                    sim_collapse->count);
    }
    std::printf("  worst frame #%llu at %.2f ms\n",
                static_cast<unsigned long long>(report.worst_frame().index),
                report.worst_frame().ms);
    std::printf("  work ledger: %s\n", make_ledger(peaks).to_json(-1).c_str());

    if (opt.out) {
        FILE* f = std::fopen(opt.out, "wb");
        if (!f) {
            std::fprintf(stderr, "  could not write %s\n", opt.out);
            return 1;
        }
        const std::string json = report.to_json();
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::printf("  wrote %s\n", opt.out);
    }

    std::fflush(stdout); // so the gate's stderr lines land after the numbers they judge
    if (!result.ok()) {
        std::fprintf(stderr, "  PERF GATE:\n%s", core::PerfGate::format(result).c_str());
    } else {
        std::printf("  perf gate: %s", core::PerfGate::format(result).c_str());
    }
    std::printf("10-destructible-wall --perf: %s\n",
                result.ok() ? "within budget" : "FAILED the perf gate");
    return result.ok() ? 0 : 1;
}

// ── --serve: stream the wall coming apart, let a client re-hit it (Track S0) ─────────────────────
int run_serve(const std::filesystem::path& cooked, const std::string& host, std::uint16_t port) {
    const auto asset = load_wall(cooked);
    if (!asset) {
        return 1;
    }
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "10-destructible-wall server: no Vulkan device\n");
        return 1;
    }
    WallScene scene(app, *asset);
    auto streamer = stream::FrameStreamer::create(*app.device(), {kWidth, kHeight});
    auto listener = platform::TcpListener::bind(port, host);
    if (!streamer || !listener) {
        std::fprintf(stderr, "10-destructible-wall server: could not create streamer/listener\n");
        return 1;
    }
    std::printf("10-destructible-wall server: listening on %s:%u — waiting for a client…\n",
                host.c_str(),
                listener->local_port());
    auto accepted = listener->accept();
    if (!accepted) {
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*accepted));
    if (!conn.handshake()) {
        return 1;
    }
    std::printf("10-destructible-wall server: client connected — streaming 720p.\n");

    std::atomic<bool> stop{false};
    std::atomic<bool> rehit{false};
    std::thread input_thread([&] {
        stream::MessageType type{};
        std::vector<std::byte> payload;
        while (!stop.load()) {
            if (!conn.recv_message(type, payload) || type == stream::MessageType::Bye) {
                break;
            }
            if (type == stream::MessageType::Input) {
                stream::InputEvent e;
                if (e.decode(payload) && e.kind == stream::InputEvent::Kind::KeyDown) {
                    rehit.store(true); // any key: hit the wall again
                }
            }
        }
        stop.store(true);
    });

    stream::FrameEncoder encoder;
    std::uint64_t seq = 0;
    const auto period = std::chrono::milliseconds(33);
    auto next = std::chrono::steady_clock::now();
    bool hit_once = false;
    int frame = 0;
    while (!stop.load()) {
        if ((!hit_once && frame == 30) || rehit.exchange(false)) {
            hit_base_seam(scene.dw, scene.inst);
            hit_once = true;
        }
        scene.step_and_refresh();
        app.step(app.fixed_dt());
        const stream::FrameView fv = streamer->capture(app.graph()->physical(scene.last_ldr));
        stream::FrameMessage fm;
        fm.sequence = seq;
        fm.capture_us = platform::Clock::now_ns() / 1000;
        fm.codec = stream::Codec::Jpeg;
        fm.desc = {{kWidth, kHeight}, fv.format};
        if (!encoder.encode(stream::Codec::Jpeg, fm.desc, fv.pixels, fm.data) ||
            !conn.send_frame(fm)) {
            break;
        }
        ++seq;
        ++frame;
        next += period;
        std::this_thread::sleep_until(next);
    }
    stop.store(true);
    input_thread.join();
    std::printf("10-destructible-wall server: done (%llu frames).\n",
                static_cast<unsigned long long>(seq));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode { Headless, Serve, Perf } mode = Mode::Headless;
    std::filesystem::path cooked = default_cooked();
    const char* ppm = nullptr;
    const char* wav = nullptr; // m13.4: write the destruction's own mixdown (see SimConfig::wav)
    std::string host = "0.0.0.0";
    std::uint16_t port = 9100;
    PerfOptions perf;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless") {
            mode = Mode::Headless;
        } else if (a == "--serve") {
            mode = Mode::Serve;
        } else if (a == "--perf") {
            mode = Mode::Perf;
        } else if (a == "--cooked" && i + 1 < argc) {
            cooked = argv[++i];
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
        } else if (a == "--wav" && i + 1 < argc) {
            wav = argv[++i];
        } else if (a == "--ppm" && i + 1 < argc) {
            ppm = argv[++i];
        } else if (a == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
    }

    // Keep the scripted timeline inside a shortened run. A --frames 60 smoke test whose break
    // lands at frame 60 would measure an intact wall and record no collapse window at all — which
    // the gate does catch (that is what the vacuity floors are for), but failing on a mis-scaled
    // script rather than on the engine wastes everyone's afternoon.
    if (mode == Mode::Perf) {
        if (perf.break_frame >= perf.frames) {
            perf.break_frame = perf.frames / 10;
        }
        perf.collapse = std::min(perf.collapse, perf.frames - perf.break_frame);
    }

    if (mode == Mode::Serve) {
        return run_serve(cooked, host, port);
    }
    if (mode == Mode::Perf) {
        return run_perf(cooked, perf);
    }
    return run_headless(cooked, ppm, wav);
}
