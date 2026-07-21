// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Structural proofs for m10.5a's DDGI probes (ADR-0032 §2). No golden images — every claim is a
// property the technique guarantees, checked with a stated margin:
//
//  (1) PHYSICAL SANITY: a probe in an open, sunlit space accumulates non-zero irradiance; a probe
//      sealed inside a closed box stays dark (the self-shadow sphere-trace working, not test-scene
//      trickery — see ddgi_trace.comp's header).
//
//  (2) DIRECTIONALITY: for a probe above a lit floor, the octahedral texel pointing DOWN (toward
//      the lit surface) carries materially more irradiance than the one pointing UP (empty sky) —
//      the probe stores a direction-dependent function, not one number. Also exercises the
//      BORDER's correctness directly (the "classic bug" the brief calls out): the down direction
//      (0,-1,0) encodes to the octahedral square's bottom EDGE, so its interior texel has a real
//      border neighbour one row further out, and the fold-then-decode logic
//      (ddgi_blend_irradiance.comp) must make the two agree.
//
//  (3) TEMPORAL CONVERGENCE: irradiance approaches a steady state over repeated updates and the
//      frame-to-frame delta decays — the point of the per-frame random ray rotation + hysteresis
//      blend (docs/math/ddgi.md §5).
//
//  (4) SNAP STABILITY: sub-spacing camera motion does not reshuffle the lattice (zero probes
//      re-primed, origin bit-identical) — the same anti-shimmer discipline SdfClipmap's own levels
//      already prove (tests/render/sdf_clipmap_test.cpp).
//
// Shares the box-SDF builder's SHAPE with sdf_clipmap_test.cpp (same analytic formula, same
// padding policy) but keeps its own copy — each GPU test TU builds the fixtures it needs, the same
// discipline sdf_clipmap_test.cpp itself follows relative to earlier proofs.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/render_graph.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::vulkan_required;

namespace {

// ── Hand-built, analytically-exact box SDFs (mirrors sdf_clipmap_test.cpp's own builder) ────────

float analytic_box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

assets::MeshSdfAsset build_box_sdf(core::Vec3 half_extents, std::uint32_t target_resolution = 24) {
    const float longest = std::max({half_extents.x, half_extents.y, half_extents.z}) * 2.0f;
    const float voxel_size = longest / static_cast<float>(target_resolution);
    const float pad = 2.0f * voxel_size;
    std::uint32_t res[3] = {0, 0, 0};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    const float half[3] = {half_extents.x, half_extents.y, half_extents.z};
    for (int a = 0; a < 3; ++a) {
        const float padded_extent = 2.0f * half[a] + 2.0f * pad;
        res[a] = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(padded_extent / voxel_size)), 4u);
        origin[a] = -0.5f * static_cast<float>(res[a]) * voxel_size;
    }
    assets::MeshSdfAsset sdf;
    sdf.grid_origin = {origin[0], origin[1], origin[2]};
    sdf.voxel_size = voxel_size;
    sdf.resolution = {res[0], res[1], res[2]};
    sdf.local_bounds =
        assets::Aabb{core::Vec3{-half_extents.x, -half_extents.y, -half_extents.z}, half_extents};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t kz = 0; kz < res[2]; ++kz) {
        for (std::uint32_t jy = 0; jy < res[1]; ++jy) {
            for (std::uint32_t ix = 0; ix < res[0]; ++ix) {
                const core::Vec3 p{sdf.grid_origin.x + (static_cast<float>(ix) + 0.5f) * voxel_size,
                                   sdf.grid_origin.y + (static_cast<float>(jy) + 0.5f) * voxel_size,
                                   sdf.grid_origin.z +
                                       (static_cast<float>(kz) + 0.5f) * voxel_size};
                const float d = analytic_box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

// A large, thin slab well inside clipmap level 0's 8 m extent: an open floor with nothing above it
// (mirrors the m10.4c spike's own floor piece).
void compose_open_floor(SdfClipmap& clipmap) {
    clipmap.update_instance(
        1, build_box_sdf({3.5f, 0.3f, 3.5f}), core::mat4_translation({0.0f, -1.3f, 0.0f}));
}

// A fully sealed ~2 m cube around the origin: 6 overlapping slabs (floor/ceiling/4 walls), each
// extending 0.15 m past the shared cavity boundary so no edge/corner leaks light through — the
// same "several separate instances, generous overlap" construction the spike's room uses.
void compose_sealed_box(SdfClipmap& clipmap) {
    std::uint64_t id = 1;
    const auto place = [&](core::Vec3 half, core::Vec3 at) {
        clipmap.update_instance(id++, build_box_sdf(half), core::mat4_translation(at));
    };
    place({1.15f, 0.15f, 1.15f}, {0.0f, -1.15f, 0.0f}); // floor: inner face y=-1.0
    place({1.15f, 0.15f, 1.15f}, {0.0f, 1.15f, 0.0f});  // ceiling: inner face y=+1.0
    place({0.15f, 1.15f, 1.15f}, {-1.15f, 0.0f, 0.0f}); // -x wall: inner face x=-1.0
    place({0.15f, 1.15f, 1.15f}, {1.15f, 0.0f, 0.0f});  // +x wall: inner face x=+1.0
    place({1.15f, 1.15f, 0.15f}, {0.0f, 0.0f, -1.15f}); // -z wall: inner face z=-1.0
    place({1.15f, 1.15f, 0.15f}, {0.0f, 0.0f, 1.15f});  // +z wall: inner face z=+1.0
}

// Declare-execute-submit one (SdfClipmap + DdgiProbes) update and hand back DdgiProbes' stats.
DdgiStats step(rhi::Device& device,
               SdfClipmap& clipmap,
               DdgiProbes& ddgi,
               core::Vec3 camera_pos,
               const DdgiLightingInputs& lighting,
               const LightingSettings& settings) {
    RenderGraph graph(device);
    graph.reset();
    clipmap.add(graph, camera_pos);
    ddgi.add(graph, clipmap, camera_pos, lighting, settings);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return ddgi.stats();
}

// Read the irradiance atlas back to CPU linear RGB (reuses render_test_support.hpp's RGBA16Float
// decode — the exact format DdgiProbes stores).
test::HdrImage read_irradiance(rhi::Device& device, const DdgiProbes& ddgi) {
    const rhi::Extent2D ext = ddgi.irradiance_atlas_extent();
    const std::vector<std::uint8_t> bytes =
        test::read_texture(device, ddgi.irradiance_atlas(), ext.width, ext.height, 8);
    return test::decode_hdr(bytes, ext.width, ext.height);
}

// A single-probe (1x1x1) lattice's own tile sits at atlas origin (0,0) — `ddgi_atlas_tile(0, 1)`
// gives (0,0) directly, so this is just the world-direction -> texel-within-tile half of that.
// `oct_encode`'s UV -> texel INDEX inverse (the test-only half of the round trip; production code
// never needs to invert it, only decode it — ddgi_blend_irradiance.comp).
int uv_to_texel(float coord, int interior) {
    const float texel_f = (coord + 1.0f) * 0.5f * static_cast<float>(interior) - 0.5f;
    const int texel = static_cast<int>(std::lround(texel_f)) + 1; // +1: skip the border column/row
    return std::clamp(texel, 1, interior);
}

} // namespace

TEST_CASE("ddgi: an open sunlit probe accumulates irradiance; a sealed probe stays dark (m10.5a)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the DDGI physical-sanity proof");
        return;
    }

    LightingSettings settings{};
    settings.ddgi_probe_count_x = settings.ddgi_probe_count_y = settings.ddgi_probe_count_z = 1;
    settings.ddgi_rays_per_probe = 64;
    settings.ddgi_hysteresis = 0.7f; // a handful of updates settles well past its first snap
    settings.ddgi_albedo = 0.6f;

    DdgiLightingInputs lighting{};
    lighting.has_sun = true;
    lighting.sun_direction = {0.0f, -1.0f, 0.0f}; // straight down
    lighting.sun_radiance[0] = lighting.sun_radiance[1] = lighting.sun_radiance[2] = 3.0f;
    lighting.sky_radiance[0] = lighting.sky_radiance[1] = lighting.sky_radiance[2] = 0.02f;

    // (a) Open floor: a single probe at the origin, 1 m above a large sunlit slab.
    {
        SdfClipmap clipmap(*device);
        DdgiProbes ddgi(*device);
        compose_open_floor(clipmap);
        for (int i = 0; i < 10; ++i)
            (void)step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);

        const test::HdrImage img = read_irradiance(*device, ddgi);
        float peak = 0.0f;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x)
                peak = std::max(peak, img.luminance(x, y));

        // The brightest texel is a weighted average over rays that mostly hit the floor's top face
        // (normal (0,1,0), NdotL=1 against a straight-down sun) — the theoretical CEILING for any
        // texel is albedo*radiance = 0.6*3.0 = 1.8 (a ray that hit dead-on with lit=1, weighted at
        // 100%); a cosine-weighted AVERAGE over many rays is necessarily below that ceiling. The
        // sky-only floor (a probe that saw nothing but ambient) would be ~0.02. 0.05 sits well
        // above any plausible noise/sky floor and well below the physical ceiling — the margin is
        // "clearly a lit surface, not sky", not a tight numerical match.
        MESSAGE("open floor: brightest texel luminance = " << peak);
        CHECK(peak > 0.05);
        CHECK(peak < 1.8 * 1.1); // sanity: never exceeds the physical ceiling by more than slack
    }

    // (b) Sealed box: the SAME sun, but every ray's self-shadow trace must find a wall.
    {
        SdfClipmap clipmap(*device);
        DdgiProbes ddgi(*device);
        compose_sealed_box(clipmap);
        for (int i = 0; i < 10; ++i)
            (void)step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);

        const test::HdrImage img = read_irradiance(*device, ddgi);
        float peak = 0.0f;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x)
                peak = std::max(peak, img.luminance(x, y));

        // Every ray hits an interior wall within ~2 m and every hit's self-shadow trace must also
        // hit a wall (the box is sealed) — radiance is the exact product albedo*radiance*ndotl*lit
        // with lit=0, so this should be exactly 0.0 modulo floating-point round-off. 0.01 is a
        // generous margin against any voxelization edge case (a near-corner ray sampling right at
        // two overlapping slabs' shared boundary) while still being two orders of magnitude below
        // the open floor's own 0.05 CHECK above — "dark", not "merely dimmer".
        MESSAGE("sealed box: brightest texel luminance = " << peak);
        CHECK(peak < 0.01);
    }
}

TEST_CASE("ddgi: the down-facing texel is materially brighter than the up-facing one, and the "
          "border agrees with its interior neighbour (m10.5a)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the DDGI directionality proof");
        return;
    }

    LightingSettings settings{};
    settings.ddgi_probe_count_x = settings.ddgi_probe_count_y = settings.ddgi_probe_count_z = 1;
    settings.ddgi_rays_per_probe = 64;
    settings.ddgi_hysteresis = 0.7f;
    settings.ddgi_albedo = 0.6f;

    DdgiLightingInputs lighting{};
    lighting.has_sun = true;
    lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    lighting.sun_radiance[0] = lighting.sun_radiance[1] = lighting.sun_radiance[2] = 3.0f;
    lighting.sky_radiance[0] = lighting.sky_radiance[1] = lighting.sky_radiance[2] = 0.02f;

    SdfClipmap clipmap(*device);
    DdgiProbes ddgi(*device);
    compose_open_floor(clipmap);
    for (int i = 0; i < 10; ++i)
        (void)step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);

    const test::HdrImage img = read_irradiance(*device, ddgi);

    // Single-probe (1x1x1) lattice: probe 0's tile sits at atlas-tile (0,0), i.e. texel origin
    // (0,0) directly (ddgi_atlas_tile(0, 1) == {0,0}) — no offset math needed beyond the
    // within-tile texel index.
    const core::Vec2 down_uv = ddgi_oct_encode({0.0f, -1.0f, 0.0f});
    const core::Vec2 up_uv = ddgi_oct_encode({0.0f, 1.0f, 0.0f});
    const int down_x = uv_to_texel(down_uv.x, kDdgiIrradianceTileInterior);
    const int down_y = uv_to_texel(down_uv.y, kDdgiIrradianceTileInterior);
    const int up_x = uv_to_texel(up_uv.x, kDdgiIrradianceTileInterior);
    const int up_y = uv_to_texel(up_uv.y, kDdgiIrradianceTileInterior);

    const float down_lum =
        img.luminance(static_cast<std::uint32_t>(down_x), static_cast<std::uint32_t>(down_y));
    const float up_lum =
        img.luminance(static_cast<std::uint32_t>(up_x), static_cast<std::uint32_t>(up_y));
    MESSAGE("down texel (" << down_x << "," << down_y << ") luminance=" << down_lum
                           << "; up texel (" << up_x << "," << up_y << ") luminance=" << up_lum);

    // Down sees the sunlit floor (NdotL up to 1.0 against a straight-down sun); up sees nothing but
    // the flat sky term (0.02). 5x is comfortably beyond any ray-sampling noise at 64 rays/update
    // while being far below the ~dozens-of-x gap the actual numbers above (0.05 vs ~0.02) suggest —
    // a property of the DIRECTIONS, not a coincidence of this one scene's brightness knobs.
    CHECK(down_lum > up_lum * 5.0);
    CHECK(down_lum > 0.03); // also clearly above the sky floor in absolute terms

    // THE BORDER. (0,-1,0) encodes to the octahedral square's bottom EDGE MIDPOINT (u=0,v=-1) — a
    // real, non-corner case with a border neighbour one row further out (py=0, vs the interior
    // texel's py=1). ddgi_blend_irradiance.comp's oct_decode_folded must make the two agree (the
    // whole point of folding rather than copying): assert they are close, not just both bright.
    REQUIRE(down_y == 1); // confirms this scene actually exercises the edge case the comment claims
    const float border_lum =
        img.luminance(static_cast<std::uint32_t>(down_x), 0u); // py=0: the border row
    MESSAGE("down-adjacent border texel (" << down_x << ",0) luminance=" << border_lum);
    // Both are noisy 64-ray estimates of the SAME underlying direction (bottom-edge texels and
    // their border neighbour decode to directions that differ only by the fold's exact placement,
    // a few degrees at most) — 25% relative agreement is generous slack for independent per-texel
    // ray-weighting noise while still catching a genuinely wrong fold (which would show the border
    // reading something unrelated, e.g. close to the UP value or to zero).
    CHECK(std::fabs(border_lum - down_lum) < 0.25 * std::max(down_lum, 0.03f));
}

TEST_CASE("ddgi: irradiance decays geometrically at the configured hysteresis rate once the light "
          "is gone, and settles near zero (m10.5a)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the DDGI temporal-convergence proof");
        return;
    }

    // Why a "light removed" step response, not a running average's frame-to-frame delta: an EMA
    // fed a CONSTANT-noise stream (each update's 64-ray estimate has roughly the same variance,
    // rotation after rotation) does not decay its own step-to-step delta toward zero — it decays
    // toward a STEADY-STATE noise floor whose variance is only ever a bounded fraction
    // (1/(1+hysteresis), never below 1/2) of the initial transient's, which a first draft of this
    // test learned the hard way (it asserted a 4x drop that the algebra of the recurrence
    // stored_n = h*stored_{n-1} + (1-h)*new_n cannot deliver for any h < 1 — a flawed TEST, not a
    // flawed blend). What DOES decay cleanly, deterministically, and is exactly what
    // docs/math/ddgi.md §5 means by "hysteresis costs latency": the influence of the FIRST sample
    // on the running value, which the recurrence's own closed form makes an EXACT h^n — so this
    // test creates a real step change (the sun disappears) and checks the response tracks h^n.
    LightingSettings settings{};
    settings.ddgi_probe_count_x = settings.ddgi_probe_count_y = settings.ddgi_probe_count_z = 1;
    settings.ddgi_rays_per_probe = 64;
    settings.ddgi_hysteresis = 0.8f;
    settings.ddgi_albedo = 0.6f;

    DdgiLightingInputs lit_on{};
    lit_on.has_sun = true;
    lit_on.sun_direction = {0.0f, -1.0f, 0.0f};
    lit_on.sun_radiance[0] = lit_on.sun_radiance[1] = lit_on.sun_radiance[2] = 3.0f;
    lit_on.sky_radiance[0] = lit_on.sky_radiance[1] = lit_on.sky_radiance[2] = 0.02f;

    // has_sun defaults false (sun_radiance is never even copied — see DdgiProbes::add) and the sky
    // term is explicitly zeroed too, so EVERY ray this update contributes EXACTLY (0,0,0): a hit's
    // direct term is albedo*0*ndotl*lit = 0, a miss's sky term is the zeroed sky_radiance. That
    // makes new_estimate = 0/weight_sum = 0 to the bit (no division-by-noise, no ray-sampling
    // variance at all) — the cleanest possible instrument for the recurrence's OWN math, with zero
    // help from margins papering over stochastic noise.
    DdgiLightingInputs lit_off{};

    SdfClipmap clipmap(*device);
    DdgiProbes ddgi(*device);
    compose_open_floor(clipmap);

    const auto read_peak = [&]() {
        const test::HdrImage img = read_irradiance(*device, ddgi);
        float peak = 0.0f;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x)
                peak = std::max(peak, img.luminance(x, y));
        return peak;
    };

    // Prime with the sun on: the first-ever update snaps straight to its own (noisy but bright)
    // estimate, hysteresis forced to 0 (DdgiStats::newly_primed) — see test 1 for why this lands
    // well above 0.05.
    const DdgiStats primed = step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lit_on, settings);
    REQUIRE(primed.newly_primed == 1);
    const float initial = read_peak();
    MESSAGE("initial (lit) peak = " << initial);
    REQUIRE(initial > 0.05f);

    // Now the light is gone. Since every subsequent new_estimate is exactly 0, mix(0, old, h) = h *
    // old — an EXACT geometric decay with no sampling noise to obscure it.
    constexpr int kUpdates = 20;
    std::vector<float> values;
    values.reserve(kUpdates);
    for (int i = 0; i < kUpdates; ++i) {
        const DdgiStats s = step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lit_off, settings);
        REQUIRE(s.newly_primed == 0); // still the same primed probe — hysteresis actually applies
        values.push_back(read_peak());
    }

    // Strictly non-increasing (h*old <= old for old >= 0, h in [0,1)) — allow a hair of slack for
    // fp16 atlas-storage rounding (irradiance is RGBA16Float; each mix() is quantized on write).
    for (std::size_t i = 1; i < values.size(); ++i) {
        CHECK(values[i] <= values[i - 1] * 1.01f);
    }

    // The decay rate matches hysteresis: after N updates the value should sit near initial*h^N.
    // 15% relative tolerance covers fp16 quantization compounding over 10 multiplications plus the
    // one place real (if tiny) noise can still enter — a probe that shifts which texel is the
    // image-wide MAX from step to step, right as several near-tied bright texels all decay in
    // lockstep. This is still a tight, meaningful check: a wrong hysteresis constant or a blend
    // formula bug would miss it by far more than 15%.
    const float expected_10 = initial * std::pow(0.8f, 10);
    MESSAGE("after 10 dark updates: " << values[9] << " (expected ~" << expected_10 << ")");
    CHECK(std::fabs(values[9] - expected_10) < 0.15f * std::max(expected_10, 0.001f));

    // And it settles near-fully dark: 0.8^20 ≈ 0.0115 of the initial value.
    MESSAGE("after 20 dark updates: " << values.back() << " (initial was " << initial << ")");
    CHECK(values.back() < initial * 0.03f);
}

TEST_CASE("ddgi: sub-spacing camera motion does not reshuffle the lattice (m10.5a)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the DDGI snap-stability proof");
        return;
    }

    LightingSettings settings{};
    settings.ddgi_probe_count_x = settings.ddgi_probe_count_y = settings.ddgi_probe_count_z = 4;
    settings.ddgi_probe_spacing = 1.0f;
    settings.ddgi_rays_per_probe = 32; // this test only cares about lattice bookkeeping, not light

    DdgiLightingInputs lighting{}; // no sun needed — nothing here reads atlas contents

    SdfClipmap clipmap(*device);
    DdgiProbes ddgi(*device);

    const DdgiStats warm = step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);
    CHECK(warm.grid_shifted);       // the first-ever call is always a "shift" (nothing primed yet)
    CHECK(warm.newly_primed == 64); // 4^3 probes, all new
    const core::Vec3 origin0 = ddgi.origin();

    // Settle: same camera position again — no shift, and (since the whole 64-probe grid fits under
    // kMaxDdgiProbesPerUpdate=512) every probe is already primed from the warmup call above.
    const DdgiStats settled = step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);
    CHECK_FALSE(settled.grid_shifted);
    CHECK(settled.newly_primed == 0);

    // (a) Nudge WELL under the 1 m probe spacing: zero reshuffling anywhere, origin bit-identical —
    // the anti-shimmer property SdfClipmap's own levels already prove for their voxel grid,
    // applied here to the probe lattice.
    const DdgiStats nudged = step(*device, clipmap, ddgi, {0.05f, 0.0f, 0.0f}, lighting, settings);
    CHECK_FALSE(nudged.grid_shifted);
    CHECK(nudged.newly_primed == 0);
    CHECK(ddgi.origin().x == origin0.x); // bit-identical, not merely close

    // Back to the exact position the origin above was measured against.
    (void)step(*device, clipmap, ddgi, {0.0f, 0.0f, 0.0f}, lighting, settings);

    // (b) A big jump — past the lattice's own multi-metre extent — forces a shift: every probe's
    // primed history resets (a fresh area of the world was scrolled into view, so its probes have
    // never been traced), and the origin actually moves.
    const DdgiStats jumped = step(*device, clipmap, ddgi, {50.0f, 0.0f, 0.0f}, lighting, settings);
    CHECK(jumped.grid_shifted);
    CHECK(jumped.newly_primed == 64);
    CHECK(ddgi.origin().x != origin0.x);
}
