// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/audio/audio.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/events.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/physics.hpp"
#include "rime/vfx/dust.hpp"

// M8.4 proofs: the health-transition fan-out (ADR-0029 §7). update() publishes a canonical stream
// of PartDamaged / PartDied / IslandDetached / DebrisSettled events through a core::EventChannel;
// the VFX dust stub, the engine/audio null backend, and gameplay each read that one immutable span.
// Everything here is GPU-free (the dust's actual DRAW is m8.6): we prove the events fire, name the
// right parts/bodies/bounds, arrive in a reproducible order, clear every quiet tick, and drive
// three decoupled listeners — remove one and the others are byte-identical (guardrail 2).
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

// The paper-fixture helpers (mirrors of damage_test.cpp's): a grid of box parts with vertical-only
// bonds so each column is an independent support chain, anchored along the base. Part id = y·nx +
// x.
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

core::Vec3 center(const physics::Aabb& b) {
    return core::Vec3{
        0.5f * (b.min.x + b.max.x), 0.5f * (b.min.y + b.max.y), 0.5f * (b.min.z + b.max.z)};
}

std::size_t debris_with_part(const destruction::DestructionWorld& dw, std::uint32_t part) {
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        const auto parts = dw.debris_parts(i);
        for (const std::uint32_t p : parts) {
            if (p == part) {
                return i;
            }
        }
    }
    return SIZE_MAX;
}

} // namespace

TEST_CASE("M8.4 one break ⇒ exactly one PartDied and one IslandDetached, in that order, with "
          "payloads") {
    // The 2×2 paper fixture: kill anchor 0 ⇒ it dies (PartDied) and its dependant 2 loses support
    // and detaches (IslandDetached). Nothing else. This is the plan's headline "1 + 1" break.
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{0.0f, 0.0f, 7.0f});
    dw.update(pw);

    int damaged = 0, died = 0, detached = 0, settled = 0;
    for (const destruction::DestructionEvent& e : dw.events()) {
        switch (e.kind) {
            case destruction::DestructionEventKind::PartDamaged:
                ++damaged;
                break;
            case destruction::DestructionEventKind::PartDied:
                ++died;
                break;
            case destruction::DestructionEventKind::IslandDetached:
                ++detached;
                break;
            case destruction::DestructionEventKind::DebrisSettled:
                ++settled;
                break;
        }
    }
    CHECK(damaged == 0); // part 0 went straight to dead — no survived-damage event
    CHECK(died == 1);
    CHECK(detached == 1);
    CHECK(settled == 0);

    // Order: the death (stage 2) is published before the detachment (the fracture that follows).
    REQUIRE(dw.events().size() == 2);
    const destruction::DestructionEvent& death = dw.events()[0];
    const destruction::DestructionEvent& island = dw.events()[1];
    CHECK(death.kind == destruction::DestructionEventKind::PartDied);
    CHECK(island.kind == destruction::DestructionEventKind::IslandDetached);

    // PartDied names part 0 of this instance; magnitude is the (positive) health removed; world
    // bounds bracket part 0's COM at (−0.25, 0.25, 0).
    CHECK(death.part == 0);
    CHECK(death.instance == inst);
    CHECK(death.magnitude > 0.0f);
    CHECK(death.world_bounds.min.x <= -0.25f);
    CHECK(death.world_bounds.max.x >= -0.25f);
    CHECK(death.world_bounds.min.y <= 0.25f);
    CHECK(death.world_bounds.max.y >= 0.25f);

    // IslandDetached names the debris body carrying orphan part 2 (a real, live body), no part id,
    // and a kick magnitude (the +Z impulse reached part 2's dependant chain? no — part 2 got no op;
    // it detached from lost support, so its kick is 0). Its bounds bracket part 2's COM (0.25 y).
    CHECK(island.instance == inst);
    CHECK(island.part == destruction::kInvalidPartIndex);
    REQUIRE(island.body.is_valid());
    CHECK(pw.is_alive(island.body));
    const std::size_t d2 = debris_with_part(dw, 2);
    REQUIRE(d2 != SIZE_MAX);
    CHECK(island.body == dw.debris_body(d2));
    CHECK(island.world_bounds.max.y >= 0.75f); // part 2 sits at y = 0.75
}

TEST_CASE("M8.4 PartDamaged for a hit that is survived; PartDied only when the part falls") {
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    // Part 3 (top-right, non-critical), health 1.0. A 0.4 hit — survives, reports PartDamaged.
    dw.apply_damage(inst, {0.25f, 0.75f, 0.0f}, 0.2f, 0.4f, core::Vec3{});
    dw.update(pw);
    REQUIRE(dw.events().size() == 1);
    CHECK(dw.events()[0].kind == destruction::DestructionEventKind::PartDamaged);
    CHECK(dw.events()[0].part == 3);
    CHECK(dw.events()[0].magnitude == doctest::Approx(0.4f));
    CHECK(dw.part_alive(inst, 3));
    CHECK(dw.part_health(inst, 3) == doctest::Approx(0.6f));
    CHECK(dw.debris_count() == 0);

    // A second 0.4 — still standing, still PartDamaged.
    dw.apply_damage(inst, {0.25f, 0.75f, 0.0f}, 0.2f, 0.4f, core::Vec3{});
    dw.update(pw);
    REQUIRE(dw.events().size() == 1);
    CHECK(dw.events()[0].kind == destruction::DestructionEventKind::PartDamaged);
    CHECK(dw.part_health(inst, 3) == doctest::Approx(0.2f));

    // The lethal 0.4: PartDied now, and since part 3 supported nobody there is NO IslandDetached.
    dw.apply_damage(inst, {0.25f, 0.75f, 0.0f}, 0.2f, 0.4f, core::Vec3{});
    dw.update(pw);
    int died = 0, detached = 0, damaged = 0;
    for (const destruction::DestructionEvent& e : dw.events()) {
        if (e.kind == destruction::DestructionEventKind::PartDied)
            ++died;
        if (e.kind == destruction::DestructionEventKind::IslandDetached)
            ++detached;
        if (e.kind == destruction::DestructionEventKind::PartDamaged)
            ++damaged;
    }
    CHECK(damaged == 0);
    CHECK(died == 1);
    CHECK(detached == 0);
    CHECK_FALSE(dw.part_alive(inst, 3));
}

TEST_CASE("M8.4 the event channel is clean on a quiet tick") {
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);
    CHECK_FALSE(dw.events().empty()); // the break published events

    dw.update(pw);              // nothing pending this tick
    CHECK(dw.events().empty()); // …so the channel is empty again

    // A brand-new world with no destructibles publishes an empty frame too.
    destruction::DestructionWorld fresh;
    physics::PhysicsWorld fresh_pw;
    fresh.update(fresh_pw);
    CHECK(fresh.events().empty());
}

TEST_CASE("M8.4 the event stream is deterministic across runs (kinds, parts, bodies)") {
    const assets::DestructibleAsset asset = make_grid(3, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    // Kill column 1's middle (part 4): part 4 dies, its top (part 7) detaches — the same every run.
    const auto run = [&] {
        destruction::DestructionWorld dw;
        physics::PhysicsWorld pw;
        const destruction::PatternId pattern = dw.register_pattern(asset, pw);
        const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
        dw.apply_damage(inst, {0.0f, 0.75f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
        dw.update(pw);
        std::vector<std::uint32_t> fingerprint;
        for (const destruction::DestructionEvent& e : dw.events()) {
            fingerprint.push_back(static_cast<std::uint32_t>(e.kind));
            fingerprint.push_back(e.part);
            fingerprint.push_back(e.body.index);
        }
        return fingerprint;
    };
    const std::vector<std::uint32_t> a = run();
    const std::vector<std::uint32_t> b = run();
    CHECK(a == b);
    CHECK_FALSE(a.empty()); // it actually broke something
}

TEST_CASE("M8.4 DebrisSettled fires when a fallen debris body comes to rest") {
    // A 1×3 column standing 3 m UP, ground far below: kill anchor 0 ⇒ chunk {0} + island {1,2} fall
    // (the no-pop invariant means they are born in place, so they must actually drop to generate a
    // real awake→asleep transition — an on-the-ground column would be born already at rest). Step
    // until the world sleeps and collect the settle events.
    const assets::DestructibleAsset asset = make_grid(1, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    core::Transform up;
    up.translation = {0.0f, 3.0f, 0.0f};
    const destruction::InstanceId inst = dw.spawn(pattern, up, pw);
    add_ground(pw);

    dw.apply_damage(inst, {0.0f, 3.0f + 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);
    const std::size_t debris = dw.debris_count();
    REQUIRE(debris >= 1);

    int settled = 0;
    std::vector<bool> reported(debris, false);
    // Step first, THEN gate on the awake count: right after the break the debris are freshly
    // created and the awake-body stat has not been recomputed yet (it refreshes during step()), so
    // a leading `awake > 0` guard would skip the whole loop before anything falls.
    for (int i = 0; i < 1200; ++i) {
        pw.step(kDt);
        dw.update(pw);
        for (const destruction::DestructionEvent& e : dw.events()) {
            if (e.kind == destruction::DestructionEventKind::DebrisSettled) {
                ++settled;
                CHECK(e.instance == inst);
                // The settled body is one of our debris bodies.
                bool matched = false;
                for (std::size_t d = 0; d < dw.debris_count(); ++d) {
                    if (dw.debris_body(d) == e.body) {
                        matched = true;
                        reported[d] = true;
                    }
                }
                CHECK(matched);
            }
        }
        if (pw.stats().awake_bodies == 0) {
            break; // the world has come to rest
        }
    }
    REQUIRE(pw.stats().awake_bodies == 0); // the world came to rest
    CHECK(settled >= 1);                   // …and at least one settle was reported
}

TEST_CASE("M8.4 fan-out: three listeners observe the stream once each, and VFX is removable") {
    // The whole point of the channel: one break, fanned to three INDEPENDENT listeners — the dust
    // stub, the null audio backend, and a gameplay tally — with none aware of the others. Then the
    // guardrail-2 drill: remove the VFX listener and the other two get byte-identical input.
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);

    struct Gameplay {
        int deaths = 0;
        int detaches = 0;
        float energy = 0.0f;
    };

    const auto fan_out =
        [&](bool with_vfx, vfx::DustField& dust, audio::NullAudioBackend& sound, Gameplay& game) {
            destruction::DestructionWorld dw;
            physics::PhysicsWorld pw;
            const destruction::PatternId pattern = dw.register_pattern(asset, pw);
            const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
            dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{0.0f, 0.0f, 7.0f});
            dw.update(pw);
            for (const destruction::DestructionEvent& e : dw.events()) {
                switch (e.kind) {
                    case destruction::DestructionEventKind::PartDied:
                        sound.play(1, center(e.world_bounds), 1.0f);
                        ++game.deaths;
                        if (with_vfx) {
                            dust.emit_burst(e.world_bounds.min, e.world_bounds.max, 1.0f);
                        }
                        break;
                    case destruction::DestructionEventKind::IslandDetached:
                        sound.play(2, center(e.world_bounds), 0.8f);
                        ++game.detaches;
                        game.energy += e.magnitude;
                        if (with_vfx) {
                            dust.emit_burst(e.world_bounds.min, e.world_bounds.max, 1.0f);
                        }
                        break;
                    default:
                        break;
                }
            }
        };

    vfx::DustField dust1;
    audio::NullAudioBackend sound1;
    Gameplay game1;
    fan_out(true, dust1, sound1, game1);
    // Every listener fired for the one break: 1 death + 1 detach.
    CHECK(game1.deaths == 1);
    CHECK(game1.detaches == 1);
    REQUIRE(sound1.log().size() == 2);
    CHECK(sound1.log()[0].sound == 1); // PartDied delivered before IslandDetached (stream order)
    CHECK(sound1.log()[1].sound == 2);
    CHECK(dust1.count() > 0); // the dust bloomed

    // Removability drill: identical break, VFX listener dropped.
    vfx::DustField dust2;
    audio::NullAudioBackend sound2;
    Gameplay game2;
    fan_out(false, dust2, sound2, game2);
    CHECK(dust2.count() == 0); // VFX did nothing…

    // …and the audio + gameplay listeners are byte-identical to the with-VFX run: no listener's
    // output depends on another being present (guardrail 2, proven not just asserted).
    CHECK(game2.deaths == game1.deaths);
    CHECK(game2.detaches == game1.detaches);
    CHECK(game2.energy == doctest::Approx(game1.energy));
    REQUIRE(sound2.log().size() == sound1.log().size());
    for (std::size_t i = 0; i < sound1.log().size(); ++i) {
        CHECK(sound2.log()[i].sound == sound1.log()[i].sound);
        CHECK(sound2.log()[i].gain == doctest::Approx(sound1.log()[i].gain));
        CHECK(sound2.log()[i].position.x == doctest::Approx(sound1.log()[i].position.x));
    }
}
