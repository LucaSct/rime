// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// A render-graph pass name is an IDENTITY, not a label (m17.3, ADR-0041 Ruling 2). Everything
// downstream keys on it: `resolve_timings` reports it, `PerfReport` folds same-named timings into
// one distribution, and a committed `docs/perf/` report writes one JSON object key per pass. So a
// duplicate name is not cosmetic — it merges unrelated work into one statistic and produces an
// artifact whose every ordinary reader silently drops all but one of the collisions.
//
// That is not hypothetical. `docs/perf/2026-08-30-99-the-block-nvidia-geforce-rtx-3060.json`
// contains FOUR `"depth-prepass"` keys inside one object, because a CSM declares the depth pre-pass
// once per cascade. The committed file under-reports its own worst frame by 0.32 ms, and NO row in
// it is named for shadow work at all — in the milestone that has to decide what shadows cost.
//
// Two proofs, and neither is a golden image:
//
//  (1) The graph hands out unique names. Declare one name three times; get three passes back that
//      can be told apart, and told apart from a caller who declared the collided name by hand.
//
//  (2) The names say what the work IS. A frame with a sun and two shadowing spots declares
//      `depth-prepass` exactly once, `csm-cascade-N` per cascade and `spot-shadow-N` per rendered
//      slot — and NOTHING in that frame needs the uniquifier, which is the stronger claim: the
//      naming is deliberate upstream rather than papered over downstream.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::vulkan_required;

namespace {

// Every declared pass name in the frame, in declaration order — culled ones included, because a
// name collision is a property of what was DECLARED and survives whatever the compiler prunes.
[[nodiscard]] std::vector<std::string> declared_names(const RenderGraph& graph) {
    std::vector<std::string> out;
    out.reserve(graph.pass_count());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(graph.pass_count()); ++i)
        out.push_back(graph.pass_name(i));
    return out;
}

[[nodiscard]] std::size_t count_of(const std::vector<std::string>& names, std::string_view want) {
    return static_cast<std::size_t>(std::count_if(
        names.begin(), names.end(), [want](const std::string& n) { return n == want; }));
}

[[nodiscard]] bool all_unique(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    return std::adjacent_find(names.begin(), names.end()) == names.end();
}

} // namespace

TEST_CASE("pass identity: one name declared three times is three passes, not one (m17.3)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping the pass-identity proof");
        return;
    }

    RenderGraph graph(*device);
    graph.reset();

    constexpr std::uint32_t kSize = 32;

    // Three declarations of one name — the shape a CSM had before m17.3 — plus a caller who
    // declares the *collided* name by hand. That last one is the case a naive "count the prefix
    // matches" uniquifier gets wrong: it would hand out `twin#1` twice.
    //
    // Each pass gets its OWN exported target. That is not incidental: passes writing one shared
    // texture nobody reads are dead, the compiler culls them, and `resolve_timings` then reports
    // nothing — so the timing subcase below would pass by measuring an empty list. It did, on the
    // first run of this file.
    const char* const kDeclared[] = {"twin", "twin#1", "twin", "twin", "alone"};
    std::vector<RGTexture> targets;
    std::vector<std::vector<RGColorAttachment>> atts;
    targets.reserve(std::size(kDeclared));
    atts.reserve(std::size(kDeclared));
    for (std::size_t i = 0; i < std::size(kDeclared); ++i) {
        const std::string name = "identity-target-" + std::to_string(i);
        targets.push_back(graph.create_texture({{kSize, kSize}, rhi::Format::RGBA8Unorm, name}));
        atts.push_back({{targets.back(), rhi::LoadOp::Clear, rhi::StoreOp::Store, {}}});
        graph.add_raster_pass(kDeclared[i], {.colors = atts.back()}, [](rhi::CommandBuffer&) {});
        graph.export_texture(targets.back());
    }

    const std::vector<std::string> names = declared_names(graph);
    REQUIRE(names.size() == 5);

    // The property, stated as the property: no two passes answer to the same name. Before m17.3
    // this failed with three passes all called "twin".
    CHECK(all_unique(names));

    // And the disambiguation is legible rather than arbitrary — the first keeps the name it asked
    // for, so an existing reader of `docs/perf/` sees no churn on a pass that never collided.
    CHECK(names[0] == "twin");
    CHECK(names[1] == "twin#1");
    CHECK(names[4] == "alone");
    // The two later "twin"s could not take `twin#1` (already claimed by name), so they skipped past
    // it. Which exact suffixes they got matters less than that they are distinct and stable.
    CHECK(names[2] != names[3]);
    CHECK(names[2].rfind("twin#", 0) == 0);
    CHECK(names[3].rfind("twin#", 0) == 0);

    SUBCASE("resolve_timings reports the same distinct names, so a report cannot fold them") {
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto timings = graph.resolve_timings(*cmd);
        if (timings.empty()) {
            MESSAGE("device cannot timestamp — the timing half is skipped");
        } else {
            // Every pass is live and exported, so an empty or short list here means culling, not a
            // device limit — assert the count before the property it is supposed to hold over.
            CHECK(timings.size() == std::size(kDeclared));
            std::vector<std::string> timed;
            timed.reserve(timings.size());
            for (const RenderGraph::PassTiming& t : timings)
                timed.emplace_back(t.name);
            CHECK(all_unique(timed));
        }
    }
}

TEST_CASE("pass identity: shadow work is named for shadows, and needs no uniquifier (m17.3)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping the shadow-naming proof");
        return;
    }

    constexpr std::uint32_t kSize = 128;
    constexpr std::uint32_t kCascades = 4;

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(20.0f), "identity-floor");
    const MeshId box = meshes.add(make_cube(0.5f), "identity-box");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});

    core::Transform box_tf{};
    box_tf.translation = {0.0f, 2.0f, 0.0f};
    (void)world.spawn_with(ecs::WorldTransform{box_tf}, MeshRef{box}, MaterialRef{mat});

    // The sun: a DirectionalLight aims down its own −z, so a −90° pitch points it at the ground.
    core::Transform sun_tf{};
    sun_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    (void)world.spawn_with(ecs::WorldTransform{sun_tf}, DirectionalLight{});

    // Two spots, far enough apart that both are live and both must render a slot on a cold cache.
    for (const float x : {-6.0f, 6.0f}) {
        core::Transform spot_tf{};
        spot_tf.translation = {x, 6.0f, 0.0f};
        spot_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
        SpotLight sl{};
        sl.intensity = 300.0f;
        (void)world.spawn_with(ecs::WorldTransform{spot_tf}, sl);
    }

    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 8.0f, 12.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.5f);
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 60.0f, true});

    LightingSettings ls;
    ls.shadows_enabled = true;
    ls.cascade_count = kCascades;
    ls.local_shadows_enabled = true;
    ls.local_shadow_resolution = 512;
    ls.shadow_map_resolution = 512;
    renderer.set_lighting(ls);

    RenderGraph graph(*device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());

    const std::vector<std::string> names = declared_names(graph);
    REQUIRE_FALSE(names.empty());

    // (a) The primary view keeps the name it always had — `pbr_pipeline_test` asserts that literal,
    //     and a committed report's `depth-prepass` row must keep meaning "the camera's pre-pass".
    CHECK(count_of(names, "depth-prepass") == 1);

    // (b) Every cascade is its own row. Before m17.3 these four WERE the extra "depth-prepass"
    //     entries, folded by name into one distribution and written as duplicate JSON keys.
    for (std::uint32_t c = 0; c < kCascades; ++c) {
        const std::string want = "csm-cascade-" + std::to_string(c);
        INFO("cascade name: ", want);
        CHECK(count_of(names, want) == 1);
    }

    // (c) Both spots rendered on a cold cache, and each named its own slot. The renderer's own
    //     counter is the cross-check: if it says two slots rendered, two slot names must exist —
    //     a skip that the report could not see is exactly what this brick is about.
    const LocalShadowStats stats = renderer.local_shadow_stats();
    CHECK(stats.rendered == 2);
    CHECK(count_of(names, "spot-shadow-0") == 1);
    CHECK(count_of(names, "spot-shadow-1") == 1);

    // (d) The whole frame's keys are unique...
    CHECK(all_unique(names));

    // (e) ...and NOT because the backstop rescued it. No name in a real frame carries a `#`, which
    //     is what says the callers name their own work rather than relying on the graph to
    //     disambiguate anonymous collisions. If this ever fires, some system started reusing a pass
    //     without saying which instance it is — the m17.3 bug, returning.
    for (const std::string& n : names) {
        INFO("pass name: ", n);
        CHECK(n.find('#') == std::string::npos);
    }
}
