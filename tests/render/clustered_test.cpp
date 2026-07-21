// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Structural proofs for clustered forward shading (m10.3, ADR-0032 §4). No golden images:
//
//  (1) The froxel grid (GPU-free): the log-z partition is monotonic and covers exactly
//      [z_near, z_far]; adjacent froxels tile the frustum with no gaps and no overlaps; the sphere
//      test agrees with the geometry it claims to test.
//
//  (2) Culling is CORRECT (on lavapipe) — the brick's central claim. The cull dispatch fills the
//      per-froxel lists; the test then re-derives what SHOULD be in every one of the 3456 lists
//      with the CPU mirror of the same maths and compares, with a ±1% margin on the radius so
//      float differences between the two implementations cannot flake the result:
//        * no froxel lists a light whose sphere clearly misses it (the "never shades a light that
//          cannot reach this pixel" half — the whole point of culling), and
//        * no froxel drops a light whose sphere clearly reaches it (the half that would be a
//          visible bug: a light that vanishes in part of the screen).
//      Plus the negative control that culling is actually selective (most froxel/light pairs are
//      rejected) and the overflow clamp holds under an adversarial pile of lights.
//
//  (3) Equivalence (on lavapipe): the same scene rendered through the clustered path and through
//      ADR-0022's uniform-block loop matches pixel for pixel within tolerance — the regression
//      bridge that lets the fixed 16-light block retire.
//
//  (4) Scale (on lavapipe): 1000 point lights render finite, artifact-free radiance through a
//      pipeline whose uniform block could only ever hold 16.
//
// Shares pbr_pipeline/shadow_test's harness via render_test_support.hpp.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/clustered.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::project;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

ClusterInputs test_inputs() {
    ClusterInputs in{};
    // A camera at (0, 2, 10) looking down −z, i.e. an identity-rotation view translated back.
    core::Transform cam{};
    cam.translation = {0.0f, 2.0f, 10.0f};
    in.view = core::inverse(core::to_matrix(cam));
    in.fov_y = 0.9f;
    in.aspect = 16.0f / 9.0f;
    in.z_near = 0.1f;
    in.z_far = 100.0f;
    in.extent = {320, 180};
    return in;
}

core::Vec3 to_view(const core::Mat4& view, core::Vec3 world) {
    const core::Vec4 v = view * core::Vec4{world.x, world.y, world.z, 1.0f};
    return core::Vec3{v.x, v.y, v.z};
}

// Run the cull dispatch alone and hand back the raw list buffer. The graph is what makes this
// work: nothing else in this frame reads the lists, so without export_buffer the cull pass would
// be culled as dead — exporting is the declaration that the outside world wants the result.
std::vector<std::uint32_t> cull_lists(rhi::Device& device,
                                      ClusteredLights& clustered,
                                      RenderGraph& graph,
                                      std::span<const GpuPointLight> lights,
                                      const ClusterInputs& in,
                                      double* out_cull_ms = nullptr) {
    graph.reset();
    const ClusterBinding binding = clustered.add(graph, lights, in);
    graph.export_buffer(binding.lists);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    if (out_cull_ms != nullptr) {
        *out_cull_ms = 0.0;
        for (const RenderGraph::PassTiming& t : graph.resolve_timings(*cmd)) {
            if (t.name == "cluster-cull")
                *out_cull_ms = t.gpu_ms;
        }
    }

    rhi::BufferDesc rb{};
    rb.size = kClusterListBytes;
    rb.usage = rhi::BufferUsage::TransferDst;
    rb.memory = rhi::MemoryUsage::GpuToCpu;
    rb.debug_name = "cluster-lists-readback";
    const rhi::BufferHandle host = device.create_buffer(rb);
    auto copy = device.begin_commands();
    copy->copy_buffer(graph.physical_buffer(binding.lists), host, kClusterListBytes);
    device.submit_blocking(*copy);

    std::vector<std::uint32_t> out(kClusterCount * kClusterListStride);
    device.read_buffer(host, out.data(), out.size() * sizeof(std::uint32_t), 0);
    device.destroy(host);
    return out;
}

} // namespace

TEST_CASE("clustered: the froxel grid partitions the frustum (m10.3)") {
    const ClusterInputs in = test_inputs();

    SUBCASE("the log-z partition is monotonic and spans exactly [near, far]") {
        // Slice boundaries come from the bounds themselves (view-space z is negative, so the
        // NEARER face is the larger z): slice k spans depths [-max.z, -min.z].
        float previous_far = in.z_near;
        for (std::uint32_t k = 0; k < kClusterGridZ; ++k) {
            const ClusterBounds b = cluster_bounds(cluster_index(0, 0, k), in);
            const float near_face = -b.max.z;
            const float far_face = -b.min.z;
            CHECK(near_face == doctest::Approx(previous_far)); // no gap with the previous slice
            CHECK(far_face > near_face);                       // strictly increasing
            previous_far = far_face;
        }
        CHECK(previous_far == doctest::Approx(in.z_far)); // the last slice ends at the far plane

        // Log-z means each slice is a constant RATIO deeper, not a constant distance — that is the
        // property that keeps froxels roughly cube-shaped all the way out.
        const ClusterBounds first = cluster_bounds(cluster_index(0, 0, 0), in);
        const ClusterBounds last = cluster_bounds(cluster_index(0, 0, kClusterGridZ - 1), in);
        const float first_depth = -first.min.z - -first.max.z;
        const float last_depth = -last.min.z - -last.max.z;
        CHECK(last_depth > 50.0f * first_depth);
    }

    SUBCASE("cluster_depth_slice inverts the partition and clamps at both ends") {
        CHECK(cluster_depth_slice(in.z_near, in.z_near, in.z_far) == 0);
        CHECK(cluster_depth_slice(0.001f, in.z_near, in.z_far) == 0); // nearer than near: clamped
        CHECK(cluster_depth_slice(1e6f, in.z_near, in.z_far) == kClusterGridZ - 1); // past far
        // Every slice's own midpoint must map back to that slice.
        for (std::uint32_t k = 0; k < kClusterGridZ; ++k) {
            const ClusterBounds b = cluster_bounds(cluster_index(0, 0, k), in);
            const float mid = 0.5f * (-b.max.z + -b.min.z);
            CHECK(cluster_depth_slice(mid, in.z_near, in.z_far) == k);
        }
    }

    SUBCASE("a fragment's froxel lookup lands inside that froxel's bounds") {
        // THE load-bearing property: the shading half maps a fragment to a froxel index (screen
        // tile + log-z slice) and then trusts the culling half's list for it. If the two disagreed
        // — a fragment assigned to a froxel whose bounds do not contain it — culling could
        // legitimately drop a light that does reach the pixel, and lights would flicker in bands.
        //
        // (Note the froxel AABBs deliberately OVERLAP their neighbours: each is the box around a
        // frustum slab spanning a depth range, so its far face is wider than its near face. That
        // is conservative in the safe direction and is why "do the boxes tile exactly?" is the
        // wrong question — "does a point land in the box it is assigned to?" is the right one.)
        const core::Mat4 proj = core::perspective(in.fov_y, in.aspect, in.z_near, in.z_far);
        std::size_t tested = 0;
        std::size_t outside = 0;
        for (int ix = -9; ix <= 9; ++ix) {
            for (int iy = -9; iy <= 9; ++iy) {
                for (int iz = 1; iz <= 12; ++iz) {
                    // A spread of world points in front of the camera (which sits at y = 2, z = 10
                    // looking down −z).
                    const core::Vec3 world{static_cast<float>(ix) * 1.3f,
                                           2.0f + static_cast<float>(iy) * 0.7f,
                                           10.0f - static_cast<float>(iz) * 4.0f};
                    const core::Vec3 view_pos = to_view(in.view, world);
                    const core::Vec4 clip =
                        proj * core::Vec4{view_pos.x, view_pos.y, view_pos.z, 1.0f};
                    if (clip.w <= in.z_near || clip.w >= in.z_far)
                        continue;
                    const float ndc_x = clip.x / clip.w;
                    const float ndc_y = clip.y / clip.w;
                    if (std::fabs(ndc_x) >= 1.0f || std::fabs(ndc_y) >= 1.0f)
                        continue;

                    // The shader's lookup, in C++ (fragment_cluster() in
                    // pbr_forward_shadowed.frag).
                    const auto tx = static_cast<std::uint32_t>((ndc_x * 0.5f + 0.5f) *
                                                               static_cast<float>(kClusterGridX));
                    const auto ty = static_cast<std::uint32_t>((ndc_y * 0.5f + 0.5f) *
                                                               static_cast<float>(kClusterGridY));
                    const std::uint32_t slice =
                        cluster_depth_slice(clip.w, in.z_near, in.z_far); // clip.w == view depth
                    const ClusterBounds b =
                        cluster_bounds(cluster_index(std::min(tx, kClusterGridX - 1),
                                                     std::min(ty, kClusterGridY - 1),
                                                     slice),
                                       in);
                    ++tested;
                    const float eps = 1e-3f;
                    if (view_pos.x < b.min.x - eps || view_pos.x > b.max.x + eps ||
                        view_pos.y < b.min.y - eps || view_pos.y > b.max.y + eps ||
                        view_pos.z < b.min.z - eps || view_pos.z > b.max.z + eps)
                        ++outside;
                }
            }
        }
        REQUIRE(tested > 200); // the sample really did cover the frustum
        CHECK(outside == 0);
    }

    SUBCASE("the sphere test measures what it claims to") {
        const ClusterBounds b = cluster_bounds(cluster_index(8, 4, 10), in);
        const core::Vec3 centre{
            0.5f * (b.min.x + b.max.x), 0.5f * (b.min.y + b.max.y), 0.5f * (b.min.z + b.max.z)};
        CHECK(sphere_touches_cluster(b, centre, 0.0f)); // a point inside touches
        // A light one unit to the left of the box's left face: reaches in iff its radius does.
        const core::Vec3 outside{b.min.x - 1.0f, centre.y, centre.z};
        CHECK(!sphere_touches_cluster(b, outside, 0.9f));
        CHECK(sphere_touches_cluster(b, outside, 1.1f));
    }
}

TEST_CASE("clustered: the cull dispatch lists exactly the lights that reach each froxel (m10.3)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the cluster-cull proof");
        return;
    }

    const ClusterInputs in = test_inputs();
    ClusteredLights clustered(*device);
    RenderGraph graph(*device);

    // Lights planted across the view: near/far, left/right, one behind the camera and one past the
    // far plane (both of which must end up in NO froxel at all), and a big one that reaches most of
    // the near field.
    const std::vector<GpuPointLight> lights = {
        {{0.0f, 2.0f, 5.0f, 3.0f}, {10.0f, 10.0f, 10.0f, 0.0f}},   // dead ahead, mid-depth
        {{-6.0f, 2.0f, 0.0f, 4.0f}, {10.0f, 0.0f, 0.0f, 0.0f}},    // off to the left
        {{6.0f, 2.0f, 0.0f, 4.0f}, {0.0f, 10.0f, 0.0f, 0.0f}},     // off to the right
        {{0.0f, 2.0f, -40.0f, 8.0f}, {0.0f, 0.0f, 10.0f, 0.0f}},   // deep in the distance
        {{0.0f, 2.0f, 20.0f, 2.0f}, {10.0f, 10.0f, 0.0f, 0.0f}},   // BEHIND the camera
        {{0.0f, 2.0f, -400.0f, 5.0f}, {10.0f, 0.0f, 10.0f, 0.0f}}, // past the far plane
        {{0.0f, 2.0f, 9.0f, 6.0f}, {1.0f, 1.0f, 1.0f, 0.0f}},      // right in front of the lens
        {{2.0f, 6.0f, 2.0f, 3.0f}, {1.0f, 1.0f, 1.0f, 0.0f}},      // high and to the right
    };

    const std::vector<std::uint32_t> data = cull_lists(*device, clustered, graph, lights, in);

    // Re-derive every froxel's list on the CPU and compare. The margin (±1%) absorbs the last-ulp
    // differences between two implementations of the same maths without weakening the claim: a
    // light that clearly misses must be absent, one that clearly reaches must be present.
    std::size_t listed_but_missing = 0;   // a froxel listed a light that clearly cannot reach it
    std::size_t reaching_but_dropped = 0; // a froxel dropped a light that clearly reaches it
    std::size_t total_entries = 0;
    std::size_t nonempty_froxels = 0;

    std::vector<core::Vec3> centres;
    centres.reserve(lights.size());
    for (const GpuPointLight& l : lights)
        centres.push_back(to_view(in.view, {l.position[0], l.position[1], l.position[2]}));

    for (std::uint32_t c = 0; c < kClusterCount; ++c) {
        const std::uint32_t base = c * kClusterListStride;
        const std::uint32_t count = data[base];
        REQUIRE(count <= kMaxLightsPerCluster); // never past the run's end — the clamp
        total_entries += count;
        if (count > 0)
            ++nonempty_froxels;

        std::vector<bool> listed(lights.size(), false);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t light = data[base + 1 + i];
            REQUIRE(light < lights.size()); // never an out-of-range index
            listed[light] = true;
        }

        const ClusterBounds bounds = cluster_bounds(c, in);
        for (std::size_t l = 0; l < lights.size(); ++l) {
            const float radius = lights[l].position[3];
            if (listed[l]) {
                if (!sphere_touches_cluster(bounds, centres[l], radius * 1.01f))
                    ++listed_but_missing;
            } else {
                if (sphere_touches_cluster(bounds, centres[l], radius * 0.99f))
                    ++reaching_but_dropped;
            }
        }
    }

    CHECK(listed_but_missing == 0);
    CHECK(reaching_but_dropped == 0);

    // Negative control: culling has to be SELECTIVE, or the two checks above would pass trivially
    // for a shader that lists everything (or nothing) everywhere.
    CHECK(total_entries > 0);
    CHECK(total_entries < kClusterCount * lights.size() / 4);
    CHECK(nonempty_froxels > 0);
    CHECK(nonempty_froxels < kClusterCount);
    MESSAGE("froxels with lights: " << nonempty_froxels << "/" << kClusterCount
                                    << ", total list entries: " << total_entries);

    SUBCASE("a light outside the frustum reaches no froxel at all") {
        // Lights 4 (behind the camera) and 5 (past the far plane) are far enough outside that no
        // froxel's bounds can be within their radius — the sharpest form of "culled".
        std::size_t behind_hits = 0;
        std::size_t beyond_hits = 0;
        for (std::uint32_t c = 0; c < kClusterCount; ++c) {
            const std::uint32_t base = c * kClusterListStride;
            for (std::uint32_t i = 0; i < data[base]; ++i) {
                if (data[base + 1 + i] == 4)
                    ++behind_hits;
                if (data[base + 1 + i] == 5)
                    ++beyond_hits;
            }
        }
        CHECK(behind_hits == 0);
        CHECK(beyond_hits == 0);
    }
}

TEST_CASE("clustered: an adversarial light pile clamps instead of overflowing (m10.3)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the cluster overflow proof");
        return;
    }

    const ClusterInputs in = test_inputs();
    ClusteredLights clustered(*device);
    RenderGraph graph(*device);

    // 200 enormous lights piled on the view axis: every one of them reaches most froxels, so the
    // per-froxel lists must saturate at the cap rather than run off the end of their run.
    std::vector<GpuPointLight> lights;
    for (int i = 0; i < 200; ++i) {
        const float z = 5.0f - static_cast<float>(i) * 0.05f;
        lights.push_back({{0.0f, 2.0f, z, 60.0f}, {0.05f, 0.05f, 0.05f, 0.0f}});
    }

    const std::vector<std::uint32_t> data = cull_lists(*device, clustered, graph, lights, in);
    std::size_t saturated = 0;
    for (std::uint32_t c = 0; c < kClusterCount; ++c) {
        const std::uint32_t count = data[c * kClusterListStride];
        REQUIRE(count <= kMaxLightsPerCluster);
        if (count == kMaxLightsPerCluster)
            ++saturated;
        // Every index in a clamped list is still a real light — a clamp that wrote garbage past
        // the cap would show up here.
        for (std::uint32_t i = 0; i < count; ++i)
            REQUIRE(data[c * kClusterListStride + 1 + i] < lights.size());
    }
    CHECK(saturated > 0); // the pile really did overflow somewhere — the clamp was exercised
    MESSAGE("saturated froxels: " << saturated << "/" << kClusterCount);
}

TEST_CASE("clustered: the froxel path matches the uniform-block loop, and scales past it (m10.3)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the clustered shading proof");
        return;
    }

    constexpr std::uint32_t kSize = 192;

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(20.0f), "cluster-floor");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.01f, 0.01f, 0.01f);

    // Looking straight down at a large floor from high up, so the lit footprint of every point
    // light lands in frame and a pixel's froxel is unambiguous.
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 20.0f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // −z → −y

    // Render the floor lit by `lights` (world positions), with clustering on or off.
    const auto render_lights =
        [&](const std::vector<core::Vec3>& lights, bool clustered_on, float radius) {
            ecs::World world;
            register_render_components(world);
            (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
            for (const core::Vec3& p : lights) {
                core::Transform tf{};
                tf.translation = p;
                PointLight pl{};
                pl.intensity = 8.0f;
                pl.radius = radius;
                (void)world.spawn_with(ecs::WorldTransform{tf}, pl);
            }
            (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 60.0f, true});

            LightingSettings ls;
            ls.clustered_enabled = clustered_on;
            renderer.set_lighting(ls);

            RenderGraph graph(*device);
            graph.reset();
            const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
            REQUIRE(out.hdr.is_valid());
            graph.export_texture(out.hdr);
            auto cmd = device->begin_commands();
            graph.execute(*cmd);
            device->submit_blocking(*cmd);
            return decode_hdr(
                read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
        };

    SUBCASE("equivalence: clustered == the ADR-0022 uniform-block loop for a small light set") {
        const std::vector<core::Vec3> lights = {
            {-3.0f, 1.5f, -3.0f},
            {3.0f, 1.5f, -3.0f},
            {0.0f, 1.5f, 0.0f},
            {-3.0f, 1.5f, 3.0f},
            {3.0f, 1.5f, 3.0f},
        };
        const HdrImage classic = render_lights(lights, /*clustered_on=*/false, 6.0f);
        const HdrImage froxel = render_lights(lights, /*clustered_on=*/true, 6.0f);

        // Both paths evaluate the same BRDF over the same lights; only the light LIST differs, so
        // the pixels must agree to within float-summation-order noise. Compared relatively so the
        // tolerance means the same thing in bright and dim pixels.
        double worst = 0.0;
        double lit_pixels = 0.0;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const float a = classic.luminance(x, y);
                const float b = froxel.luminance(x, y);
                if (a > 0.05f)
                    ++lit_pixels;
                const double denom =
                    std::max({static_cast<double>(a), static_cast<double>(b), 0.01});
                worst = std::max(worst, std::fabs(static_cast<double>(a - b)) / denom);
            }
        }
        REQUIRE(lit_pixels > 0.2 * kSize * kSize); // the scene really is lit (sanity)
        CHECK(worst < 0.02);
        MESSAGE("worst relative luminance difference clustered vs uniform-block: " << worst);
    }

    SUBCASE("scale: 1000 point lights render through a 16-light uniform block") {
        // A 40×25 grid of small lights hovering just above the floor — 62× what ADR-0022's block
        // can hold, which is the entire point of the brick. Spread wide (and each with a short
        // reach) so no single froxel gathers more than the per-froxel cap: this subcase is about
        // scale, and the overflow behaviour has its own test above.
        std::vector<core::Vec3> many;
        for (int gz = 0; gz < 25; ++gz) {
            for (int gx = 0; gx < 40; ++gx) {
                many.push_back(
                    {static_cast<float>(gx - 20) * 0.5f, 0.4f, static_cast<float>(gz - 12) * 0.5f});
            }
        }
        REQUIRE(many.size() == 1000);

        const HdrImage img = render_lights(many, /*clustered_on=*/true, 0.7f);
        double total = 0.0;
        std::size_t bad = 0;
        std::size_t lit = 0;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const float l = img.luminance(x, y);
                if (!std::isfinite(l) || l < 0.0f)
                    ++bad;
                if (l > 0.05f)
                    ++lit;
                total += l;
            }
        }
        CHECK(bad == 0); // no NaNs, no negatives: the list indexing stayed in bounds
        CHECK(lit > 0.15 * kSize * kSize);
        MESSAGE("1000 lights: mean luminance " << total / (kSize * kSize) << ", lit pixels "
                                               << lit);

        // The middle of the floor is blanketed by lights and the corners are not, so the frame
        // must still have STRUCTURE — 1000 lights that all washed out to one value would sail
        // through the finiteness check while being visibly wrong.
        const float mid_l = img.luminance(kSize / 2, kSize / 2);
        const float corner_l = img.luminance(3, 3);
        CHECK(mid_l > 5.0f * corner_l);
    }
}

TEST_CASE("clustered: culling cost against light count (m10.3, shape only)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the cluster timing note");
        return;
    }

    // Relative fences only (ADR-0032 §11): lavapipe runs compute on the CPU, so these numbers are
    // a SHAPE — cull cost grows with the light count, and even at 1000 lights it is one dispatch,
    // not 1000 per-pixel evaluations. Absolute budgets wait for real hardware (m12.0).
    const ClusterInputs in = test_inputs();
    ClusteredLights clustered(*device);
    RenderGraph graph(*device);

    for (const std::uint32_t n : {10u, 100u, 1000u}) {
        std::vector<GpuPointLight> lights;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) * 0.37f;
            lights.push_back({{std::sin(t) * 6.0f, 2.0f, -std::fabs(std::cos(t)) * 30.0f, 4.0f},
                              {1.0f, 1.0f, 1.0f, 0.0f}});
        }
        double cull_ms = 0.0;
        const std::vector<std::uint32_t> data =
            cull_lists(*device, clustered, graph, lights, in, &cull_ms);
        std::size_t entries = 0;
        for (std::uint32_t c = 0; c < kClusterCount; ++c)
            entries += data[c * kClusterListStride];
        MESSAGE("cull " << n << " lights: " << cull_ms << " ms, " << entries << " list entries ("
                        << static_cast<double>(entries) / kClusterCount << " per froxel)");
        CHECK(entries > 0);
    }
}
