// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The SSR resolve's DDGI fallback must tell NO PROBE COVERAGE from DARK RADIANCE (2026-10-14).
//
// ssr_resolve.frag reads the DDGI irradiance field in the reflection direction wherever the screen
// march misses or the surface is rough. Its sampler drops every probe that fails the trilinear,
// wrap or Chebyshev gate; when ALL of them fail it has no answer. It used to return black in that
// case, and the resolve replaced its (correct, already computed) sky fallback with that black. In
// 99-the-block that is every far road pixel whose reflection points away from the probes behind
// it: a measured capture with the no-coverage branch painted magenta put magenta on every dark road
// pixel sampled. The fix is the flag the sampler already has the evidence for, `covered`: no
// coverage keeps the sky fallback, coverage keeps DDGI, even when DDGI says 0.
//
// HOW THE PROOF IS BUILT. The resolve pass runs ALONE, on hand-filled inputs, so each case controls
// exactly one variable and the expected answer is known rather than rendered. The DDGI atlases
// have the real formats and the real tile layout (lighting/ddgi.hpp); only their contents are
// chosen. Coverage is controlled through the VISIBILITY atlas -- the Chebyshev gate:
//   - mean 0, variance 0 says "a wall at distance zero in every direction", so every probe is
//     rejected for any fragment at a positive distance: NO coverage, with no geometric luck needed.
//   - mean 100 m says "nothing in the way", so every probe whose wrap weight is positive passes.
// The fragment sits OUTSIDE the probe grid (2 m in front of it) on purpose: that is the block's
// geometry, and it shows that clamping to the lattice edge is not what decides coverage. Every
// pixel's reflection points back at the grid, so the wrap gate passes all four probes it reads.
//
// Roughness 1 makes the cone fully rough, so the screen march is skipped and the output is the
// probe term alone; metallic 1 with a white base makes F0 = 1, so Schlick's weight is exactly 1;
// the scene colour is black. Each output pixel is therefore the fallback radiance itself.
//
// Sensitivity control: temporarily put back the old unconditional
// `if (ddgi.enabled_pad.x != 0u) probe = ddgi_sample_irradiance(...)` and the first case fails --
// the uncovered pixels read 0 against a reference of 0.5. The covered-dark case is the control in
// the other direction: a fix that tested the RADIANCE for zero instead of coverage would turn that
// sealed-room black into sky, and fails it.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/lighting/sky_specular.hpp"
#include "rime/render/lighting/ssr.hpp"
#include "rime/render/render_graph.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::half_to_float;
using rime::render::test::HdrImage;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr std::uint32_t kSize = 8;
constexpr float kWallZ = -2.0f; // view space; the camera is the world origin looking down -z

// The probe grid: 2x2x2 probes, 2 m apart, its nearest layer 0.5 m BEHIND the camera. The wall
// (z = -2) is outside it; every pixel's mirror ray (wall normal +z) points back toward it.
constexpr float kGridOrigin[3] = {-1.0f, -1.0f, 0.5f};
constexpr float kGridSpacing = 2.0f;

// Radiances, each exactly representable in half precision so a constant texture reads back exactly.
constexpr std::array<float, 3> kSkyLut{0.25f, 0.5f, 1.0f};        // binding 7, the raw sky LUT
constexpr std::array<float, 3> kChainSource{1.5f, 0.75f, 0.375f}; // what the chain is baked from
constexpr std::array<float, 3> kDdgiBright{2.0f, 1.0f, 0.5f};

[[nodiscard]] std::unique_ptr<rhi::Device> device_or_skip(const char* what) {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available -- skipping " << what);
    }
    return device;
}

// Only for values that are EXACT in half precision (all the constants above); the round trip is
// checked so a value that is not cannot slip in silently.
[[nodiscard]] std::uint16_t exact_half(float f) {
    if (f == 0.0f)
        return 0;
    std::uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const std::uint32_t exp = ((x >> 23) & 0xFFu) - 127u + 15u;
    const auto h =
        static_cast<std::uint16_t>(((x >> 16) & 0x8000u) | (exp << 10) | ((x & 0x7FFFFFu) >> 13));
    REQUIRE(half_to_float(h) == f);
    return h;
}

struct Fixture {
    enum class Coverage { None, Covered, ProbePlane };

    rhi::Device& device;
    SsrPass ssr;
    SkySpecular sky_specular;
    rhi::SamplerHandle linear;
    std::vector<rhi::TextureHandle> owned;

    explicit Fixture(rhi::Device& d) : device(d), ssr(d), sky_specular(d) {
        rhi::SamplerDesc sd{};
        sd.mag_filter = rhi::Filter::Linear;
        sd.min_filter = rhi::Filter::Linear;
        sd.address_mode = rhi::AddressMode::ClampToEdge;
        sd.debug_name = "ssr-coverage-linear";
        linear = device.create_sampler(sd);
    }

    ~Fixture() {
        for (const rhi::TextureHandle t : owned)
            device.destroy(t);
        device.destroy(linear);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    rhi::TextureHandle upload(rhi::Extent2D extent,
                              std::uint32_t depth,
                              rhi::Format format,
                              const void* data,
                              std::size_t bytes) {
        rhi::TextureDesc td{};
        td.extent = extent;
        td.depth = depth;
        td.format = format;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        td.debug_name = "ssr-coverage-input";
        const rhi::TextureHandle t = device.create_texture(td);
        device.write_texture(t, data, bytes);
        owned.push_back(t);
        return t;
    }

    rhi::TextureHandle constant_f32(rhi::Extent2D e, std::array<float, 4> v) {
        std::vector<float> px(static_cast<std::size_t>(e.width) * e.height * 4);
        for (std::size_t i = 0; i < px.size(); ++i)
            px[i] = v[i % 4];
        return upload(e, 1, rhi::Format::RGBA32Float, px.data(), px.size() * sizeof(float));
    }

    rhi::TextureHandle constant_f16(rhi::Extent2D e, std::array<float, 4> v) {
        std::vector<std::uint16_t> px(static_cast<std::size_t>(e.width) * e.height * 4);
        for (std::size_t i = 0; i < px.size(); ++i)
            px[i] = exact_half(v[i % 4]);
        return upload(e, 1, rhi::Format::RGBA16Float, px.data(), px.size() * sizeof(std::uint16_t));
    }

    rhi::TextureHandle constant_rg32(rhi::Extent2D e, float mean, float mean2) {
        std::vector<float> px(static_cast<std::size_t>(e.width) * e.height * 2);
        for (std::size_t i = 0; i < px.size(); i += 2) {
            px[i] = mean;
            px[i + 1] = mean2;
        }
        return upload(e, 1, rhi::Format::RG32Float, px.data(), px.size() * sizeof(float));
    }

    rhi::TextureHandle probe_plane_visibility(std::uint32_t dims) {
        const auto per_row = dims * dims;
        const rhi::Extent2D e{per_row * kDdgiVisibilityTileSize, dims * kDdgiVisibilityTileSize};
        std::vector<float> px(static_cast<std::size_t>(e.width) * e.height * 2);
        for (std::uint32_t y = 0; y < e.height; ++y) {
            for (std::uint32_t x = 0; x < e.width; ++x) {
                // Real packing: one XY plane of tiles per atlas row, Z slices stacked down.
                const auto global =
                    x / kDdgiVisibilityTileSize + (y / kDdgiVisibilityTileSize) * per_row;
                const float mean = global / (dims * dims) >= 1 ? 100.0f : 0.0f;
                const std::size_t i = 2 * (static_cast<std::size_t>(y) * e.width + x);
                px[i] = mean;
                px[i + 1] = mean * mean;
            }
        }
        return upload(e, 1, rhi::Format::RG32Float, px.data(), px.size() * sizeof(float));
    }

    // A narrow-band SDF level filled with ONE unsaturated distance: half the band everywhere. It
    // is never solid (d > 0), so the cone never hits; it only narrows, and sdf_cone_occlusion's
    // min(d / cone radius) darkens the sky by a fixed, scene-independent fraction.
    rhi::TextureHandle half_band_sdf() {
        constexpr std::uint32_t kRes = 4;
        std::vector<std::int16_t> v(kRes * kRes * kRes, static_cast<std::int16_t>(16384));
        return upload({kRes, kRes}, kRes, rhi::Format::R16Snorm, v.data(), v.size() * 2);
    }

    struct Case {
        bool ddgi_enabled = true;
        Coverage coverage = Coverage::None;
        std::array<float, 3> ddgi_radiance{0.0f, 0.0f, 0.0f};
        bool chain = false;
        bool sdf = false;
        float wall_z = kWallZ;
        bool finite_support = false; // legacy authored/thin grids retain their clamp contract
        std::uint32_t grid_dims = 2;
        float grid_spacing = kGridSpacing;
        bool snap_grid_to_camera = false;
        core::Vec3 camera_position{};
        bool track_world_point = false;
        core::Vec3 world_point{}; // held at pixel (3,3) using an off-axis frustum
    };

    core::Vec3 last_origin{};

    HdrImage run(const Case& c) {
        core::Mat4 proj = core::perspective(1.0f, 1.0f, 0.1f, 100.0f);
        if (c.track_world_point) {
            // Keep a real world-plane point under the SAME pixel while translating the camera.
            // An off-axis perspective frustum changes neither that plane nor its +Z normal.
            constexpr float query_ndc = (3.5f / kSize) * 2.0f - 1.0f;
            const float distance = c.camera_position.z - c.wall_z;
            proj.at(0, 2) =
                proj.at(0, 0) * (c.world_point.x - c.camera_position.x) / distance - query_ndc;
            proj.at(1, 2) =
                proj.at(1, 1) * (c.world_point.y - c.camera_position.y) / distance - query_ndc;
        }
        const core::Vec4 wall_clip =
            proj * core::Vec4{0.0f, 0.0f, c.wall_z - c.camera_position.z, 1.0f};
        const float wall_ndc = wall_clip.z / wall_clip.w;

        const rhi::Extent2D e{kSize, kSize};
        const rhi::TextureHandle scene_color = constant_f32(e, {0.0f, 0.0f, 0.0f, 1.0f});
        // G-buffer: oct-encoded world normal +z is (0, 0); roughness 1; mask 1.
        const rhi::TextureHandle gbuffer = constant_f32(e, {0.0f, 0.0f, 1.0f, 1.0f});
        const rhi::TextureHandle depth = constant_f32(e, {wall_ndc, 0.0f, 0.0f, 0.0f});
        const rhi::TextureHandle material = constant_f32(e, {1.0f, 1.0f, 1.0f, 1.0f});
        const rhi::TextureHandle lut =
            constant_f16({32, 18}, {kSkyLut[0], kSkyLut[1], kSkyLut[2], 1.0f});

        // Match DdgiProbes' actual XY-plane packing at every dimension; the finite-volume
        // regressions use 8^3 real tiles, never a tiny atlas sampled out of bounds.
        const auto per_row = c.grid_dims * c.grid_dims;
        const rhi::TextureHandle irradiance =
            constant_f16({per_row * kDdgiIrradianceTileSize, c.grid_dims * kDdgiIrradianceTileSize},
                         {c.ddgi_radiance[0], c.ddgi_radiance[1], c.ddgi_radiance[2], 1.0f});
        const float mean = c.coverage == Coverage::Covered ? 100.0f : 0.0f;
        const rhi::TextureHandle visibility =
            c.coverage == Coverage::ProbePlane
                ? probe_plane_visibility(c.grid_dims)
                : constant_rg32(
                      {per_row * kDdgiVisibilityTileSize, c.grid_dims * kDdgiVisibilityTileSize},
                      mean,
                      mean * mean);

        GpuDdgiSampleParams dp{};
        const std::array camera{c.camera_position.x, c.camera_position.y, c.camera_position.z};
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float half_extent = static_cast<float>(c.grid_dims - 1) * c.grid_spacing * .5f;
            dp.grid_origin_spacing[axis] =
                c.snap_grid_to_camera
                    ? std::floor((camera[axis] - half_extent) / c.grid_spacing) * c.grid_spacing
                    : kGridOrigin[axis];
        }
        if (c.coverage == Coverage::ProbePlane)
            dp.grid_origin_spacing[2] = c.wall_z;
        dp.grid_origin_spacing[3] = c.grid_spacing;
        dp.grid_dims_perrow[0] = dp.grid_dims_perrow[1] = dp.grid_dims_perrow[2] = c.grid_dims;
        dp.grid_dims_perrow[3] = per_row;
        dp.enabled_pad[0] = c.ddgi_enabled ? 1u : 0u;
        dp.enabled_pad[1] = c.finite_support ? 1u : 0u;
        last_origin = {
            dp.grid_origin_spacing[0], dp.grid_origin_spacing[1], dp.grid_origin_spacing[2]};

        RenderGraph graph(device);
        graph.reset();
        const auto imp = [&](rhi::TextureHandle t) {
            return graph.import_texture(t, rhi::ResourceState::ShaderRead);
        };

        SkySpecularBinding spec{};
        if (c.chain) {
            const rhi::TextureHandle src =
                constant_f16({64, 36}, {kChainSource[0], kChainSource[1], kChainSource[2], 1.0f});
            spec = sky_specular.add(graph, imp(src), /*source_changed=*/true);
        } else {
            spec = sky_specular.empty_binding(graph);
        }

        SdfSpecularOcclusionBinding sdf{};
        if (c.sdf) {
            GpuSdfClipmapLevels levels{};
            for (std::uint32_t i = 0; i < kSdfClipmapLevels; ++i) {
                // The same 8 m box at every level, centred on the wall: band 1 m, 0.25 m voxels.
                levels.levels[i].origin_extent[0] = -4.0f;
                levels.levels[i].origin_extent[1] = -4.0f;
                levels.levels[i].origin_extent[2] = -4.0f;
                levels.levels[i].origin_extent[3] = 8.0f;
                levels.levels[i].band_voxel[0] = 1.0f;
                levels.levels[i].band_voxel[1] = 0.25f;
                sdf.levels[i] = imp(half_band_sdf());
            }
            sdf.params = graph.push_frame_data(&levels, sizeof(levels));
            sdf.sampler = linear;
        }

        SsrInputs in{};
        in.view = core::mat4_translation(-c.camera_position);
        in.proj = proj;
        in.z_near = 0.1f;
        in.z_far = 100.0f;
        in.extent = e;
        in.ambient[0] = in.ambient[1] = in.ambient[2] = 7.0f; // must never be read: the sky is on

        const RGTexture out =
            graph.create_texture({e, rhi::Format::RGBA16Float, "ssr-coverage-out"});
        ssr.add(graph,
                imp(scene_color),
                imp(gbuffer),
                imp(depth),
                out,
                in,
                imp(irradiance),
                imp(visibility),
                graph.push_frame_data(&dp, sizeof(dp)),
                linear,
                imp(lut),
                linear,
                /*sky_enabled=*/true,
                imp(material),
                spec,
                linear,
                /*sky_specular_live=*/c.chain,
                sdf);
        graph.export_texture(out);
        auto cmd = device.begin_commands();
        graph.execute(*cmd);
        device.submit_blocking(*cmd);
        return decode_hdr(read_texture(device, graph.physical(out), kSize, kSize, 8), kSize, kSize);
    }
};

double rel(double a, double b) {
    return std::abs(a - b) / std::max(std::abs(b), 1e-9);
}

// The worst per-channel relative error of every pixel against `expected`.
double worst_against(const HdrImage& img, const std::array<float, 3>& expected) {
    double worst = 0.0;
    for (std::size_t i = 0; i < img.rgb.size(); ++i)
        worst = std::max(worst, rel(img.rgb[i], expected[i % 3]));
    return worst;
}

// The worst per-channel relative error of every pixel against the SAME pixel of `ref`.
double worst_against(const HdrImage& img, const HdrImage& ref) {
    double worst = 0.0;
    for (std::size_t i = 0; i < img.rgb.size(); ++i)
        worst = std::max(worst, rel(img.rgb[i], ref.rgb[i]));
    return worst;
}

float max_channel(const HdrImage& img) {
    float m = 0.0f;
    for (const float v : img.rgb)
        m = std::max(m, std::abs(v));
    return m;
}

} // namespace

TEST_CASE("ssr ddgi coverage: no accepted probe keeps the sky fallback; a covered dark probe stays "
          "dark") {
    auto device = device_or_skip("the SSR DDGI-coverage proof");
    if (!device)
        return;
    Fixture f(*device);

    // The reference: the same pixels with DDGI off are exactly the LUT.
    const HdrImage sky = f.run({.ddgi_enabled = false});
    // DDGI on, every probe rejected. The fix: identical to the reference. The bug: all zeros.
    const HdrImage uncovered = f.run({.coverage = Fixture::Coverage::None});
    // DDGI on, every probe accepted and reading black: the sealed room. Must stay 0.
    const HdrImage covered_dark = f.run({.coverage = Fixture::Coverage::Covered});
    // The same coverage reading a bright field: proves the dark case really was covered (it reads
    // DDGI, not the sky) and that a covered answer still takes precedence over the sky.
    const HdrImage covered_bright =
        f.run({.coverage = Fixture::Coverage::Covered, .ddgi_radiance = kDdgiBright});
    // Uncovered pixels whose radiance atlas is BRIGHT: an uncovered probe's contents must not leak.
    const HdrImage uncovered_bright =
        f.run({.coverage = Fixture::Coverage::None, .ddgi_radiance = kDdgiBright});

    MESSAGE("ssr coverage: px(3,3) sky=" << sky.luminance(3, 3)
                                         << " uncovered=" << uncovered.luminance(3, 3)
                                         << " covered-dark=" << covered_dark.luminance(3, 3)
                                         << " covered-bright=" << covered_bright.luminance(3, 3));

    CHECK_MESSAGE(worst_against(sky, kSkyLut) < 1e-3, "the DDGI-off reference is the raw sky LUT");
    CHECK_MESSAGE(worst_against(uncovered, sky) < 1e-3,
                  "no accepted probe must keep the sky fallback, not reflect black");
    CHECK_MESSAGE(worst_against(uncovered_bright, sky) < 1e-3,
                  "a rejected probe's radiance must not leak into the fallback");
    CHECK_MESSAGE(max_channel(covered_dark) <= 1e-6, "a covered dark field must stay dark");
    CHECK_MESSAGE(worst_against(covered_bright, kDdgiBright) < 1e-3,
                  "a covered field must win over the sky");
}

TEST_CASE(
    "ssr ddgi coverage: with the prefiltered chain live, an uncovered pixel reads the chain") {
    auto device = device_or_skip("the SSR DDGI-coverage chain proof");
    if (!device)
        return;
    Fixture f(*device);

    // At roughness 1 the lookup is the chain's last layer, baked from a constant source that is NOT
    // the LUT bound beside it -- so reading the chain and reading the LUT give different answers.
    const HdrImage uncovered = f.run({.coverage = Fixture::Coverage::None, .chain = true});
    const HdrImage covered_bright = f.run(
        {.coverage = Fixture::Coverage::Covered, .ddgi_radiance = kDdgiBright, .chain = true});
    MESSAGE("ssr coverage chain: px(3,3) uncovered=" << uncovered.luminance(3, 3) << " covered="
                                                     << covered_bright.luminance(3, 3));

    // A constant sky filters to itself (sky_specular_test's white furnace holds it to 2e-3).
    CHECK_MESSAGE(worst_against(uncovered, kChainSource) < 1e-2,
                  "an uncovered pixel must read the roughness-aware chain");
    CHECK_MESSAGE(worst_against(uncovered, kSkyLut) > 0.5, "and not the raw LUT");
    CHECK_MESSAGE(worst_against(covered_bright, kDdgiBright) < 1e-3,
                  "a covered field still wins with the chain live");
}

TEST_CASE("ssr ddgi coverage: SDF occlusion darkens an uncovered sky and never a covered probe") {
    auto device = device_or_skip("the SSR DDGI-coverage SDF proof");
    if (!device)
        return;
    Fixture f(*device);

    const HdrImage open_sky = f.run({.ddgi_enabled = false});
    // The reference occluded sky: DDGI off, the path the SDF proofs already established.
    const HdrImage occluded_sky = f.run({.ddgi_enabled = false, .sdf = true});
    const HdrImage uncovered = f.run({.coverage = Fixture::Coverage::None, .sdf = true});
    const HdrImage covered_bright =
        f.run({.coverage = Fixture::Coverage::Covered, .ddgi_radiance = kDdgiBright, .sdf = true});

    const double ratio = occluded_sky.luminance(3, 3) / std::max(open_sky.luminance(3, 3), 1e-9f);
    MESSAGE("ssr coverage sdf: px(3,3) open=" << open_sky.luminance(3, 3) << " occluded="
                                              << occluded_sky.luminance(3, 3) << " (ratio " << ratio
                                              << ") uncovered+sdf=" << uncovered.luminance(3, 3));

    // Sensitivity: the half-band field must actually bite, or the next check proves nothing --
    // and must not be total either, or it could not tell an occluded sky from the old black.
    REQUIRE(ratio < 0.5);
    REQUIRE(ratio > 1e-3);
    CHECK_MESSAGE(worst_against(uncovered, occluded_sky) < 1e-2,
                  "an uncovered pixel's sky is occluded exactly as with DDGI off");
    CHECK_MESSAGE(worst_against(covered_bright, kDdgiBright) < 1e-3,
                  "the SDF cone must never alter a covered DDGI answer");
}

TEST_CASE("ssr ddgi coverage: normal-biased query stays on the covered side of a probe plane") {
    auto device = device_or_skip("the SSR probe-plane stability proof");
    if (!device)
        return;
    Fixture f(*device);

    // The wall is the z=0 probe plane, with a zero-visibility layer on it and a bright,
    // visible layer at z=+2. These depth perturbations straddle the plane without changing
    // the reflection direction or material. A query at the unmodified negative wall position
    // clamps to z=0 and would return the sky; the 5 cm normal bias reaches the visible layer.
    const HdrImage just_below = f.run({.coverage = Fixture::Coverage::ProbePlane,
                                       .ddgi_radiance = kDdgiBright,
                                       .chain = true,
                                       .wall_z = kWallZ - 0.001f});
    const HdrImage just_above = f.run({.coverage = Fixture::Coverage::ProbePlane,
                                       .ddgi_radiance = kDdgiBright,
                                       .chain = true,
                                       .wall_z = kWallZ + 0.001f});
    // Ten centimetres below the plane is beyond the five-centimetre bias. A blanket "DDGI is
    // covered" branch fails this control: it must still choose the actual filtered sky.
    const HdrImage outside = f.run({.coverage = Fixture::Coverage::ProbePlane,
                                    .ddgi_radiance = kDdgiBright,
                                    .chain = true,
                                    .wall_z = kWallZ - 0.1f});

    MESSAGE("ssr probe plane: below=" << just_below.luminance(3, 3)
                                      << " above=" << just_above.luminance(3, 3)
                                      << " outside=" << outside.luminance(3, 3));
    CHECK_MESSAGE(worst_against(just_below, kDdgiBright) < 1e-2,
                  "negative depth perturbation must see the bright accepted probe layer");
    CHECK_MESSAGE(worst_against(just_above, kDdgiBright) < 1e-2,
                  "positive depth perturbation must see the same bright probe layer");
    CHECK_MESSAGE(worst_against(just_below, just_above) < 1e-2,
                  "small plane-crossing depth changes must not flip reflection source");
    CHECK_MESSAGE(worst_against(outside, kChainSource) < 1e-2,
                  "a query beyond the bias must still read the filtered sky");
    // Each sky channel is 75% of its probe counterpart: the analytic control gap is 25%.
    CHECK_MESSAGE(worst_against(outside, kDdgiBright) > 0.2,
                  "the unsupported control must differ visibly from probe radiance");
}

TEST_CASE("ssr ddgi finite support: covered core darkness, missing coverage and remote fallback") {
    auto device = device_or_skip("finite DDGI SSR support proof");
    if (!device)
        return;
    Fixture f(*device);
    Fixture::Case core{};
    core.coverage = Fixture::Coverage::Covered;
    core.wall_z = -0.5f;
    core.finite_support = true;
    core.grid_dims = 8;
    core.grid_spacing = 1.0f;
    core.snap_grid_to_camera = true;
    const auto black = f.run(core);
    core.ddgi_radiance = kDdgiBright;
    const auto bright = f.run(core);
    core.coverage = Fixture::Coverage::None;
    const auto rejected = f.run(core);
    CHECK_MESSAGE(max_channel(black) <= 1e-6f, "accepted black in the supported core stays black");
    CHECK_MESSAGE(worst_against(bright, kDdgiBright) < 1e-3,
                  "bright-atlas control proves the core really accepts probes");
    CHECK_MESSAGE(worst_against(rejected, kSkyLut) < 1e-3,
                  "no accepted core probe retains the sky despite a bright irradiance atlas");

    Fixture::Case remote = core;
    remote.coverage = Fixture::Coverage::Covered;
    remote.track_world_point = true;
    remote.world_point = {6.0f, 0.0f, remote.wall_z};
    const auto finite = f.run(remote);
    remote.finite_support = false;
    const auto legacy = f.run(remote);
    const auto channel = [](const HdrImage& image, unsigned c) {
        return image.rgb[(3 * kSize + 3) * 3 + c];
    };
    for (unsigned c = 0; c < 3; ++c) {
        CHECK(channel(finite, c) == doctest::Approx(kSkyLut[c]).epsilon(.002));
        CHECK(channel(legacy, c) == doctest::Approx(kDdgiBright[c]).epsilon(.002));
    }
    MESSAGE("finite SSR remote: sky=" << finite.luminance(3, 3)
                                      << "; legacy covered-probe sensitivity="
                                      << legacy.luminance(3, 3));
}

TEST_CASE("ssr ddgi finite support: a fixed world query blends continuously across camera snap") {
    auto device = device_or_skip("finite DDGI recenter continuity proof");
    if (!device)
        return;
    Fixture f(*device);
    Fixture::Case query{};
    query.coverage = Fixture::Coverage::Covered;
    query.ddgi_radiance = kDdgiBright;
    query.wall_z = -0.5f;
    query.finite_support = true;
    query.grid_dims = 8;
    query.grid_spacing = 1.0f;
    query.snap_grid_to_camera = true;
    query.track_world_point = true;
    query.world_point = {2.5f, 0.0f, query.wall_z};
    query.camera_position = {0.49f, 0.0f, 0.0f};
    const auto before = f.run(query);
    const auto origin_before = f.last_origin;
    query.camera_position.x = .51f;
    const auto after = f.run(query);
    REQUIRE(f.last_origin.x == origin_before.x + 1.0f);
    const auto sample = [](const HdrImage& image, unsigned channel) {
        return image.rgb[(3 * kSize + 3) * 3 + channel];
    };
    float largest_step = 0.0f;
    for (unsigned c = 0; c < 3; ++c) {
        const float lo = std::min(kSkyLut[c], kDdgiBright[c]);
        const float hi = std::max(kSkyLut[c], kDdgiBright[c]);
        CHECK(sample(before, c) > lo + .1f * (hi - lo));
        CHECK(sample(before, c) < hi - .1f * (hi - lo));
        CHECK(sample(after, c) > lo + .1f * (hi - lo));
        CHECK(sample(after, c) < hi - .1f * (hi - lo));
        const float change = std::abs(sample(after, c) - sample(before, c));
        largest_step = std::max(largest_step, change);
        // A two-centimetre camera move must change a one-metre fade by a small fraction of its
        // measured sky/probe control gap, even while the actual lattice moves a WHOLE metre.
        CHECK(change < .04f * (hi - lo) + .002f);
    }
    CHECK(largest_step > .005f); // sensitivity: continuous support is live, not frozen to a slot
    CHECK(sample(after, 0) > sample(before, 0)); // camera moved toward this fixed bright query
    MESSAGE("finite SSR snap: fixed world (2.5,0,-.5), camera .49->.51, grid "
            << origin_before.x << "->" << f.last_origin.x
            << "; max radiance step=" << largest_step);
}
