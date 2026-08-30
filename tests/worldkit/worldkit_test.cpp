// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m14.1 — the component profile. GPU-free, no assets, no window.
//
// The case that earns this file is the last one: the block's own `.rscene` loads STRICTLY, with
// zero unknown component types. That is the assertion that would have caught `blockkit::SlabRole`
// on the day m13.2c landed, instead of a human discovering it by pointing the editor at the block a
// milestone later. Everything above it is the scaffolding that makes it mean something.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // this TU supplies doctest's main() for the exe
#include <doctest/doctest.h>

#include <cstddef>
#include <string>

#include "rime/blockkit/block.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/physics/components.hpp"
#include "rime/render/components.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/worldkit/profile.hpp"

using namespace rime;

TEST_CASE("m14.1: the profile registers the engine's component set") {
    ecs::World world;
    CHECK(world.registered_component_count() == 0);

    const std::size_t n = worldkit::register_engine_components(world);
    CHECK(n > 0);
    CHECK(n == world.registered_component_count());

    // A spot check across the layers, chosen so that dropping any ONE line from the profile fails
    // here. Naming them individually rather than asserting a count: a count moves whenever any
    // module gains a component and would be edited into agreement without anyone reading it.
    CHECK(world.is_registered<ecs::LocalTransform>());
    CHECK(world.is_registered<physics::RigidBody>());
    CHECK(world.is_registered<render::Camera>());
    CHECK(world.is_registered<gameplay::CharacterState>());

    // The two nobody's own register_*_components covers, because they are derived state and so are
    // deliberately unreflected. The editor host and 99-the-block had each rediscovered these one
    // line at a time; that is precisely the rediscovery this module exists to end.
    CHECK(world.is_registered<ecs::WorldTransform>());
    CHECK(world.is_registered<ecs::RenderTransform>());

    // And NOT the game's. An engine profile that knew about the vision demo's content would be an
    // engine that cannot be used for a second game.
    CHECK_FALSE(world.is_registered<blockkit::SlabRole>());
}

TEST_CASE("m14.1: the profile is idempotent") {
    // Callers layer it with a game's own set and may re-run it after a world reset. Registering a
    // type twice must be a no-op returning the same id, not a second slot — which would change the
    // schema hash and split a network session down the middle.
    ecs::World world;
    const std::size_t first = worldkit::register_engine_components(world);
    const ecs::ComponentId camera = world.component_id<render::Camera>();

    const std::size_t second = worldkit::register_engine_components(world);
    CHECK(second == first);
    CHECK(world.component_id<render::Camera>() == camera);
}

TEST_CASE("m14.1: two worlds built through the profile agree on the schema hash") {
    // The reason the order is fixed rather than merely tidy. `ecs::component_schema_hash` goes into
    // `net::NetDriver::Config`, and two peers whose sets differ refuse to connect — which is
    // exactly the failure 99-the-block hit from the other side. Both peers calling one function
    // cannot disagree, and this is the assertion that says so.
    ecs::World a;
    ecs::World b;
    worldkit::register_engine_components(a);
    worldkit::register_engine_components(b);
    CHECK(ecs::component_schema_hash(a) == ecs::component_schema_hash(b));

    // A world that layers the game's set on top is a DIFFERENT session — same rule, stated so the
    // property is not mistaken for "any two worlds match".
    blockkit::register_blockkit_components(b);
    CHECK(ecs::component_schema_hash(a) != ecs::component_schema_hash(b));
}

TEST_CASE("m14.1: the block's scene loads with zero unknown component types") {
    // THE ONE THAT EARNS THIS FILE.
    //
    // `rime-blockgen` writes the block as a `.rscene`; the editor answered "unknown component type
    // 'rime::blockkit::SlabRole' — is it registered?" and could not open the newest content in the
    // repo. Nothing failed, because nothing looked. This looks.
    //
    // It is a STRICT load on purpose. m14.1 also taught the loader to skip unknown types and count
    // them, which is what lets a tool open a scene from a build it does not match — but a build
    // that has the engine profile AND the game's own module should need none of that tolerance, and
    // if it ever does, that is a drift worth failing on rather than absorbing.
    ecs::World authoring;
    const blockkit::BlockStats stats = blockkit::assemble(authoring, blockkit::BlockParams{});
    const std::string text = scene::save_scene_to_string(authoring);

    ecs::World world;
    worldkit::register_engine_components(world);
    blockkit::register_blockkit_components(world);

    scene::LoadOptions tolerant;
    tolerant.allow_unknown_components = true;
    const scene::LoadReport report = scene::load_scene_from_string(world, text, tolerant);

    REQUIRE_MESSAGE(report.ok, report.error);
    CHECK(report.entities == stats.entities);

    // Zero, and the names printed if not — "3 unknown" sends you hunting, "SlabRole" sends you to
    // the module.
    if (report.skipped_components != 0) {
        std::string names;
        for (const std::string& n : report.unknown_types) {
            names += n + " ";
        }
        FAIL("the block's scene names "
             << report.skipped_components
             << " component(s) this profile does not register: " << names);
    }
    CHECK(report.unknown_types.empty());
}

TEST_CASE("m14.1: without the game's own module, the block is exactly one type short") {
    // The negative control, and it is what stops the case above from passing for the wrong reason.
    // If `register_engine_components` had quietly grown a blockkit line, the strict load would
    // still succeed and nobody would learn that the engine profile had absorbed a game's content.
    //
    // It also demonstrates the tolerance a tool relies on: the scene still loads, every entity is
    // there, and the report names the module that is missing.
    ecs::World authoring;
    (void)blockkit::assemble(authoring, blockkit::BlockParams{});
    const std::string text = scene::save_scene_to_string(authoring);

    ecs::World engine_only;
    worldkit::register_engine_components(engine_only); // no blockkit

    scene::LoadOptions tolerant;
    tolerant.allow_unknown_components = true;
    const scene::LoadReport report = scene::load_scene_from_string(engine_only, text, tolerant);

    REQUIRE_MESSAGE(report.ok, report.error);
    CHECK(report.skipped_components > 0);
    REQUIRE(report.unknown_types.size() == 1);
    CHECK(report.unknown_types[0] == "rime::blockkit::SlabRole");

    // Strict, the same file is refused outright — the behaviour every game keeps by default.
    ecs::World strict_world;
    worldkit::register_engine_components(strict_world);
    CHECK_FALSE(scene::load_scene_from_string(strict_world, text).ok);
}
