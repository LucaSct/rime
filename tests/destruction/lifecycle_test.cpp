// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/physics.hpp"

// M8.5 proofs: the debris lifetime & budget half (ADR-0029 §8) — the lifecycle that keeps the
// physics stores BOUNDED under refracture. The mechanisms, each with its own test: settled debris
// freeze after a linger; a live-body cap evicts settled debris (settled-first, deterministic); the
// fracture body swap frees the old remainder compound (never the shared pattern one); and — the
// load-bearing property — all of it stays bit-reproducible across runs and physics worker counts,
// because it lives in update()'s sequential tail (the M11 replay contract). Everything is GPU-free
// (cooked geometry in, physics bodies out), so the suite runs on every CI OS + ASan/UBSan + TSan.
//
// Off by default: a DestructionWorld reclaims nothing until configure_lifecycle turns it on, so the
// M8.2–8.4 tests are untouched. The first case here pins that (configuring "off" == never touching
// it, byte for byte).
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::vector<std::byte> read_fixture(const std::string& name) {
    const std::string path = std::string(RIME_DESTRUCTION_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open fixture: " << path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

assets::DestructibleAsset load_asset(const std::string& name) {
    const std::vector<std::byte> file = read_fixture(name);
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    REQUIRE_MESSAGE(asset.has_value(), name << " failed to decode: " << assets::to_string(err));
    return std::move(*asset);
}

// A box-hull part (the cook's output shape, hand-made): 8 COM-centred corners, six outward-wound
// quads, exact volume/bounds. Mirrors the M8.3 suite's helper.
assets::DestructiblePart make_box_part(core::Vec3 half, core::Vec3 com) {
    assets::DestructiblePart part;
    part.com = com;
    part.aabb_min = com - half;
    part.aabb_max = com + half;
    part.volume = 8.0f * half.x * half.y * half.z;
    for (std::uint32_t i = 0; i < 8; ++i) {
        part.vertices.push_back({(i & 1u) != 0 ? half.x : -half.x,
                                 (i & 2u) != 0 ? half.y : -half.y,
                                 (i & 4u) != 0 ? half.z : -half.z});
    }
    part.face_counts = {4, 4, 4, 4, 4, 4};
    part.face_indices = {0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    return part;
}

// An nx × ny grid of box parts on y = 0, VERTICAL bonds only (each column its own support chain),
// anchored along the bottom row. Part ids row-major from the bottom: p = y·nx + x.
assets::DestructibleAsset
make_grid(int nx, int ny, core::Vec3 half, float damage_threshold, float damage_scale) {
    assets::DestructibleAsset asset;
    asset.half_extents = {static_cast<float>(nx) * half.x, static_cast<float>(ny) * half.y, half.z};
    asset.damage_threshold = damage_threshold;
    asset.damage_scale = damage_scale;
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const core::Vec3 com{(static_cast<float>(x) - static_cast<float>(nx - 1) * 0.5f) *
                                     2.0f * half.x,
                                 (static_cast<float>(y) + 0.5f) * 2.0f * half.y,
                                 0.0f};
            asset.parts.push_back(make_box_part(half, com));
        }
    }
    for (int y = 0; y + 1 < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(y * nx + x);
            asset.bonds.push_back({a, a + static_cast<std::uint32_t>(nx), 1.0f});
        }
    }
    for (int x = 0; x < nx; ++x) {
        asset.anchors.push_back(static_cast<std::uint32_t>(x));
    }
    return asset;
}

physics::BodyId add_ground(physics::PhysicsWorld& w, float top_y = 0.0f) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape.type = physics::ShapeType::Box;
    d.shape.half_extents = {50.0f, 0.5f, 50.0f};
    d.position = {0.0f, top_y - 0.5f, 0.0f};
    return w.create_body(d);
}

// How many debris bodies are still LIVE (not frozen) — the roster is append-only, so a frozen
// debris keeps its slot but reads a dead body.
std::size_t live_debris(const destruction::DestructionWorld& dw, physics::PhysicsWorld& pw) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        if (pw.is_alive(dw.debris_body(i))) {
            ++n;
        }
    }
    return n;
}

// Step until the whole world is asleep (debris come to rest) or a generous cap is hit, running the
// destruction tail each tick so the lifecycle acts as bodies settle. Step FIRST, then gate: debris
// are born AT REST (the no-pop invariant), so awake_bodies reads a stale 0 until the first step
// integrates gravity into them (the M8.4 gotcha). A gate-first loop would see that 0 and never
// step.
void settle(destruction::DestructionWorld& dw, physics::PhysicsWorld& pw, int max_steps = 2000) {
    for (int i = 0; i < max_steps; ++i) {
        pw.step(kDt);
        dw.update(pw);
        if (pw.stats().awake_bodies == 0) {
            break;
        }
    }
}

} // namespace

TEST_CASE("M8.5 lifecycle OFF ⇒ byte-identical to unconfigured, and nothing is reclaimed") {
    // The gate that protects every earlier brick: a default (disabled) config must behave exactly
    // as a DestructionWorld that was never configured at all — same fingerprints, same rosters,
    // and every debris body still alive after it settles (nothing frozen, the store append-only).
    const assets::DestructibleAsset asset = make_grid(3, 4, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    const auto run = [&](bool configure_disabled) {
        destruction::DestructionWorld dw;
        physics::PhysicsWorld pw;
        if (configure_disabled) {
            dw.configure_lifecycle(destruction::LifecycleConfig{}); // enabled defaults to false
        }
        const destruction::PatternId pattern = dw.register_pattern(asset, pw);
        const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
        add_ground(pw, 0.0f);
        // Kill the whole anchored base row (parts 0,1,2): each column above loses support and
        // detaches; the killed anchors fly off too — a spray of debris that then settles.
        for (int x = 0; x < 3; ++x) {
            dw.apply_damage(inst,
                            dw.part_placement(inst, static_cast<std::uint32_t>(x)).translation,
                            0.15f,
                            10.0f,
                            core::Vec3{});
        }
        dw.update(pw);
        settle(dw, pw);
        struct R {
            std::uint64_t sh, wh;
            std::size_t total, live;
            bool operator==(const R&) const = default;
        };
        return R{dw.state_hash(), pw.world_hash(), dw.debris_count(), live_debris(dw, pw)};
    };
    const auto never = run(false);
    const auto off = run(true);
    CHECK(never == off);              // configuring "disabled" changes nothing
    CHECK(never.total > 0);           // debris really were produced
    CHECK(never.live == never.total); // …and none were reclaimed (every body still alive)
}

TEST_CASE(
    "M8.5 freeze: a settled debris is reclaimed after its linger; the shared pattern survives") {
    // A 1×3 column, anchor killed ⇒ {0} leaves as a k=1 hull chunk (shared pattern hull) and the
    // orphaned {1,2} as a k>1 runtime compound. Both fall, settle, and — after freeze_delay_ticks —
    // freeze: body destroyed, the k>1 compound unregistered, the roster record kept. The shared
    // pattern geometry (the k=1 hull, the intact compound) must be untouched: a fresh instance
    // stands.
    const assets::DestructibleAsset asset = make_grid(1, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    destruction::LifecycleConfig cfg;
    cfg.enabled = true;
    cfg.freeze_delay_ticks = 8;
    cfg.max_live_debris = 1000; // no cap pressure — isolate the linger→freeze path
    dw.configure_lifecycle(cfg);
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    add_ground(pw, 0.0f);

    dw.apply_damage(inst, dw.part_placement(inst, 0).translation, 0.1f, 10.0f, core::Vec3{});
    dw.update(pw);
    REQUIRE(dw.debris_count() == 2);
    const std::size_t roster = dw.debris_count();
    const std::size_t body_count_with_debris = pw.body_count(); // stats() is stale until a step()

    settle(dw, pw);
    REQUIRE(pw.stats().awake_bodies == 0);
    // Step past the linger so both settled debris freeze.
    for (std::uint32_t i = 0; i < cfg.freeze_delay_ticks + 3; ++i) {
        pw.step(kDt);
        dw.update(pw);
    }

    CHECK(dw.debris_count() == roster);          // append-only: the roster never shrinks
    CHECK_FALSE(pw.is_alive(dw.debris_body(0))); // both bodies reclaimed
    CHECK_FALSE(pw.is_alive(dw.debris_body(1)));
    CHECK(live_debris(dw, pw) == 0);
    CHECK(pw.body_count() < body_count_with_debris); // bodies really left the world (stats() lags)

    // The shared pattern outlived both freezes (the k=1 hull and the intact compound were never
    // touched — only the k>1 debris's OWN compound was unregistered): a new instance stands whole.
    const destruction::InstanceId inst2 = dw.spawn(pattern, core::Transform{}, pw);
    CHECK(inst2.is_valid());
    CHECK(pw.is_alive(dw.body_of(inst2)));
    CHECK(dw.instance_part_count(inst2) == 3);
}

TEST_CASE("M8.5 body swap: repeated fracture frees the old remainder compounds (bounded store)") {
    // A tall 1×N column: killing the current TOP part each update keeps the base standing and swaps
    // the body every tick, so from the 2nd swap on each swap frees the PRIOR remainder compound. A
    // probe compound registered at the end lands on the lowest free slot — shallow when those
    // remainders were freed and reused, deep when they leaked. (Observable because the physics
    // shape store hands back a freed slot on the next register — M8.5's whole point.)
    constexpr int kN = 8;
    const assets::DestructibleAsset asset = make_grid(1, kN, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    const auto probe_slot_after_sequential_kills = [&](bool enabled) {
        destruction::DestructionWorld dw;
        physics::PhysicsWorld pw;
        destruction::LifecycleConfig cfg;
        cfg.enabled = enabled;
        cfg.freeze_delay_ticks = 1000000; // isolate the swap-free path (no debris freezing)
        cfg.max_live_debris = 1000000;
        dw.configure_lifecycle(cfg);
        const destruction::PatternId pattern = dw.register_pattern(asset, pw);
        const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
        // Kill parts kN-1 … 1 (leave anchor 0 standing): each kill swaps the body to the shrinking
        // remainder. No step needed — the swap is pure op-path work.
        for (int p = kN - 1; p >= 1; --p) {
            dw.apply_damage(inst,
                            dw.part_placement(inst, static_cast<std::uint32_t>(p)).translation,
                            0.1f,
                            10.0f,
                            core::Vec3{});
            dw.update(pw);
        }
        // The probe: a lone box-primitive compound (no hull needed). Its slot index tells the store
        // depth — reused-low if the remainders were freed, ever-higher if they leaked.
        physics::ShapeDesc box;
        box.type = physics::ShapeType::Box;
        box.half_extents = {0.1f, 0.1f, 0.1f};
        std::vector<physics::CompoundChildDesc> children{
            physics::CompoundChildDesc{box, core::Vec3{0.0f, 0.0f, 0.0f}, core::quat_identity()}};
        const physics::CompoundId probe = pw.register_compound(physics::CompoundDesc{children});
        REQUIRE(probe.is_valid());
        return probe.index;
    };
    const std::uint32_t slot_on = probe_slot_after_sequential_kills(true);
    const std::uint32_t slot_off = probe_slot_after_sequential_kills(false);
    CHECK(slot_on < slot_off);                             // freeing kept the store shallow
    CHECK(slot_on <= 3);                                   // only pattern + current remainder
    CHECK(slot_off >= static_cast<std::uint32_t>(kN - 2)); // off: a fresh slot per swap
}

TEST_CASE("M8.5 cap: the live debris population is held under max_live_debris (settled-first, "
          "deterministic)") {
    // A wide, shallow wall (10 columns × 2) with its whole anchored base killed: 20 debris — the 10
    // killed anchors and the 10 orphaned tops — spread across the floor and settle cleanly. The cap
    // then bites: it evicts settled debris down to the cap, largest-and-oldest first, and picks the
    // very same victims on a re-run (a total-order score — no wall clock, no iteration order
    // leaks).
    //
    // The cap is applied AFTER the debris settle, not during: freezing a debris wakes whatever
    // rested on it, so capping mid-settle would churn a pile indefinitely. Letting everything fall
    // under a loose cap, THEN tightening it, evicts in one clean pass (no step in between ⇒ nothing
    // re-wakes mid-eviction).
    const assets::DestructibleAsset asset = make_grid(10, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    constexpr std::uint32_t kCap = 8;
    const auto run = [&]() {
        destruction::DestructionWorld dw;
        physics::PhysicsWorld pw;
        destruction::LifecycleConfig cfg;
        cfg.enabled = true;
        cfg.freeze_delay_ticks = 1000000; // linger off — isolate the cap
        cfg.max_live_debris = 1000000;    // …and let everything fall + settle uncapped first
        dw.configure_lifecycle(cfg);
        const destruction::PatternId pattern = dw.register_pattern(asset, pw);
        const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
        add_ground(pw, 0.0f);
        for (int x = 0; x < 10; ++x) { // kill the whole anchored base row
            dw.apply_damage(inst,
                            dw.part_placement(inst, static_cast<std::uint32_t>(x)).translation,
                            0.1f,
                            10.0f,
                            core::Vec3{});
        }
        dw.update(pw);
        const std::size_t created = dw.debris_count();
        settle(dw, pw, 4000);
        REQUIRE(pw.stats().awake_bodies == 0); // all at rest before the cap bites

        cfg.max_live_debris = kCap; // tighten — the next update evicts to the cap in one clean pass
        dw.configure_lifecycle(cfg);
        dw.update(pw);

        std::vector<std::uint8_t> frozen;
        for (std::size_t i = 0; i < dw.debris_count(); ++i) {
            frozen.push_back(pw.is_alive(dw.debris_body(i)) ? std::uint8_t{0} : std::uint8_t{1});
        }
        struct R {
            std::size_t created, live;
            std::vector<std::uint8_t> frozen;
            bool operator==(const R&) const = default;
        };
        return R{created, live_debris(dw, pw), std::move(frozen)};
    };
    const auto a = run();
    CHECK(a.created > kCap); // far more debris were created than the cap allows to live
    CHECK(a.live == kCap);   // …and the cap held it to exactly the cap once settled
    const auto b = run();
    CHECK(a == b); // the eviction picked exactly the same victims — deterministic
}

namespace {

// One soak run: three walls blasted in rotation over a long tick budget, with BOTH lifecycle
// mechanisms on (linger + cap). Everything observed is returned for exact comparison — the M11
// witness, extended to the lifecycle.
struct SoakRun {
    std::uint64_t state_hash = 0;
    std::uint64_t world_hash = 0;
    std::size_t debris_total = 0;
    std::size_t final_live = 0;
    bool operator==(const SoakRun&) const = default;
};

SoakRun run_soak(const assets::DestructibleAsset& asset, int workers, std::uint32_t cap) {
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    destruction::LifecycleConfig cfg;
    cfg.enabled = true;
    cfg.freeze_delay_ticks = 20;
    cfg.max_live_debris = cap;
    dw.configure_lifecycle(cfg);
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    std::vector<destruction::InstanceId> insts;
    std::vector<float> origin_x;
    for (int k = 0; k < 3; ++k) {
        core::Transform t;
        t.translation = {static_cast<float>(k) * 6.0f - 6.0f, 0.0f, 0.0f};
        insts.push_back(dw.spawn(pattern, t, pw));
        origin_x.push_back(t.translation.x);
    }
    add_ground(pw, -1.5f);

    std::unique_ptr<core::JobSystem> js;
    if (workers >= 0) {
        js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
        pw.set_job_system(js.get());
    }

    for (int tick = 0; tick < 360; ++tick) {
        if (tick % 12 == 0) {
            const int which = (tick / 12) % 3;
            const float band = -0.6f + 0.4f * static_cast<float>((tick / 12) % 4);
            for (float x = -1.8f; x <= 1.8f; x += 0.6f) {
                dw.apply_damage(
                    insts[static_cast<std::size_t>(which)],
                    core::Vec3{origin_x[static_cast<std::size_t>(which)] + x, band, 0.0f},
                    0.5f,
                    6.0f,
                    core::Vec3{0.0f, 0.0f, 20.0f});
            }
        }
        pw.step(kDt);
        dw.update(pw);
    }
    settle(dw, pw);
    dw.update(pw);

    SoakRun out;
    out.state_hash = dw.state_hash();
    out.world_hash = pw.world_hash();
    out.debris_total = dw.debris_count();
    out.final_live = live_debris(dw, pw);
    return out;
}

} // namespace

TEST_CASE("M8.5 soak: continuous refracture under a cap stays bounded and bit-reproducible across "
          "worker counts") {
    // The headline lifetime proof: a sustained stream of fractures (three walls carved in rotation)
    // with both the linger and the cap active. Far more debris are created than could ever coexist,
    // yet the settled population stays under the cap — and the whole run is bit-identical on a
    // re-run and for 1/2/4 physics workers, because the lifecycle lives in the sequential tail.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    constexpr std::uint32_t kCap = 12;

    const SoakRun base = run_soak(asset, -1, kCap);
    CHECK(base.debris_total > kCap); // many, many debris passed through
    CHECK(base.final_live <= kCap);  // the store settled to within the cap (linger + cap)

    CHECK(run_soak(asset, -1, kCap) == base); // same run twice — identical to the bit
    CHECK(run_soak(asset, 1, kCap) == base);  // and across physics worker counts
    CHECK(run_soak(asset, 2, kCap) == base);
    CHECK(run_soak(asset, 4, kCap) == base);
}
