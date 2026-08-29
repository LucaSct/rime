// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.2c — the block STANDS UP: 140 cooked destructibles bind on two peers, and the content can
// supply ADR-0035 §1's debris floor. GPU-free, but not asset-free — it consumes the nine `.rdest`
// files the CTest cook fixture produces with `rime fracture`, exactly as a user would.
//
// THE THREE THINGS THIS COVERS THAT THE PURE TEST CANNOT:
//
//   1. **The cook table on disk matches the one in the code.** CMake drives the nine fractures and
//      `blockkit::cook_specs()` describes them; those are two lists that must agree, and nothing
//      but this makes a disagreement visible. A CMake cook with the wrong `--parts` would otherwise
//      produce a block that is quietly smaller than every assertion in blockkit_test claims.
//   2. **`unresolved == 0`.** bind_destructibles hands back "entities whose asset the resolver did
//      not know" precisely so a wall that silently fails to appear is a number instead of an
//      absence. A proof that only checked `bound > 0` cannot see the difference between a block and
//      most of a block.
//   3. **The debris floor is reachable.** ADR-0035 §1 wants >= 400 peak live debris bodies, and the
//      arrangement was chosen before anyone checked one hero building could supply it. This is a
//      CAPABILITY CHECK on the content and the lifecycle config — deliberately NOT the scripted
//      hero beat, which is m13.5's and which pancakes one corner rather than levelling a building.
//
// THE LIFECYCLE CONFIG, AND THE INVARIANT THAT PINS IT. 10-destructible-wall runs at
// max_live_debris = 48; this needs an order of magnitude more. The tempting move is to raise the
// live cap past the visual one so C6 keeps binding at its 512 default — that is wrong.
// destruction/world.hpp makes "the visual budget is DELIBERATELY LARGER than the live one" the
// contract, and inverting it would let a still-simulating debris be visually retired. So both rise:
// 512 live, 1024 visual.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/components.hpp"
#include "rime/scene/scene_format.hpp"

#ifndef RIME_BLOCK_COOKED_DIR
#define RIME_BLOCK_COOKED_DIR "cooked"
#endif

using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

// A peer: its own world, its own physics, its own destruction, its own patterns. Nothing is shared
// — which is the point, since the whole reason `Destructible` names a content id instead of an
// InstanceId is that two peers must be able to arrive at their own local indices independently.
struct Peer {
    ecs::World world;
    physics::PhysicsWorld physics;
    destruction::DestructionWorld destruction;
    std::unordered_map<std::uint64_t, destruction::PatternId> patterns;

    void register_components() {
        ecs::register_transform_components(world);
        blockkit::register_blockkit_components(world);
        destruction::register_destruction_components(world);
        render::register_render_components(world);
    }

    // Register every cook up front. Binding deliberately does not do this itself (bind.hpp: pattern
    // registration is the cold path that pays for hull and compound registration, and hiding it in
    // a per-tick system would put that cost exactly where it is least affordable).
    [[nodiscard]] bool register_patterns(const std::filesystem::path& cooked) {
        for (const blockkit::CookSpec& spec : blockkit::cook_specs()) {
            const auto bytes = platform::read_file(cooked / (std::string(spec.name) + ".rdest"));
            if (!bytes) {
                MESSAGE("missing cook: " << spec.name);
                return false;
            }
            assets::AssetError err{};
            auto asset = assets::read_destructible(*bytes, err);
            if (!asset) {
                MESSAGE("undecodable cook: " << spec.name);
                return false;
            }

            // The cook table in CMake and the one in blockkit must agree. This is the only place
            // that can see them disagree.
            CHECK_MESSAGE(asset->parts.size() == spec.parts,
                          "cook '" << spec.name << "' has " << asset->parts.size()
                                   << " parts, blockkit says " << spec.parts);

            const destruction::PatternId id = destruction.register_pattern(*asset, physics);
            if (!id.is_valid()) {
                // register_pattern rejects the WHOLE pattern on any bad part and hands back one
                // null id, so re-walk the parts here to say WHICH one the physics validator threw
                // out. "a cook was rejected" is not an actionable failure; "part 17 of side_hero
                // has 4 vertices and 1e-7 volume" is.
                physics::PhysicsWorld probe;
                for (std::size_t i = 0; i < asset->parts.size(); ++i) {
                    const assets::DestructiblePart& part = asset->parts[i];
                    const physics::HullDesc hull{
                        part.vertices, part.face_counts, part.face_indices};
                    if (!probe.register_hull(hull).is_valid()) {
                        MESSAGE("cook '" << std::string(spec.name) << "' part " << i
                                         << " rejected: " << part.vertices.size() / 3 << " verts, "
                                         << part.face_counts.size() << " faces, vol "
                                         << part.volume);
                    }
                }
                MESSAGE("register_pattern rejected cook '" << std::string(spec.name) << "' ("
                                                           << asset->parts.size() << " parts)");
                return false;
            }
            patterns[spec.asset] = id;
        }
        return true;
    }

    [[nodiscard]] destruction::PatternResolver resolver() {
        return [this](std::uint64_t asset) {
            const auto it = patterns.find(asset);
            return it == patterns.end() ? destruction::PatternId{} : it->second;
        };
    }
};

// How many debris bodies are still LIVE (not frozen). The roster is append-only, so a frozen debris
// keeps its slot but reads a dead body — the same helper tests/destruction/lifecycle_test uses.
[[nodiscard]] std::size_t live_debris(const destruction::DestructionWorld& dw,
                                      physics::PhysicsWorld& pw) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        if (pw.is_alive(dw.debris_body(i))) {
            ++n;
        }
    }
    return n;
}

physics::BodyId add_street(physics::PhysicsWorld& w, const blockkit::BlockParams& p) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape.type = physics::ShapeType::Box;
    d.shape.half_extents = {p.street_length(), 0.5f, p.street_length()};
    d.position = {p.street_length() * 0.5f, -0.5f, 0.0f};
    return w.create_body(d);
}

// The assembled block, serialized once and loaded into each peer — the same route the demo takes.
[[nodiscard]] std::string block_scene(const blockkit::BlockParams& p) {
    ecs::World authoring;
    (void)blockkit::assemble(authoring, p);
    return scene::save_scene_to_string(authoring);
}

} // namespace

TEST_CASE("blockkit: the block binds on a server and a client, from the same scene file") {
    const std::filesystem::path cooked = RIME_BLOCK_COOKED_DIR;
    const blockkit::BlockParams params;
    const std::string text = block_scene(params);

    Peer server;
    Peer client;
    server.register_components();
    client.register_components();
    REQUIRE(server.register_patterns(cooked));
    REQUIRE(client.register_patterns(cooked));

    REQUIRE(scene::load_scene_from_string(server.world, text).ok);
    REQUIRE(scene::load_scene_from_string(client.world, text).ok);

    // A server binds Local (it owns them, and both damage sources feed them); a client binds
    // Remote, which is what stops its own solver's contact impulses from eroding a wall the server
    // is already eroding for it.
    const destruction::BindStats sb =
        destruction::bind_destructibles(server.world,
                                        server.destruction,
                                        server.physics,
                                        server.resolver(),
                                        destruction::Authority::Local);
    const destruction::BindStats cb =
        destruction::bind_destructibles(client.world,
                                        client.destruction,
                                        client.physics,
                                        client.resolver(),
                                        destruction::Authority::Remote);

    // 128 building slabs + 12 crates. `unresolved` is the load-bearing half: a missing pattern
    // otherwise reads as a quietly smaller block that every count downstream agrees with.
    CHECK(sb.bound == 140);
    CHECK(sb.unresolved == 0);
    CHECK(cb.bound == 140);
    CHECK(cb.unresolved == 0);
    CHECK(server.destruction.instance_count() == 140);
    CHECK(client.destruction.instance_count() == 140);

    SUBCASE("authority is stamped per peer, and the parts add up to the floor") {
        std::size_t server_parts = 0;
        std::size_t local = 0;
        std::size_t remote = 0;
        for (std::size_t i = 0; i < server.destruction.instance_count(); ++i) {
            const auto inst = destruction::InstanceId{static_cast<std::uint32_t>(i)};
            server_parts += server.destruction.instance_part_count(inst);
            if (server.destruction.authority_of(inst) == destruction::Authority::Local) {
                ++local;
            }
        }
        for (std::size_t i = 0; i < client.destruction.instance_count(); ++i) {
            const auto inst = destruction::InstanceId{static_cast<std::uint32_t>(i)};
            if (client.destruction.authority_of(inst) == destruction::Authority::Remote) {
                ++remote;
            }
        }
        CHECK(local == 140);
        CHECK(remote == 140);

        // ADR-0035 §1's part floor, counted from real registered patterns rather than from a table.
        CHECK(server_parts == 2016);
        CHECK(server_parts >= 1500);
    }

    SUBCASE("binding is idempotent — a second pass costs one query and binds nothing") {
        const destruction::BindStats again =
            destruction::bind_destructibles(server.world,
                                            server.destruction,
                                            server.physics,
                                            server.resolver(),
                                            destruction::Authority::Local);
        CHECK(again.bound == 0);
        CHECK(again.unresolved == 0);
        CHECK(server.destruction.instance_count() == 140);
    }

    SUBCASE("the instance index stays private to each peer") {
        // DestructibleInstanceRef is deliberately unreflected, so it can never ride a scene file or
        // a snapshot. Re-serializing a BOUND world must therefore produce a scene with no trace of
        // it — the property m11.4 depends on, checked here rather than assumed.
        const std::string reserialized = scene::save_scene_to_string(server.world);
        CHECK(reserialized.find("DestructibleInstanceRef") == std::string::npos);
        CHECK(reserialized.find("Destructible ") != std::string::npos);
    }
}

TEST_CASE("blockkit: one hero building can supply ADR-0035 §1's peak-debris floor") {
    const std::filesystem::path cooked = RIME_BLOCK_COOKED_DIR;
    const blockkit::BlockParams params;

    Peer peer;
    peer.register_components();
    REQUIRE(peer.register_patterns(cooked));
    REQUIRE(scene::load_scene_from_string(peer.world, block_scene(params)).ok);
    (void)add_street(peer.physics, params);

    destruction::LifecycleConfig life;
    life.enabled = true; // WITHOUT THIS nothing is ever reclaimed and the caps below mean nothing —
                         // the peak would be a measurement of an unbounded run wearing a budget's
                         // name, and `peak_live <= max_live_debris` would pass vacuously.
    life.max_live_debris = 512;
    life.max_visual_debris = 1024;
    peer.destruction.configure_lifecycle(life);

    REQUIRE(destruction::bind_destructibles(peer.world,
                                            peer.destruction,
                                            peer.physics,
                                            peer.resolver(),
                                            destruction::Authority::Local)
                .bound == 140);

    // The south hero. Collect its slabs' instances and where the building stands.
    const std::uint32_t hero = params.hero_south;
    std::vector<destruction::InstanceId> hero_slabs;
    core::Vec3 centre{0.0f, 0.0f, 0.0f};
    peer.world
        .query<blockkit::SlabRole, ecs::LocalTransform, destruction::DestructibleInstanceRef>()
        .for_each([&](blockkit::SlabRole& role,
                      ecs::LocalTransform& tf,
                      destruction::DestructibleInstanceRef& ref) {
            if (role.building != hero || !blockkit::slab_kind::is_destructible(role.kind)) {
                return;
            }
            hero_slabs.push_back(destruction::InstanceId{ref.instance});
            centre.x += tf.value.translation.x;
            centre.y += tf.value.translation.y;
            centre.z += tf.value.translation.z;
        });
    REQUIRE(hero_slabs.size() == params.slabs_per_building());
    const float n = static_cast<float>(hero_slabs.size());
    centre.x /= n;
    centre.y /= n;
    centre.z /= n;

    // Level it. Radius comfortably past the building's diagonal and an amount that still exceeds
    // 1.0 health at the far corner, so every part is struck dead outright and flies off as its own
    // chunk rather than detaching in islands — this is a measurement of what the CONTENT can
    // supply, so it deliberately asks for the maximum rather than a realistic hit.
    for (const destruction::InstanceId inst : hero_slabs) {
        peer.destruction.apply_damage(inst, centre, 20.0f, 40.0f, {0.0f, 0.0f, 0.0f});
    }

    std::size_t peak_live = 0;
    std::size_t peak_visual = 0;
    for (int tick = 0; tick < 120; ++tick) {
        peer.physics.step(kDt);
        peer.destruction.update(peer.physics);
        peak_live = std::max(peak_live, live_debris(peer.destruction, peer.physics));
        peak_visual = std::max(peak_visual, peer.destruction.visual_debris_count());
    }

    MESSAGE("hero collapse: peak live debris " << peak_live << ", peak visible " << peak_visual
                                               << ", roster " << peer.destruction.debris_count());

    // ADR-0035 §1: >= 400 peak live debris bodies. One hero building at 420 parts is what makes
    // that reachable at all — a background building's 180 could not do it even if every part
    // detached, which is exactly why the two heroes are fractured finer.
    CHECK(peak_live >= 400);

    // And the live cap was raised for a reason: at 48 (10-destructible-wall's setting) this would
    // plateau an order of magnitude below the floor.
    CHECK(peak_live <= life.max_live_debris);
    CHECK(life.max_visual_debris > life.max_live_debris);
}
