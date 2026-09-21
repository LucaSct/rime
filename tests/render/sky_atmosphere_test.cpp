// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m17.7c's GPU proof is deliberately structural.  It checks the numerical guarantees of the LUTs
// and their cache keys rather than committing a screenshot of a future physical sky body.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "render_test_support.hpp"
#include "rime/render/lighting/sky.hpp"
#include "rime/render/render_graph.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::half_to_float;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr std::uint32_t kTransmittanceWidth = 256;
constexpr std::uint32_t kTransmittanceHeight = 64;
constexpr std::uint32_t kMultipleScatteringSize = 32;

void run_lighting_frame(rhi::Device& device, SkyPass& sky, const SkyParams& params) {
    RenderGraph graph(device);
    graph.reset();
    const SkyLightBinding binding = sky.add_lighting(graph, params, {});
    // Keeping sky-view makes its physical sampled dependencies a visible producer chain.  The
    // atmosphere tables are persistent imports too, so their direct writes survive even on a
    // physical-only edit before m17.7d uses their values in the sky body.
    graph.export_texture(binding.skyview);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
}

[[nodiscard]] float texel_red(const std::vector<std::uint8_t>& pixels,
                              std::uint32_t width,
                              std::uint32_t x,
                              std::uint32_t y) {
    const auto* half = reinterpret_cast<const std::uint16_t*>(pixels.data());
    return half_to_float(half[(static_cast<std::size_t>(y) * width + x) * 4]);
}

[[nodiscard]] float max_red(const std::vector<std::uint8_t>& pixels) {
    const auto* half = reinterpret_cast<const std::uint16_t*>(pixels.data());
    float result = 0.0f;
    for (std::size_t i = 0; i < pixels.size() / (4 * sizeof(std::uint16_t)); ++i) {
        result = std::max(result, half_to_float(half[i * 4]));
    }
    return result;
}

} // namespace

TEST_CASE("sky atmosphere: physical LUTs are finite, cached independently, and darken with "
          "extinction (m17.7c)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky atmosphere proof");
        return;
    }

    SkyPass sky(*device);
    SkyParams params{};
    params.enabled = true;
    params.use_scene_sun = false;
    params.clouds_enabled = false;

    // First use creates both persistent physical tables and fills them.  This must precede the
    // analytic sky-view/SH passes: their declared sampled reads pin the graph edge for m17.7d.
    run_lighting_frame(*device, sky, params);
    const SkyAtmosphereStats first = sky.atmosphere_stats();
    CHECK(first.transmittance_filled == 1);
    CHECK(first.multiple_scattering_filled == 1);
    CHECK(first.transmittance_reused == 0);
    CHECK(first.multiple_scattering_reused == 0);
    REQUIRE(sky.transmittance_lut().is_valid());
    REQUIRE(sky.multiple_scattering_lut().is_valid());

    const std::vector<std::uint8_t> transmittance = read_texture(
        *device, sky.transmittance_lut(), kTransmittanceWidth, kTransmittanceHeight, 8);
    const std::vector<std::uint8_t> multiple = read_texture(*device,
                                                            sky.multiple_scattering_lut(),
                                                            kMultipleScatteringSize,
                                                            kMultipleScatteringSize,
                                                            8);
    sky.note_transmittance_state(rhi::ResourceState::TransferSrc);
    sky.note_multiple_scattering_state(rhi::ResourceState::TransferSrc);
    for (const std::vector<std::uint8_t>* table : {&transmittance, &multiple}) {
        const auto* half = reinterpret_cast<const std::uint16_t*>(table->data());
        for (std::size_t i = 0; i < table->size() / sizeof(std::uint16_t); ++i) {
            const float value = half_to_float(half[i]);
            CHECK(std::isfinite(value));
            CHECK(value >= 0.0f);
        }
    }
    const float initial_multiple = max_red(multiple);
    CHECK(initial_multiple > 0.0f);
    // A representative low-altitude, sun-above-horizon lookup: sufficiently far from the opaque
    // ground-facing half of the table to make the monotonic extinction claim numerically sharp.
    const float clear_transmittance = texel_red(transmittance, kTransmittanceWidth, 224, 8);
    CHECK(clear_transmittance > 0.0f);

    // No change means both physical caches reuse.  The counters make a stale/always-refilling
    // cache observable instead of inferring it from a picture that could look plausible either way.
    run_lighting_frame(*device, sky, params);
    const SkyAtmosphereStats reused = sky.atmosphere_stats();
    CHECK(reused.transmittance_filled == 1);
    CHECK(reused.multiple_scattering_filled == 1);
    CHECK(reused.transmittance_reused == 1);
    CHECK(reused.multiple_scattering_reused == 1);

    // These all repaint/rebake the ANALYTIC sky, but physical-medium tables must not notice them.
    SkyParams authored = params;
    authored.zenith[0] = 0.91f;
    authored.horizon[2] = 0.13f;
    authored.clouds_enabled = true;
    authored.coverage = 0.8f;
    authored.sun_radiance[1] = 0.31f;
    authored.sun_direction[1] = -0.6f;
    run_lighting_frame(*device, sky, authored);
    const SkyAtmosphereStats authored_reused = sky.atmosphere_stats();
    CHECK(authored_reused.transmittance_filled == 1);
    CHECK(authored_reused.multiple_scattering_filled == 1);
    CHECK(authored_reused.transmittance_reused == 2);
    CHECK(authored_reused.multiple_scattering_reused == 2);

    // Ground albedo only feeds the MS closure. It must leave transmittance cached, refill only the
    // second table, and produce a measurably different non-zero result.
    SkyParams ground_edit = authored;
    ground_edit.atmosphere.ground_albedo[0] = 0.9f;
    ground_edit.atmosphere.ground_albedo[1] = 0.9f;
    ground_edit.atmosphere.ground_albedo[2] = 0.9f;
    run_lighting_frame(*device, sky, ground_edit);
    const SkyAtmosphereStats ground_refilled = sky.atmosphere_stats();
    CHECK(ground_refilled.transmittance_filled == 1);
    CHECK(ground_refilled.multiple_scattering_filled == 2);
    const std::vector<std::uint8_t> ground_multiple = read_texture(*device,
                                                                   sky.multiple_scattering_lut(),
                                                                   kMultipleScatteringSize,
                                                                   kMultipleScatteringSize,
                                                                   8);
    sky.note_multiple_scattering_state(rhi::ResourceState::TransferSrc);
    CHECK(max_red(ground_multiple) > initial_multiple * 1.2f);
    CHECK(sky.stats().filled == 3); // authored values, then the physical MS dependency

    SkyParams denser = ground_edit;
    // Rayleigh is appreciable at the near-ground sample below; multiplying this physical
    // coefficient produces a margin larger than RGBA16F's roundoff without changing any authored
    // sky control. A tiny Mie-absorption edit would be a real change but an unhelpfully weak test.
    denser.atmosphere.rayleigh_scattering[0] *= 8.0f;
    denser.atmosphere.rayleigh_scattering[1] *= 8.0f;
    denser.atmosphere.rayleigh_scattering[2] *= 8.0f;
    run_lighting_frame(*device, sky, denser);
    const SkyAtmosphereStats refilled = sky.atmosphere_stats();
    CHECK(refilled.transmittance_filled == 2);
    CHECK(refilled.multiple_scattering_filled == 3);
    CHECK(sky.stats().filled == 4); // a physical edit also invalidates the future m17.7d consumers
    const std::vector<std::uint8_t> dense_transmittance = read_texture(
        *device, sky.transmittance_lut(), kTransmittanceWidth, kTransmittanceHeight, 8);
    sky.note_transmittance_state(rhi::ResourceState::TransferSrc);
    const float dense_value = texel_red(dense_transmittance, kTransmittanceWidth, 224, 8);
    MESSAGE("transmittance red at (224,8): clear=" << clear_transmittance
                                                   << " dense=" << dense_value);
    CHECK(dense_value < clear_transmittance * 0.97f);
}

TEST_CASE("sky atmosphere: off creates no physical LUT work (m17.7c)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky-off atmosphere proof");
        return;
    }

    SkyPass sky(*device);
    RenderGraph graph(*device);
    graph.reset();
    const SkyLightBinding empty = sky.empty_binding(graph);
    graph.export_texture(empty.skyview);
    CHECK(graph.pass_count() == 0);
    CHECK_FALSE(sky.transmittance_lut().is_valid());
    CHECK_FALSE(sky.multiple_scattering_lut().is_valid());
    const SkyAtmosphereStats stats = sky.atmosphere_stats();
    CHECK(stats.transmittance_filled == 0);
    CHECK(stats.multiple_scattering_filled == 0);
}
