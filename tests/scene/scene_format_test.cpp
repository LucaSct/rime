// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the `.rscene` scene format (M9 / m9.2, ADR-0031). All GPU-free CPU work, so it runs on
// every CI OS. The claims:
//   (1) a posed, parented world round-trips through text bit-exactly, over EVERY registered type
//   and
//       with the destination registering the types in a DIFFERENT order (hash keying, not id
//       order);
//   (2) a multi-level parent chain remaps scene-local ids → fresh entities correctly;
//   (3) floats round-trip to the exact same bits, and the writer is stable (save∘load∘save ==
//   save); (4) a hand-authored file — partial fields, comments, sloppy whitespace — loads, omitted
//   fields
//       keeping their defaults;
//   (5) malformed / unknown-type / stale-hash / dangling-reference input fails cleanly, never
//   silently; (6) the checked-in samples/07-first-light fixture loads (a real on-disk scene).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <fmt/core.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "rime/core/math/transform.hpp"
#include "rime/core/reflect.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/scene/scene_format.hpp"

using namespace rime;

namespace {

core::Transform trs(float tx, float ty, float tz) {
    return core::Transform{
        core::Vec3{tx, ty, tz}, core::quat_identity(), core::Vec3{1.0f, 1.0f, 1.0f}};
}

// The canonical "first light" scene: a camera, a sun, a ground mesh, and a prop parented to the
// ground — the shape the checked-in fixture holds. mesh ids double as stable identity tags the
// tests match on after a load (entity handles differ between worlds; a MeshRef value does not).
void build_first_light(ecs::World& w) {
    ecs::register_transform_components(w);
    render::register_render_components(w);

    (void)w.spawn_with(ecs::LocalTransform{trs(0.0f, 1.5f, 4.0f)},
                       render::Camera{0.9f, 0.1f, 500.0f, true});
    (void)w.spawn_with(ecs::LocalTransform{trs(0.0f, 10.0f, 0.0f)},
                       render::DirectionalLight{1.0f, 0.95f, 0.9f, 3.0f});
    const ecs::Entity ground = w.spawn_with(
        ecs::LocalTransform{trs(0.0f, 0.0f, 0.0f)}, render::MeshRef{0}, render::MaterialRef{0});
    (void)w.spawn_with(ecs::LocalTransform{trs(1.0f, 0.0f, 0.0f)},
                       render::MeshRef{1},
                       render::MaterialRef{1},
                       ecs::Parent{ground});
}

// Find the (single) entity whose MeshRef.mesh == id, or a null entity.
ecs::Entity entity_with_mesh(ecs::World& w, render::MeshId id) {
    ecs::Entity found = ecs::kNullEntity;
    w.query<render::MeshRef>().for_each([&](ecs::Entity e, render::MeshRef& m) {
        if (m.mesh == id) {
            found = e;
        }
    });
    return found;
}

std::string camera_hash_hex() {
    return fmt::format("0x{:016x}", core::reflect<render::Camera>().type_hash);
}

} // namespace

TEST_CASE("scene: a posed, parented world round-trips through text bit-exactly") {
    ecs::World src;
    build_first_light(src);
    const std::string text = scene::save_scene_to_string(src);
    CHECK(text.find("rime_scene 1") != std::string::npos);
    CHECK(text.find("rime::render::Camera") != std::string::npos);
    CHECK(text.find("value @") != std::string::npos); // the Parent edge, as a scene-local id

    // Destination registers the SAME types in a DIFFERENT order — the round-trip must key on
    // type_hash, not the registration-order ComponentId.
    ecs::World dst;
    render::register_render_components(dst);
    ecs::register_transform_components(dst);
    const scene::LoadReport r = scene::load_scene_from_string(dst, text);
    REQUIRE(r.ok);
    CHECK(r.entities == 4);
    CHECK(dst.entity_count() == 4);

    // Exactly one camera, its lens intact.
    int cameras = 0;
    dst.query<render::Camera>().for_each([&](ecs::Entity, render::Camera& c) {
        ++cameras;
        CHECK(c.fov_y == 0.9f);
        CHECK(c.z_near == 0.1f);
        CHECK(c.z_far == 500.0f);
        CHECK(c.active == true);
    });
    CHECK(cameras == 1);

    // The sun's colour/intensity survived.
    int lights = 0;
    dst.query<render::DirectionalLight>().for_each([&](ecs::Entity, render::DirectionalLight& l) {
        ++lights;
        CHECK(l.color_g == 0.95f);
        CHECK(l.intensity == 3.0f);
    });
    CHECK(lights == 1);

    // The prop (mesh 1) is parented; its parent remaps to the ground (mesh 0), and the ground's
    // authored transform came back exactly.
    const ecs::Entity prop = entity_with_mesh(dst, 1);
    REQUIRE(prop != ecs::kNullEntity);
    const ecs::Parent* parent = dst.get<ecs::Parent>(prop);
    REQUIRE(parent != nullptr);
    CHECK(parent->value == entity_with_mesh(dst, 0));
    const ecs::LocalTransform* ground_lt = dst.get<ecs::LocalTransform>(parent->value);
    REQUIRE(ground_lt != nullptr);
    CHECK(ground_lt->value.translation.x == 0.0f);
    // The prop's own placement.
    const ecs::LocalTransform* prop_lt = dst.get<ecs::LocalTransform>(prop);
    REQUIRE(prop_lt != nullptr);
    CHECK(prop_lt->value.translation.x == 1.0f);
}

TEST_CASE("scene: a multi-level parent chain remaps correctly") {
    // grandparent(10) <- parent(11) <- child(12), each with a distinct translation.
    ecs::World src;
    ecs::register_transform_components(src);
    render::register_render_components(src);
    const ecs::Entity gp =
        src.spawn_with(ecs::LocalTransform{trs(1.0f, 0.0f, 0.0f)}, render::MeshRef{10});
    const ecs::Entity p = src.spawn_with(
        ecs::LocalTransform{trs(2.0f, 0.0f, 0.0f)}, render::MeshRef{11}, ecs::Parent{gp});
    (void)src.spawn_with(
        ecs::LocalTransform{trs(3.0f, 0.0f, 0.0f)}, render::MeshRef{12}, ecs::Parent{p});

    const std::string text = scene::save_scene_to_string(src);
    ecs::World dst;
    ecs::register_transform_components(dst);
    render::register_render_components(dst);
    REQUIRE(scene::load_scene_from_string(dst, text).ok);

    const ecs::Entity child = entity_with_mesh(dst, 12);
    const ecs::Entity parent = entity_with_mesh(dst, 11);
    const ecs::Entity grand = entity_with_mesh(dst, 10);
    REQUIRE(child != ecs::kNullEntity);
    REQUIRE(parent != ecs::kNullEntity);
    REQUIRE(grand != ecs::kNullEntity);

    // Walk the chain by the remapped handles.
    REQUIRE(dst.get<ecs::Parent>(child) != nullptr);
    CHECK(dst.get<ecs::Parent>(child)->value == parent);
    REQUIRE(dst.get<ecs::Parent>(parent) != nullptr);
    CHECK(dst.get<ecs::Parent>(parent)->value == grand);
    CHECK(dst.get<ecs::Parent>(grand) == nullptr); // the root has no Parent
    CHECK(dst.get<ecs::LocalTransform>(grand)->value.translation.x == 1.0f);
}

TEST_CASE("scene: floats round-trip to the exact same bits and the writer is stable") {
    // Nasty float values that only survive if the writer emits shortest-round-trip decimals and the
    // reader parses them back to the nearest float (from_chars).
    ecs::World src;
    ecs::register_transform_components(src);
    render::register_render_components(src);
    (void)src.spawn_with(render::Camera{0.8726646f, 0.001f, 12345.678f, true});
    (void)src.spawn_with(render::PointLight{0.1f, 0.2f, 0.3f, 3.1415927f, 9.9999f});

    const std::string t1 = scene::save_scene_to_string(src);
    ecs::World dst;
    ecs::register_transform_components(dst);
    render::register_render_components(dst);
    REQUIRE(scene::load_scene_from_string(dst, t1).ok);

    dst.query<render::Camera>().for_each([&](ecs::Entity, render::Camera& c) {
        CHECK(c.fov_y == 0.8726646f); // exact bit equality
        CHECK(c.z_near == 0.001f);
        CHECK(c.z_far == 12345.678f);
    });
    dst.query<render::PointLight>().for_each([&](ecs::Entity, render::PointLight& l) {
        CHECK(l.intensity == 3.1415927f);
        CHECK(l.radius == 9.9999f);
    });

    // Stability: re-saving the reconstructed world yields byte-identical text (this world has one
    // archetype per entity in a fixed order, so emit order is deterministic too).
    const std::string t2 = scene::save_scene_to_string(dst);
    CHECK(t1 == t2);
}

TEST_CASE("scene: a hand-authored file loads, omitted fields keeping defaults") {
    // Deliberately messy: a comment, blank lines, extra spaces, only two of Camera's four fields.
    const std::string text = fmt::format(R"(# a hand-written scene
rime_scene 1

entity   0    {{
   rime::render::Camera {} {{
      fov_y   1.25
      active  false
   }}
}}
)",
                                         camera_hash_hex());

    ecs::World w;
    render::register_render_components(w);
    const scene::LoadReport r = scene::load_scene_from_string(w, text);
    REQUIRE(r.ok);
    CHECK(r.entities == 1);

    int n = 0;
    w.query<render::Camera>().for_each([&](ecs::Entity, render::Camera& c) {
        ++n;
        CHECK(c.fov_y == 1.25f);   // authored
        CHECK(c.active == false);  // authored
        CHECK(c.z_near == 0.1f);   // omitted → the C++ default
        CHECK(c.z_far == 1000.0f); // omitted → the C++ default
    });
    CHECK(n == 1);
}

TEST_CASE("scene: malformed input fails cleanly with a helpful message") {
    ecs::World w;
    ecs::register_transform_components(w);
    render::register_render_components(w);
    const std::string cam = camera_hash_hex();

    SUBCASE("not a scene file") {
        CHECK_FALSE(scene::load_scene_from_string(w, "hello world").ok);
    }
    SUBCASE("unterminated entity") {
        CHECK_FALSE(scene::load_scene_from_string(w, "rime_scene 1\nentity 0 {").ok);
    }
    SUBCASE("unknown component type") {
        const scene::LoadReport r =
            scene::load_scene_from_string(w, "rime_scene 1\nentity 0 { some::Bogus 0x1234 { } }");
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("unknown component type") != std::string::npos);
    }
    SUBCASE("stale hash of a KNOWN type is reported as schema drift") {
        const scene::LoadReport r = scene::load_scene_from_string(
            w, "rime_scene 1\nentity 0 { rime::render::Camera 0xdeadbeefdeadbeef { } }");
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("schema drift") != std::string::npos);
    }
    SUBCASE("unknown field") {
        const scene::LoadReport r = scene::load_scene_from_string(
            w,
            fmt::format("rime_scene 1\nentity 0 {{ rime::render::Camera {} {{ nope 1 }} }}", cam));
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("unknown field") != std::string::npos);
    }
    SUBCASE("bad value") {
        const scene::LoadReport r = scene::load_scene_from_string(
            w,
            fmt::format("rime_scene 1\nentity 0 {{ rime::render::Camera {} {{ fov_y abc }} }}",
                        cam));
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("bad value") != std::string::npos);
    }
    SUBCASE("dangling entity reference") {
        const std::string parent_hash =
            fmt::format("0x{:016x}", core::reflect<ecs::Parent>().type_hash);
        const scene::LoadReport r = scene::load_scene_from_string(
            w,
            fmt::format("rime_scene 1\nentity 0 {{ rime::ecs::Parent {} {{ value @99 }} }}",
                        parent_hash));
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("dangling entity reference") != std::string::npos);
    }
}

TEST_CASE("scene: the checked-in first-light fixture loads") {
    const std::filesystem::path fixture =
        std::filesystem::path(RIME_SCENE_FIXTURE_DIR) / "first_light.rscene";

    // Bootstrap hook: `RIME_SCENE_WRITE_FIXTURE=1 ctest -R scene` (re)generates the committed
    // fixture from the canonical world — run once by a human after a deliberate schema change, then
    // commit the file. Normal runs (and CI) only read it.
    if (std::getenv("RIME_SCENE_WRITE_FIXTURE") != nullptr) {
        ecs::World gen;
        build_first_light(gen);
        REQUIRE(scene::save_scene_file(gen, fixture));
        MESSAGE("wrote fixture: " << fixture.string());
    }

    ecs::World w;
    ecs::register_transform_components(w);
    render::register_render_components(w);
    const scene::LoadReport r = scene::load_scene_file(w, fixture);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.entities == 4);
    CHECK(w.query<render::Camera>().count() == 1);
    // The parented prop survived as a live hierarchy edge.
    int parented = 0;
    w.query<ecs::Parent>().for_each([&](ecs::Entity, ecs::Parent& p) {
        if (p.value != ecs::kNullEntity && w.is_alive(p.value)) {
            ++parented;
        }
    });
    CHECK(parented == 1);
}
