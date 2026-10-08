// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The split-sum sky specular -- the structural proof (ADR-0078 section 2). No golden images: every
// claim is a property the mathematics guarantees, checked with a stated margin on lavapipe, the
// M5.6/M6.4 pattern.
//
// WHAT IS PROVEN, and what each proof is able to see:
//
//   1. The DFG table is a physical quantity. Its two terms stay in [0, 1], their sum -- the
//      fraction of a white sky a perfect reflector returns -- never exceeds 1 (the surface cannot
//      create light) and shrinks as roughness grows (single-bounce microfacets lose energy), and
//      it AGREES with an independent brute-force quadrature of the same BRDF integral. The
//      quadrature is a different algorithm (a dense (theta, phi) midpoint grid in double precision
//      vs 512 GPU importance samples), so agreement is two independent measurements, not two
//      opinions.
//   2. White furnace. A constant sky filters to itself at every roughness, and a white metal
//      (f0 = 1) under it returns that radiance: nothing gained, nothing lost. This runs through
//      the ENGINE's own GLSL lookup (tests/render/shaders/sky_specular_probe.comp includes
//      sky_specular_eval.glsl), not a CPU copy. The f0 = 1 equality is true BY CONSTRUCTION of the
//      energy compensation; what carries evidence is the rest of the file's furnace block --
//      compensated reflectance never exceeds 1 anywhere on the (f0, n.v, roughness) domain, and
//      the UNcompensated loss is measured, to show the compensation is correcting something real.
//   3. A bright spot in the sky: the filtered peak falls strictly as roughness rises (the lobe
//      widens), the total flux over the sphere is conserved (the filter redistributes light, never
//      adds or removes it), and the GPU values agree with an independent CPU convolution of the
//      same analytic sky.
//   4. Continuity across level boundaries: a roughness an epsilon either side of a level boundary
//      reads the same value, and at exactly k/6 the lookup returns array layer k-1 -- so the
//      roughness->level mapping has no off-by-one and no seam.
//   5. Counters: every rejected sample, every re-use and every disabled frame is counted, and the
//      tests assert on the counts.
//   6. Wired in: through the whole renderer a smooth metal reads a sharp sky and a rough one a
//      blurred one, the flag really switches the path, and "off" declares no pass.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/sky.hpp"
#include "rime/render/lighting/sky_specular.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "sky_specular_probe.comp.spv.h"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::half_to_float;
using rime::render::test::HdrImage;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

[[nodiscard]] Vec3d operator+(Vec3d a, Vec3d b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3d operator*(Vec3d a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}

[[nodiscard]] double dot(Vec3d a, Vec3d b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] Vec3d normalize(Vec3d a) {
    const double inv = 1.0 / std::sqrt(dot(a, a));
    return a * inv;
}

using Rgb = std::array<double, 3>;
using SkyFn = std::function<Rgb(Vec3d)>;

// ── The sky-view LUT's parameterisation, restated on the CPU (sky_mapping.glsl) ────────────────
// Written out here rather than shared, because the test must catch a drift between the shader's
// mapping and what the CPU believes; a shared copy would drift together.
[[nodiscard]] Vec3d direction_from_uv(double u, double v) {
    const double t = 2.0 * v - 1.0;
    const double l = (t < 0 ? -1.0 : 1.0) * t * t * (kPi * 0.5);
    const double a = (u - 0.5) * 2.0 * kPi;
    const double cl = std::cos(l);
    return {cl * std::cos(a), std::sin(l), cl * std::sin(a)};
}

[[nodiscard]] std::array<double, 2> uv_from_direction(Vec3d d) {
    d = normalize(d);
    const double l = std::asin(std::clamp(d.y, -1.0, 1.0));
    const double t = (l < 0 ? -1.0 : 1.0) * std::sqrt(std::abs(l) / (kPi * 0.5));
    const double a = std::atan2(d.z, d.x);
    return {a / (2.0 * kPi) + 0.5, 0.5 + 0.5 * t};
}

// The solid angle of the texel at (x, y) of a W x H table in this parameterisation: the area
// element of the sphere cos(el) d(el) d(az), with el = (pi/2) t |t| and t = 2v - 1.
[[nodiscard]] double texel_solid_angle(std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const double v = (static_cast<double>(y) + 0.5) / h;
    const double t = 2.0 * v - 1.0;
    const double el = (t < 0 ? -1.0 : 1.0) * t * t * (kPi * 0.5);
    const double del_dv = kPi * std::abs(t) * 2.0; // d(el)/dv
    return std::cos(el) * (2.0 * kPi / w) * del_dv / h;
}

[[nodiscard]] std::uint16_t float_to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10)
            return static_cast<std::uint16_t>(sign);
        mant = (mant | 0x800000u) >> (1 - exp);
        return static_cast<std::uint16_t>(sign | ((mant + 0x1000u) >> 13));
    }
    if (exp >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    // Round to nearest even on the dropped 13 bits.
    std::uint32_t h = sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
        ++h;
    return static_cast<std::uint16_t>(h);
}

// The LUT's size, as SkyPass creates it. The source the rig hands SkySpecular has the same shape.
constexpr std::uint32_t kSrcW = 192;
constexpr std::uint32_t kSrcH = 108;
constexpr double kLevels = kSkySpecularLevels;

// ── GGX, restated in double precision (brdf.glsl) ────────────────────────────────────────────────
[[nodiscard]] double d_ggx(double n_dot_h, double alpha) {
    const double a2 = alpha * alpha;
    const double t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * t * t);
}

[[nodiscard]] double v_smith(double n_dot_v, double n_dot_l, double alpha) {
    const double a2 = alpha * alpha;
    const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / std::max(gv + gl, 1e-12);
}

// The DFG integrals by DENSE QUADRATURE -- the independent measurement. Midpoint rule over the
// whole hemisphere of l in (theta, phi). Only valid where the lobe is wider than the grid, hence
// the caller restricts it to rough-enough rows. The grid size is a MEASURED choice: at grazing n.v
// the lobe is a thin sliver and a 1024 x 512 grid was 4-5% off (B = 0.392 against a converged
// 0.411); 2048 x 2048 agrees with 4096 x 2048 and 8192 x 4096 to better than 1e-4 (checked with an
// independent numpy implementation while writing this test), so that is the floor used here.
[[nodiscard]] std::array<double, 2> dfg_by_quadrature(double n_dot_v, double roughness) {
    const double alpha = roughness * roughness;
    const Vec3d v{std::sqrt(std::max(0.0, 1.0 - n_dot_v * n_dot_v)), 0.0, n_dot_v};
    constexpr int kTheta = 2048;
    constexpr int kPhi = 2048;
    double a = 0.0, b = 0.0;
    for (int i = 0; i < kTheta; ++i) {
        const double theta = (i + 0.5) / kTheta * (kPi * 0.5);
        const double s = std::sin(theta), c = std::cos(theta);
        for (int j = 0; j < kPhi; ++j) {
            const double phi = (j + 0.5) / kPhi * 2.0 * kPi;
            const Vec3d l{s * std::cos(phi), s * std::sin(phi), c};
            const Vec3d h = normalize(v + l);
            const double v_dot_h = dot(v, h);
            const double fc = std::pow(1.0 - v_dot_h, 5.0);
            const double integrand = d_ggx(h.z, alpha) * v_smith(n_dot_v, l.z, alpha) * l.z;
            const double dw = s * (kPi * 0.5 / kTheta) * (2.0 * kPi / kPhi);
            a += (1.0 - fc) * integrand * dw;
            b += fc * integrand * dw;
        }
    }
    return {a, b};
}

[[nodiscard]] double radical_inverse(std::uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

// The sky convolved with the GGX lobe about `n`, evaluated on the ANALYTIC sky function with many
// samples in double precision. The same estimator the shader uses (importance-sampled half vectors,
// n.l weights), re-implemented independently and fed the exact sky instead of a 192x108 table, so
// it measures the shader's sampling and texture-resolution error rather than repeating it.
[[nodiscard]] Rgb convolve_reference(const SkyFn& sky, Vec3d n, double roughness) {
    const double alpha = roughness * roughness;
    const Vec3d up = std::abs(n.z) < 0.999 ? Vec3d{0, 0, 1} : Vec3d{1, 0, 0};
    // tx = normalize(cross(up, n)), ty = cross(n, tx)
    const Vec3d cx{up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z, up.x * n.y - up.y * n.x};
    const Vec3d tx = normalize(cx);
    const Vec3d ty{n.y * tx.z - n.z * tx.y, n.z * tx.x - n.x * tx.z, n.x * tx.y - n.y * tx.x};
    constexpr std::uint32_t kN = 1u << 15;
    Rgb sum{0, 0, 0};
    double weight = 0.0;
    for (std::uint32_t i = 0; i < kN; ++i) {
        const double xi0 = (i + 0.5) / kN;
        const double xi1 = radical_inverse(i);
        const double phi = 2.0 * kPi * xi0;
        const double ct = std::sqrt((1.0 - xi1) / (1.0 + (alpha * alpha - 1.0) * xi1));
        const double st = std::sqrt(std::max(0.0, 1.0 - ct * ct));
        const Vec3d h = tx * (st * std::cos(phi)) + ty * (st * std::sin(phi)) + n * ct;
        const Vec3d l = h * (2.0 * dot(n, h)) + n * -1.0;
        const double nl = dot(n, l);
        if (nl > 0.0) {
            const Rgb c = sky(l);
            for (int k = 0; k < 3; ++k)
                sum[k] += c[k] * nl;
            weight += nl;
        }
    }
    return {sum[0] / weight, sum[1] / weight, sum[2] / weight};
}

// ── The rig: a device, a SkySpecular, a synthetic source sky, and the GPU probe ──────────────────
struct Query {
    Vec3d dir{0, 1, 0};
    float roughness = 0.0f;
    float f0 = 1.0f;
    float n_dot_v = 1.0f;
};

struct Answer {
    Rgb radiance{};
    Rgb brdf{};
};

class Rig {
public:
    explicit Rig(rhi::Device& device) : device_(device), spec_(device) {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Compute;
        sd.spirv = sky_specular_probe_comp_spv;
        sd.spirv_size_bytes = sizeof(sky_specular_probe_comp_spv);
        sd.debug_name = "sky_specular_probe.comp";
        probe_shader_ = device.create_shader(sd);
        const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
            {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
            {3, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = probe_shader_;
        pd.bindings = bindings;
        pd.debug_name = "sky-specular-probe";
        probe_pipeline_ = device.create_compute_pipeline(pd);
        rhi::SamplerDesc ss{};
        ss.mag_filter = rhi::Filter::Linear;
        ss.min_filter = rhi::Filter::Linear;
        ss.address_mode = rhi::AddressMode::ClampToEdge;
        ss.debug_name = "sky-specular-probe-dfg-sampler";
        dfg_sampler_ = device.create_sampler(ss);
    }

    ~Rig() {
        if (source_.is_valid())
            device_.destroy(source_);
        device_.destroy(dfg_sampler_);
        device_.destroy(probe_pipeline_);
        device_.destroy(probe_shader_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    SkySpecular& spec() { return spec_; }

    // Upload `sky` sampled at every texel CENTRE of a kSrcW x kSrcH table.
    void set_sky(const SkyFn& sky) {
        if (source_.is_valid())
            device_.destroy(source_);
        rhi::TextureDesc td{};
        td.extent = {kSrcW, kSrcH};
        td.format = rhi::Format::RGBA16Float;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        td.debug_name = "sky-specular-test-source";
        source_ = device_.create_texture(td);
        std::vector<std::uint16_t> texels(static_cast<std::size_t>(kSrcW) * kSrcH * 4);
        source_flux_ = {0, 0, 0};
        for (std::uint32_t y = 0; y < kSrcH; ++y) {
            for (std::uint32_t x = 0; x < kSrcW; ++x) {
                const Rgb c = sky(direction_from_uv((x + 0.5) / kSrcW, (y + 0.5) / kSrcH));
                const double dw = texel_solid_angle(y, kSrcW, kSrcH);
                for (int k = 0; k < 3; ++k) {
                    texels[(static_cast<std::size_t>(y) * kSrcW + x) * 4 + k] =
                        float_to_half(static_cast<float>(c[k]));
                    source_flux_[k] += c[k] * dw;
                }
                texels[(static_cast<std::size_t>(y) * kSrcW + x) * 4 + 3] = float_to_half(1.0f);
            }
        }
        device_.write_texture(source_, texels.data(), texels.size() * sizeof(std::uint16_t));
    }

    [[nodiscard]] const Rgb& source_flux() const { return source_flux_; }

    // One frame: declare the bakes and run them.
    // Returns the GPU time of the whole frame in milliseconds (timestamp-bracketed, so upload,
    // submit and readback stay out of it), or 0 where the device has no timestamps. Reported by the
    // tests, never asserted: a time is a property of one GPU, not of the code.
    double bake(bool source_changed) {
        RenderGraph graph(device_);
        graph.reset();
        const RGTexture src = graph.import_texture(source_, rhi::ResourceState::ShaderRead);
        const SkySpecularBinding b = spec_.add(graph, src, source_changed);
        graph.export_texture(b.prefiltered);
        graph.export_texture(b.dfg);
        auto cmd = device_.begin_commands();
        cmd->write_timestamp(0);
        graph.execute(*cmd);
        cmd->write_timestamp(1);
        device_.submit_blocking(*cmd);
        std::array<std::uint64_t, 2> stamps{};
        return cmd->read_timestamps(stamps) ? static_cast<double>(stamps[1] - stamps[0]) / 1e6
                                            : 0.0;
    }

    // Layer `layer` of the chain as floats, 4 per texel (rgb + the acceptance counter).
    [[nodiscard]] std::vector<float> read_layer(std::uint32_t layer) {
        const auto bytes = read_texture(
            device_, spec_.prefiltered(), kSkySpecularWidth, kSkySpecularHeight, 8, layer);
        return_to_shader_read(spec_.prefiltered());
        return halves(bytes);
    }

    [[nodiscard]] std::vector<float> read_dfg() {
        const auto bytes =
            read_texture(device_, spec_.dfg(), kSkySpecularDfgSize, kSkySpecularDfgSize, 8);
        return_to_shader_read(spec_.dfg());
        return halves(bytes);
    }

    // Run the engine's own GLSL lookups on a batch.
    [[nodiscard]] std::vector<Answer> probe(const std::vector<Query>& queries) {
        std::vector<float> q(queries.size() * 8);
        for (std::size_t i = 0; i < queries.size(); ++i) {
            q[i * 8 + 0] = static_cast<float>(queries[i].dir.x);
            q[i * 8 + 1] = static_cast<float>(queries[i].dir.y);
            q[i * 8 + 2] = static_cast<float>(queries[i].dir.z);
            q[i * 8 + 3] = queries[i].roughness;
            q[i * 8 + 4] = queries[i].f0;
            q[i * 8 + 5] = queries[i].n_dot_v;
        }
        rhi::BufferDesc qb{};
        qb.size = q.size() * sizeof(float);
        qb.usage = rhi::BufferUsage::Storage;
        qb.memory = rhi::MemoryUsage::CpuToGpu;
        qb.debug_name = "sky-specular-probe-queries";
        const rhi::BufferHandle queries_buf = device_.create_buffer(qb);
        device_.write_buffer(queries_buf, q.data(), qb.size);
        rhi::BufferDesc rb{};
        rb.size = q.size() * sizeof(float);
        rb.usage = rhi::BufferUsage::Storage;
        rb.memory = rhi::MemoryUsage::GpuToCpu;
        rb.debug_name = "sky-specular-probe-results";
        const rhi::BufferHandle results_buf = device_.create_buffer(rb);

        auto cmd = device_.begin_commands();
        cmd->bind_compute_pipeline(probe_pipeline_);
        cmd->bind_storage_buffer(0, queries_buf);
        cmd->bind_storage_buffer(1, results_buf);
        cmd->bind_texture(3, source_, spec_.prefiltered_sampler());
        cmd->bind_texture(4, spec_.prefiltered(), spec_.prefiltered_sampler());
        cmd->bind_texture(5, spec_.dfg(), dfg_sampler_);
        cmd->dispatch(static_cast<std::uint32_t>((queries.size() + 31) / 32), 1, 1);
        device_.submit_blocking(*cmd);
        spec_.note_prefiltered_state(rhi::ResourceState::ShaderRead);
        spec_.note_dfg_state(rhi::ResourceState::ShaderRead);

        std::vector<float> r(q.size());
        device_.read_buffer(results_buf, r.data(), rb.size, 0);
        device_.destroy(results_buf);
        device_.destroy(queries_buf);
        std::vector<Answer> out(queries.size());
        for (std::size_t i = 0; i < queries.size(); ++i) {
            for (int k = 0; k < 3; ++k) {
                out[i].radiance[k] = r[i * 8 + k];
                out[i].brdf[k] = r[i * 8 + 4 + k];
            }
        }
        return out;
    }

private:
    // A readback leaves the image in TRANSFER_SRC; the probe's bind_texture assumes SHADER_READ
    // (the graph owns barriers in the engine), so hand it back explicitly and tell the owner.
    void return_to_shader_read(rhi::TextureHandle t) {
        auto cmd = device_.begin_commands();
        cmd->texture_barrier(t, rhi::ResourceState::TransferSrc, rhi::ResourceState::ShaderRead);
        device_.submit_blocking(*cmd);
        spec_.note_prefiltered_state(rhi::ResourceState::ShaderRead);
        spec_.note_dfg_state(rhi::ResourceState::ShaderRead);
    }

    [[nodiscard]] static std::vector<float> halves(const std::vector<std::uint8_t>& bytes) {
        const auto* h = reinterpret_cast<const std::uint16_t*>(bytes.data());
        std::vector<float> out(bytes.size() / 2);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = half_to_float(h[i]);
        return out;
    }

    rhi::Device& device_;
    SkySpecular spec_;
    rhi::ShaderHandle probe_shader_;
    rhi::PipelineHandle probe_pipeline_;
    rhi::SamplerHandle dfg_sampler_;
    rhi::TextureHandle source_;
    Rgb source_flux_{};
};

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

// A sky with one narrow bright feature on a dim, mildly graded background. The feature is a
// cosine-power lobe (sigma ~9 degrees: ~5 texels of the 192x108 source, so the table
// can actually represent it -- a sharper feature would test the table's resolution, not the filter)
// in a direction well clear of the poles and the azimuth seam.
const Vec3d kSpot =
    normalize({std::cos(0.55) * std::cos(0.7), std::sin(0.55), std::cos(0.55) * std::sin(0.7)});

[[nodiscard]] Rgb spot_sky(Vec3d d) {
    const double lobe = std::pow(std::max(0.0, dot(normalize(d), kSpot)), 40.0);
    const double base = 0.15 + 0.1 * (0.5 + 0.5 * normalize(d).y);
    return {base + 40.0 * lobe, base + 36.0 * lobe, base + 30.0 * lobe};
}

[[nodiscard]] double rel(double a, double b) {
    return std::abs(a - b) / std::max(std::abs(b), 1e-9);
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE(
    "sky specular: the DFG table is a physical quantity and matches an independent quadrature") {
    auto device = device_or_skip("the DFG proof");
    if (!device)
        return;
    Rig rig(*device);
    rig.set_sky([](Vec3d) { return Rgb{1, 1, 1}; });
    rig.bake(true);
    const std::vector<float> dfg = rig.read_dfg();
    constexpr std::uint32_t N = kSkySpecularDfgSize;

    // (a) Range and the acceptance counter. A, B in [0,1]; and every texel accepted SOME samples --
    // a texel with zero accepted would be an integral computed from nothing, reading as 0.
    double min_accept = 1.0, max_e = 0.0, min_a = 1.0, min_b = 1.0, max_a = 0.0, max_b = 0.0;
    std::uint32_t nonfinite = 0, zero_accept = 0;
    for (std::uint32_t y = 0; y < N; ++y) {
        for (std::uint32_t x = 0; x < N; ++x) {
            const float* t = &dfg[(static_cast<std::size_t>(y) * N + x) * 4];
            if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]))
                ++nonfinite;
            if (t[2] <= 0.0f)
                ++zero_accept;
            min_accept = std::min<double>(min_accept, t[2]);
            min_a = std::min<double>(min_a, t[0]);
            min_b = std::min<double>(min_b, t[1]);
            max_a = std::max<double>(max_a, t[0]);
            max_b = std::max<double>(max_b, t[1]);
            max_e = std::max<double>(max_e, t[0] + t[1]);
        }
    }
    MESSAGE("DFG: A in [" << min_a << ", " << max_a << "], B in [" << min_b << ", " << max_b
                          << "], max A+B=" << max_e << ", min accepted fraction=" << min_accept);
    CHECK(nonfinite == 0);
    CHECK(zero_accept == 0);
    CHECK(min_a >= 0.0);
    CHECK(min_b >= 0.0);
    // Margin 0.002: fp16 storage rounds each of A and B by <= 2^-11 relative (4.9e-4 at 1.0), so
    // their sum can overshoot a true 1.0 by ~1e-3 (measured 1.00002). A table that CREATED light --
    // a wrong Jacobian, a doubled Fresnel term -- is off by whole percent, not by a rounding step.
    CHECK(max_a <= 1.0 + 0.002);
    CHECK(max_b <= 1.0 + 0.002);
    CHECK(max_e <= 1.0 + 0.002);

    // (b) Normal incidence, smooth: the scale term approaches 1, the bias approaches 0. The texel
    // at (n.v = 0.992, roughness = 0.0078) is the nearest the table has to the corner.
    const float* corner = &dfg[(static_cast<std::size_t>(0) * N + (N - 1)) * 4];
    MESSAGE("DFG corner (n.v=" << (N - 0.5) / N << ", r=" << 0.5 / N << "): A=" << corner[0]
                               << " B=" << corner[1]);
    CHECK(corner[0] > 0.98f);
    CHECK(corner[1] < 0.02f);

    // (c) Energy loss grows with roughness at fixed n.v. A single-bounce microfacet BRDF discards
    // the light that scatters between facets, and more facets are involved the rougher the surface.
    // The assertion is non-strict by a small slack for fp16 + sampling noise; what it rules out is
    // a table whose rows are the wrong way round or whose roughness axis is scrambled.
    for (std::uint32_t x : {N / 4, N / 2, 3 * N / 4}) {
        double prev = 2.0;
        for (std::uint32_t y = 0; y < N; ++y) {
            const float* t = &dfg[(static_cast<std::size_t>(y) * N + x) * 4];
            const double e = t[0] + t[1];
            CHECK(e <= prev + 0.004);
            prev = e;
        }
    }

    // (d) Agreement with the independent quadrature, on rows rough enough for the grid to resolve
    // the lobe (roughness >= 0.25: alpha >= 0.0625, a lobe ~8 degrees wide against a 0.09-degree
    // grid).
    double worst = 0.0;
    int compared = 0;
    for (std::uint32_t y : {16u, 32u, 48u, 63u}) {
        for (std::uint32_t x : {4u, 32u, 63u}) {
            const double nv = (x + 0.5) / N, r = (y + 0.5) / N;
            const auto ref = dfg_by_quadrature(nv, r);
            const float* t = &dfg[(static_cast<std::size_t>(y) * N + x) * 4];
            const double d = std::max(std::abs(t[0] - ref[0]), std::abs(t[1] - ref[1]));
            if (d > 0.004)
                MESSAGE("  DFG texel (" << x << "," << y << ") nv=" << nv << " r=" << r
                                        << ": gpu A,B=" << t[0] << "," << t[1]
                                        << " quad A,B=" << ref[0] << "," << ref[1]);
            worst = std::max(worst, d);
            ++compared;
        }
    }
    MESSAGE("DFG vs quadrature over " << compared << " texels: worst |dA|,|dB| = " << worst);
    // Margin 0.01 against a measured worst of 0.006, which is SAMPLING error at grazing n.v (the
    // largest term, A at n.v = 0.07, converges as ~1/N: 0.0057 at 512 samples, 0.0016 at 2048 in
    // the same texel before the 2048-sample bake was tried on the rougher texel above). fp16
    // rounding adds <= ~5e-4. The margin is a stated bound on the estimator, not a number tuned to
    // pass.
    CHECK(worst < 0.01);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE("sky specular: white furnace -- a constant sky filters to itself and a white metal "
          "returns it") {
    auto device = device_or_skip("the furnace proof");
    if (!device)
        return;
    Rig rig(*device);
    const Rgb sky_value{0.7, 0.5, 0.3};
    rig.set_sky([&](Vec3d) { return sky_value; });
    rig.bake(true);

    // (a) Every texel of every layer: finite, accepted > 0 (the skip counter), and equal to the
    // constant. Margin 2e-3 relative: the source is fp16 (rel error 4.9e-4), the filtered result is
    // fp16 again, and a weight-normalised average of equal numbers adds no more than that.
    double worst_rel = 0.0, min_accept = 1.0;
    std::uint32_t nonfinite = 0, fallbacks = 0;
    for (std::uint32_t layer = 0; layer < kSkySpecularLayers; ++layer) {
        const std::vector<float> t = rig.read_layer(layer);
        for (std::size_t i = 0; i < t.size() / 4; ++i) {
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(t[i * 4 + k]))
                    ++nonfinite;
                worst_rel = std::max(worst_rel, rel(t[i * 4 + k], sky_value[k]));
            }
            if (t[i * 4 + 3] <= 0.0f)
                ++fallbacks;
            min_accept = std::min<double>(min_accept, t[i * 4 + 3]);
        }
    }
    MESSAGE("furnace: worst relative deviation over all layers = "
            << worst_rel << ", min accepted fraction = " << min_accept
            << ", fallback texels = " << fallbacks);
    CHECK(nonfinite == 0);
    CHECK(fallbacks == 0); // the "every sample rejected" path was never taken
    CHECK(worst_rel < 2e-3);
    // The accepted fraction is the fraction of samples with n.l > 0. At roughness 1 about half the
    // half-vectors reflect v below the horizon, so this is a measured property, not 1.0: a value
    // near 0 would mean the filter is averaging almost nothing.
    CHECK(min_accept > 0.3);

    // (b) The ENGINE'S lookup, through the real GLSL, at roughness 0..1 and assorted directions.
    std::vector<Query> qs;
    for (float r : {0.0f, 0.05f, 0.1f, 0.25f, 0.4f, 0.5f, 0.66f, 0.8f, 0.95f, 1.0f})
        for (Vec3d d :
             {Vec3d{0, 1, 0}, Vec3d{1, 0, 0}, Vec3d{0.3, -0.6, 0.7}, Vec3d{-0.8, 0.1, -0.2}})
            qs.push_back({normalize(d), r, 1.0f, 0.6f});
    const std::vector<Answer> out = rig.probe(qs);
    double worst_lookup = 0.0, worst_furnace = 0.0;
    for (std::size_t i = 0; i < qs.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            worst_lookup = std::max(worst_lookup, rel(out[i].radiance[k], sky_value[k]));
            // f0 = 1 (a white metal): returned = prefiltered radiance * environment BRDF.
            worst_furnace =
                std::max(worst_furnace, rel(out[i].radiance[k] * out[i].brdf[k], sky_value[k]));
        }
    }
    MESSAGE("furnace through the GLSL lookup: worst prefiltered deviation = "
            << worst_lookup << ", worst white-metal deviation = " << worst_furnace);
    CHECK(worst_lookup < 2e-3);
    // Margin 4e-3: the lookup's 2e-3 plus the DFG's fp16 round-trip in 1/E. (At f0 = 1 the product
    // E * (1/E) is exactly 1 BY CONSTRUCTION; this line proves the wiring -- the table is read
    // with the right uv, not transposed, and the compensation is applied -- not the physics. The
    // physics is the two checks below.)
    CHECK(worst_furnace < 4e-3);

    // (c) NEVER ABOVE 1, anywhere. The compensated white-sky reflectance (f0 A + B) *
    // (1 + f0 (1/E - 1)) over a dense grid of f0, n.v and roughness, evaluated by the GLSL. A
    // surface under a uniform white sky cannot return more than the sky.
    std::vector<Query> grid;
    for (float f0 : {0.0f, 0.04f, 0.2f, 0.5f, 0.8f, 1.0f})
        for (float nv : {0.02f, 0.1f, 0.3f, 0.5f, 0.8f, 1.0f})
            for (float r : {0.02f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f})
                grid.push_back({Vec3d{0, 1, 0}, r, f0, nv});
    const std::vector<Answer> g = rig.probe(grid);
    double max_reflectance = 0.0;
    for (const Answer& a : g)
        max_reflectance = std::max(max_reflectance, a.brdf[0]);
    MESSAGE("compensated reflectance over "
            << grid.size() << " (f0, n.v, roughness) cases: max = " << max_reflectance);
    // Margin 0.002, the DFG table's own fp16 slack (measured max: exactly 1.0).
    CHECK(max_reflectance <= 1.0 + 0.002);

    // (d) The uncompensated loss, measured -- evidence that the compensation corrects something
    // real rather than being decoration. A white metal at roughness ~1: E = A + B straight from the
    // table.
    const std::vector<float> dfg = rig.read_dfg();
    constexpr std::uint32_t N = kSkySpecularDfgSize;
    const float* rough_mid = &dfg[(static_cast<std::size_t>(N - 1) * N + N / 2) * 4];
    const double e_rough = rough_mid[0] + rough_mid[1];
    MESSAGE("uncompensated single-bounce albedo at roughness ~1, n.v ~0.5: E = "
            << e_rough << " (energy lost: " << (1.0 - e_rough) * 100.0 << "%)");
    CHECK(e_rough < 0.9); // a real loss, well outside any rounding
    CHECK(e_rough > 0.3); // and not so large that the table is clearly broken
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE("sky specular: a bright spot widens monotonically, conserves flux, and matches a CPU "
          "convolution") {
    auto device = device_or_skip("the bright-spot proof");
    if (!device)
        return;
    Rig rig(*device);
    rig.set_sky(spot_sky);
    rig.bake(true);

    // (a) The peak of each level falls as roughness rises. Level 0 is the source itself, read
    // through the probe at roughness 0; levels 1..6 are read straight from the array layers.
    const auto peak_of = [](const std::vector<float>& t) {
        double p = 0.0;
        for (std::size_t i = 0; i < t.size() / 4; ++i)
            p = std::max<double>(p, t[i * 4]);
        return p;
    };
    std::vector<double> peaks;
    {
        std::vector<Query> q0;
        q0.push_back({kSpot, 0.0f, 1.0f, 1.0f});
        peaks.push_back(rig.probe(q0)[0].radiance[0]);
    }
    for (std::uint32_t layer = 0; layer < kSkySpecularLayers; ++layer)
        peaks.push_back(peak_of(rig.read_layer(layer)));
    for (std::size_t i = 0; i < peaks.size(); ++i)
        MESSAGE("CPU reference at the spot, roughness "
                << i / (kLevels - 1.0) << ": "
                << convolve_reference(spot_sky, kSpot, i / (kLevels - 1.0))[0]);
    for (std::size_t i = 0; i < peaks.size(); ++i)
        MESSAGE("level " << i << " (roughness " << i / (kLevels - 1.0) << ") peak = " << peaks[i]);
    for (std::size_t i = 1; i < peaks.size(); ++i)
        CHECK(peaks[i] < peaks[i - 1]); // strictly, with no slack: the lobes are nested

    // The same through the continuous lookup: a fine roughness sweep at the spot's direction must
    // also be strictly decreasing, which additionally proves the lerp between levels is monotone.
    std::vector<Query> sweep;
    for (int i = 0; i <= 40; ++i)
        sweep.push_back({kSpot, static_cast<float>(i) / 40.0f, 1.0f, 1.0f});
    const std::vector<Answer> s = rig.probe(sweep);
    for (std::size_t i = 1; i < s.size(); ++i)
        CHECK(s[i].radiance[0] < s[i - 1].radiance[0]);

    // (b) Flux conservation. The filter's kernel depends only on the angle between n and l, so it
    // is symmetric and every row sums to 1: it moves light around the sphere without adding or
    // removing any. The flux of each level (radiance x texel solid angle, summed) must equal the
    // source's. Margin 1% against a measured 0.12%: the residue is the midpoint quadrature on two
    // different grids (128x72 vs 192x108) plus fp16; a filter that leaked or invented energy (say
    // the n.l weights not normalised by the weights actually accepted) moves the flux by tens of %.
    const double flux0 = rig.source_flux()[0];
    double worst_flux = 0.0;
    for (std::uint32_t layer = 0; layer < kSkySpecularLayers; ++layer) {
        const std::vector<float> t = rig.read_layer(layer);
        double flux = 0.0;
        for (std::uint32_t y = 0; y < kSkySpecularHeight; ++y)
            for (std::uint32_t x = 0; x < kSkySpecularWidth; ++x)
                flux += t[(static_cast<std::size_t>(y) * kSkySpecularWidth + x) * 4] *
                        texel_solid_angle(y, kSkySpecularWidth, kSkySpecularHeight);
        worst_flux = std::max(worst_flux, rel(flux, flux0));
        MESSAGE("layer " << layer << ": flux " << flux << " vs source " << flux0);
    }
    MESSAGE("worst flux deviation over the chain = " << worst_flux * 100.0 << "%");
    CHECK(worst_flux < 0.01);

    // (c) Against the independent CPU convolution of the analytic sky, at the spot, at a point
    // 6 and 15 degrees off it, and on the opposite side of the sky -- at level centres AND between
    // them (where the lookup lerps two levels and the interpolation error shows).
    std::vector<Query> qs;
    std::vector<Vec3d> dirs;
    const Vec3d side = normalize({kSpot.x + 0.1, kSpot.y, kSpot.z});
    const Vec3d side2 = normalize({kSpot.x + 0.27, kSpot.y, kSpot.z});
    for (Vec3d d : {kSpot, side, side2, Vec3d{-kSpot.x, kSpot.y * 0.2, -kSpot.z}})
        for (float r : {1.0f / 6.0f, 0.25f, 2.0f / 6.0f, 0.5f, 4.0f / 6.0f, 5.0f / 6.0f, 1.0f}) {
            qs.push_back({d, r, 1.0f, 1.0f});
            dirs.push_back(d);
        }
    const std::vector<Answer> gpu = rig.probe(qs);
    double worst_level = 0.0, worst_between = 0.0;
    for (std::size_t i = 0; i < qs.size(); ++i) {
        const Rgb ref = convolve_reference(spot_sky, normalize(qs[i].dir), qs[i].roughness);
        const double err = std::abs(gpu[i].radiance[0] - ref[0]) / std::max(ref[0], 0.15);
        const double lvl = qs[i].roughness * (kLevels - 1.0);
        const bool on_level = std::abs(lvl - std::round(lvl)) < 1e-4;
        (on_level ? worst_level : worst_between) =
            std::max(on_level ? worst_level : worst_between, err);
        MESSAGE("  conv q" << i << " r=" << qs[i].roughness << " gpu=" << gpu[i].radiance[0]
                           << " cpu=" << ref[0] << " err=" << err);
    }
    MESSAGE("GPU vs CPU convolution: worst error (relative to max(ref, background)) on a level = "
            << worst_level << ", between levels = " << worst_between);
    // Margin 5% against measured 3.3% (on a level) and 2.7% (between levels). The error is the
    // 1024-sample estimator on a spot ~1% of the sphere: it was 14% at 256 samples and 3.3% at
    // 1024, so it is sampling variance that shrinks with N, not a bias. The lerp between levels
    // costs nothing measurable on top (between <= on-level here).
    CHECK(worst_level < 0.05);
    CHECK(worst_between < 0.05);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE("sky specular: no seam at a level boundary, and roughness k/6 reads layer k-1") {
    auto device = device_or_skip("the continuity proof");
    if (!device)
        return;
    Rig rig(*device);
    rig.set_sky(spot_sky);
    rig.bake(true);

    // (a) Level identity. At roughness exactly k/6 the lookup must return array layer k-1, read
    // here back from the texture on the CPU at the texel whose centre is the queried direction.
    // This is the check that catches an off-by-one in the roughness -> layer mapping, which a
    // smooth function of roughness would otherwise hide inside "approximately continuous".
    std::vector<std::vector<float>> layers;
    for (std::uint32_t l = 0; l < kSkySpecularLayers; ++l)
        layers.push_back(rig.read_layer(l));
    const std::uint32_t tx = 70, ty = 50; // a texel near the spot, away from the azimuth seam
    const Vec3d texel_dir =
        direction_from_uv((tx + 0.5) / kSkySpecularWidth, (ty + 0.5) / kSkySpecularHeight);
    // The CPU's restatement of the mapping must itself round-trip, or "the texel at this direction"
    // below means nothing.
    const std::array<double, 2> uv = uv_from_direction(texel_dir);
    CHECK(uv[0] == doctest::Approx((tx + 0.5) / kSkySpecularWidth).epsilon(1e-9));
    CHECK(uv[1] == doctest::Approx((ty + 0.5) / kSkySpecularHeight).epsilon(1e-9));
    std::vector<Query> qs;
    for (std::uint32_t k = 1; k <= kSkySpecularLayers; ++k)
        qs.push_back({texel_dir,
                      static_cast<float>(k) / static_cast<float>(kSkySpecularLayers),
                      1.0f,
                      1.0f});
    const std::vector<Answer> a = rig.probe(qs);
    double worst_identity = 0.0;
    for (std::uint32_t k = 1; k <= kSkySpecularLayers; ++k) {
        const float expect =
            layers[k - 1][(static_cast<std::size_t>(ty) * kSkySpecularWidth + tx) * 4];
        worst_identity = std::max(worst_identity, rel(a[k - 1].radiance[0], expect));
        MESSAGE("roughness " << k << "/6: lookup " << a[k - 1].radiance[0] << " vs layer " << k - 1
                             << " texel " << expect);
    }
    // Margin 2e-3: bilinear filtering at an exact texel centre returns the texel; the residue is
    // the uv round-trip in fp32.
    CHECK(worst_identity < 2e-3);

    // (b) Continuity. Either side of each boundary, an epsilon apart. By the linear lerp the
    // difference is bounded by (change of roughness x 6) x (the gap between the two levels) -- the
    // Lipschitz bound -- so a seam (reading the wrong layer on one side) blows through it.
    constexpr float eps = 1e-3f;
    const std::vector<Vec3d> probes = {
        kSpot, normalize({kSpot.x + 0.05, kSpot.y, kSpot.z}), texel_dir};
    std::uint32_t seams = 0;
    double worst_ratio = 0.0;
    for (std::uint32_t k = 0; k <= kSkySpecularLayers; ++k) {
        for (Vec3d d : probes) {
            const float r = static_cast<float>(k) / static_cast<float>(kSkySpecularLayers);
            if (k == 0 || k == kSkySpecularLayers)
                continue; // the domain ends: nothing on the far side to compare
            std::vector<Query> pair = {{d, r - eps, 1.0f, 1.0f},
                                       {d, r + eps, 1.0f, 1.0f},
                                       {d, r - 1.0f / 6.0f, 1.0f, 1.0f},
                                       {d, r + 1.0f / 6.0f, 1.0f, 1.0f}};
            const std::vector<Answer> p = rig.probe(pair);
            const double jump = std::abs(p[1].radiance[0] - p[0].radiance[0]);
            const double level_gap =
                std::max(std::abs(p[3].radiance[0] - p[2].radiance[0]) * 0.5, 1e-4);
            // |dL| <= 2 eps * 6 * gap, plus fp16 slack of 1e-3 relative on each read.
            const double bound = 2.0 * eps * 6.0 * level_gap * 2.0 + 2e-3 * p[0].radiance[0];
            worst_ratio = std::max(worst_ratio, jump / bound);
            if (jump > bound)
                ++seams;
        }
    }
    MESSAGE("boundary continuity: worst jump / bound = " << worst_ratio << ", seams = " << seams);
    CHECK(seams == 0);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE("sky specular: every rebuild, reuse and disabled frame is counted") {
    auto device = device_or_skip("the cache-counter proof");
    if (!device)
        return;
    Rig rig(*device);
    rig.set_sky([](Vec3d) { return Rgb{0.5, 0.5, 0.5}; });

    // GPU time of the first frame (DFG table + prefilter chain) and of a rebuild (chain only).
    // Reported, not asserted. Both are ONE-SHOT costs: the table is built once per device and the
    // chain only on a frame where the sky changed -- an unchanged sky pays nothing (the reuse
    // counters below).
    const double first_ms = rig.bake(true);
    CHECK(rig.spec().stats().dfg_filled == 1);
    CHECK(rig.spec().stats().prefilter_filled == 1);
    CHECK(rig.spec().stats().prefilter_reused == 0);

    // Nothing changed: the DFG table never rebuilds (it depends on no input), and the chain reuses.
    rig.bake(false);
    rig.bake(false);
    CHECK(rig.spec().stats().dfg_filled == 1);
    CHECK(rig.spec().stats().dfg_reused == 2);
    CHECK(rig.spec().stats().prefilter_filled == 1);
    CHECK(rig.spec().stats().prefilter_reused == 2);

    // The source moved: the chain rebuilds.
    const double rebuild_ms = rig.bake(true);
    MESSAGE("GPU time: first frame (DFG + chain) = "
            << first_ms << " ms, chain-only rebuild = " << rebuild_ms << " ms ("
            << kSkySpecularLayers << " layers x " << kSkySpecularWidth << "x" << kSkySpecularHeight
            << " x " << kSkySpecularPrefilterSamples << " samples)");
    CHECK(rig.spec().stats().prefilter_filled == 2);
    CHECK(rig.spec().stats().dfg_filled == 1);

    // The source moved while nobody was filtering: the next frame must rebuild even though it is
    // told `source_changed = false`, or a stale chain of the earlier sky would be served.
    rig.spec().mark_stale();
    rig.bake(false);
    CHECK(rig.spec().stats().prefilter_filled == 3);

    // A disabled frame is counted as one, not as a reuse.
    RenderGraph graph(*device);
    graph.reset();
    const std::uint32_t before = rig.spec().stats().disabled_frames;
    (void)rig.spec().empty_binding(graph);
    CHECK(rig.spec().stats().disabled_frames == before + 1);
    CHECK(rig.spec().stats().prefilter_reused == 2); // untouched by the disabled frame
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
namespace {

constexpr std::uint32_t kSize = 96;

[[nodiscard]] HdrImage
render_hdr(rhi::Device& device, SceneRenderer& renderer, MeshId floor, MaterialId mat) {
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
    core::Transform cam{};
    cam.translation = {0.0f, 1.0f, 0.0f};
    (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{1.1f, 0.1f, 100.0f, true});
    RenderGraph graph(device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

[[nodiscard]] SkyParams clear_sky() {
    SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    sp.use_scene_sun = false;
    sp.sun_direction[0] = 0.0f;
    sp.sun_direction[1] = 0.8f;
    sp.sun_direction[2] = -0.6f;
    return sp;
}

// The floor fills the lower half of the frame (camera 1 m up, looking level). Per-row mean
// luminance over the central columns, rows 66..90. The plane is 12 m across, so from 1 m up its far
// edge is ~9.5 degrees below the horizon (row ~62): rows above that are background, not floor.
struct FloorRows {
    std::vector<double> row_mean;
    double mean = 0.0;

    [[nodiscard]] double contrast() const {
        // The relative spread of the row means: how much the reflection changes down the frame.
        double lo = 1e30, hi = -1e30;
        for (double v : row_mean) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return (hi - lo) / std::max(mean, 1e-9);
    }
};

[[nodiscard]] FloorRows floor_rows(const HdrImage& img) {
    FloorRows f;
    for (std::uint32_t y = 66; y <= 90; ++y) {
        double s = 0.0;
        for (std::uint32_t x = 32; x < 64; ++x)
            s += img.luminance(x, y);
        f.row_mean.push_back(s / 32.0);
        f.mean += s / 32.0;
    }
    f.mean /= static_cast<double>(f.row_mean.size());
    return f;
}

} // namespace

TEST_CASE("sky specular: through the renderer, a smooth metal reads a sharp sky, a rough one a "
          "blurred one") {
    auto device = device_or_skip("the integration proof");
    if (!device)
        return;
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "sky-specular-floor");
    MaterialRegistry materials;
    const auto metal = [&](float roughness) {
        PbrMaterialDesc md{};
        md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.9f;
        md.metallic = 1.0f;
        md.roughness = roughness;
        return materials.add(md);
    };
    const MaterialId smooth = metal(0.05f);
    const MaterialId rough = metal(1.0f);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_sky(clear_sky());

    // Off first: the placeholders are bound, no chain is built, and the frame counts as disabled.
    renderer.set_sky_specular_prefilter_enabled(false);
    const FloorRows smooth_off = floor_rows(render_hdr(*device, renderer, floor, smooth));
    const FloorRows rough_off = floor_rows(render_hdr(*device, renderer, floor, rough));
    CHECK(renderer.sky_specular_stats().prefilter_filled == 0);
    CHECK(renderer.sky_specular_stats().dfg_filled == 0);

    // On.
    renderer.set_sky_specular_prefilter_enabled(true);
    const FloorRows smooth_on = floor_rows(render_hdr(*device, renderer, floor, smooth));
    const FloorRows rough_on = floor_rows(render_hdr(*device, renderer, floor, rough));
    const SkySpecularStats& st = renderer.sky_specular_stats();
    MESSAGE("integration contrast (row spread / mean): smooth off="
            << smooth_off.contrast() << " on=" << smooth_on.contrast()
            << "; rough off=" << rough_off.contrast() << " on=" << rough_on.contrast());
    MESSAGE("integration mean luminance: smooth off=" << smooth_off.mean << " on=" << smooth_on.mean
                                                      << "; rough off=" << rough_off.mean
                                                      << " on=" << rough_on.mean);
    MESSAGE("stats: dfg " << st.dfg_filled << "/" << st.dfg_reused << " prefilter "
                          << st.prefilter_filled << "/" << st.prefilter_reused << " disabled "
                          << st.disabled_frames);

    // The flag really switched the path: the lit floor changed.
    CHECK(smooth_on.mean != doctest::Approx(smooth_off.mean).epsilon(1e-6));
    CHECK(rough_on.mean != doctest::Approx(rough_off.mean).epsilon(1e-6));
    // A smooth metal shows the sky's elevation gradient down the frame; a rough one averages it
    // away. The factor is a structural margin (the lobe of roughness 1 spans the hemisphere).
    const auto horizon_over_bottom = [](const FloorRows& f) {
        return f.row_mean.front() / f.row_mean.back();
    };
    MESSAGE("horizon/bottom row ratio: smooth on=" << horizon_over_bottom(smooth_on)
                                                   << " off=" << horizon_over_bottom(smooth_off)
                                                   << "; rough on=" << horizon_over_bottom(rough_on)
                                                   << " off=" << horizon_over_bottom(rough_off));

    // SMOOTH READS THE SKY ITSELF (mip 0); ROUGH READS A BLUR. The top row of the measured band
    // reflects sky ~9.5 degrees up, the bottom row ~31 degrees up. The clear sky is brighter near
    // the horizon, so a smooth metal's rows follow that gradient (measured ~1.48x top over bottom)
    // while a lobe as wide as the hemisphere averages it away (measured 1.02x). The factors below
    // are well inside those measurements; what they rule out is a rough metal still mirroring, or
    // a smooth one already blurred.
    CHECK(horizon_over_bottom(smooth_on) > 1.25);
    CHECK(horizon_over_bottom(rough_on) < 1.1);
    CHECK(smooth_on.contrast() > 5.0 * rough_on.contrast());

    // THE REGRESSION BRIDGE. A smooth metal is the case the old mirror path got right, so the
    // split-sum path must agree with it to the accuracy of the old analytic BRDF fit ("within a few
    // percent of the LUT", brdf.glsl): measured 3.3%, bound 8%.
    CHECK(smooth_on.mean == doctest::Approx(smooth_off.mean).epsilon(0.08));
    // A ROUGH metal is deliberately NOT the same: the old path scaled the SH average by a
    // single-bounce fit that loses ~55% of the energy at roughness 1, and the new path puts that
    // energy back (the furnace test above). So it must be BRIGHTER, by about 1/E.
    CHECK(rough_on.mean > 1.5 * rough_off.mean);
    // Both frames were lit.
    CHECK(smooth_on.mean > 1e-4);
    CHECK(rough_on.mean > 1e-4);

    // Counters. Two ON frames under one unchanged sky: built once, reused once; the DFG built once.
    CHECK(st.dfg_filled == 1);
    CHECK(st.prefilter_filled == 1);
    CHECK(st.prefilter_reused >= 1);
    CHECK(st.disabled_frames == 2); // the two OFF frames above, nothing else

    // A switched-off frame in the middle must not strand the chain: re-sky while OFF, then ON.
    renderer.set_sky_specular_prefilter_enabled(false);
    SkyParams other = clear_sky();
    other.sun_direction[1] = 0.1f;
    renderer.set_sky(other);
    (void)render_hdr(*device, renderer, floor, smooth);
    const std::uint32_t filled_before = renderer.sky_specular_stats().prefilter_filled;
    renderer.set_sky_specular_prefilter_enabled(true);
    (void)render_hdr(*device, renderer, floor, smooth);
    CHECK(renderer.sky_specular_stats().prefilter_filled == filled_before + 1);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
TEST_CASE("sky specular: with SSR on the resolve reads the chain too, and the picture changes") {
    auto device = device_or_skip("the SSR-reader proof");
    if (!device)
        return;
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "sky-specular-ssr-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.9f;
    md.metallic = 1.0f;
    md.roughness = 0.5f;
    const MaterialId mat = materials.add(md);

    LightingSettings ls;
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    SceneRenderer renderer(*device, meshes, materials);
    render::test::disable_temporal_aa(renderer); // compares the plain frame (ADR-0078 1e)
    renderer.set_lighting(ls);
    renderer.set_sky(clear_sky());

    // THE GAP, CLOSED. Before ssr_resolve.frag read the chain, SSR on meant the forward pass
    // compiled its sky term out and the resolve reflected the sky-view LUT itself, so the chain had
    // no reader and the renderer rightly built nothing. The resolve now reads the same chain and
    // DFG table, so with SSR on the setting must BUILD them (once, and reuse them under an
    // unchanged sky) and must change the picture -- and the OFF frame is counted, so "nothing
    // happened" stays distinguishable from "it silently stopped working".
    renderer.set_sky_specular_prefilter_enabled(false);
    const HdrImage off = render_hdr(*device, renderer, floor, mat);
    renderer.set_sky_specular_prefilter_enabled(true);
    const HdrImage on = render_hdr(*device, renderer, floor, mat);
    (void)render_hdr(*device, renderer, floor, mat);
    CHECK(on.rgb != off.rgb);
    const SkySpecularStats& st = renderer.sky_specular_stats();
    CHECK(st.prefilter_filled == 1);
    CHECK(st.prefilter_reused >= 1);
    CHECK(st.dfg_filled == 1);
    CHECK(st.disabled_frames == 1); // the one OFF frame above, nothing else
}

TEST_CASE("sky specular: with SSR and DDGI on the setting changes nothing and builds nothing") {
    auto device = device_or_skip("the DDGI-gap proof");
    if (!device)
        return;
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "sky-specular-ddgi-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.9f;
    md.metallic = 1.0f;
    md.roughness = 0.5f;
    const MaterialId mat = materials.add(md);

    LightingSettings ls;
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    // Two FRESH renderers, one frame each: DDGI accumulates over frames (hysteresis), so a second
    // frame on one renderer would differ from the first for reasons that have nothing to do with
    // the sky chain.
    SceneRenderer off_renderer(*device, meshes, materials);
    SceneRenderer on_renderer(*device, meshes, materials);
    for (SceneRenderer* r : {&off_renderer, &on_renderer}) {
        render::test::disable_temporal_aa(*r); // compares the plain frame (ADR-0078 1e)
        r->set_lighting(ls);
        r->set_sky(clear_sky());
    }

    // With DDGI on the sky is not the surface's environment -- a probe field that knows about walls
    // is -- so neither reader touches the chain: the forward pass's sky term sits in a branch that
    // is not taken, and ssr_resolve's miss term is the DDGI field. Building the chain would be work
    // nothing reads, so the renderer declines and counts the frames it declined.
    off_renderer.set_sky_specular_prefilter_enabled(false);
    on_renderer.set_sky_specular_prefilter_enabled(true);
    const HdrImage off = render_hdr(*device, off_renderer, floor, mat);
    const HdrImage on = render_hdr(*device, on_renderer, floor, mat);
    CHECK(on.rgb == off.rgb);
    CHECK(on_renderer.sky_specular_stats().prefilter_filled == 0);
    CHECK(on_renderer.sky_specular_stats().dfg_filled == 0);
    CHECK(on_renderer.sky_specular_stats().disabled_frames == 1);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// A MEASUREMENT, NOT A PROOF: what the chain's per-pixel lookup costs the pass that reads it.
//
// ADR-0078 owes the first brick that turns the chain on a Release perf run, and no committed
// sample exercises it (the-block and lit-rooms run DDGI, where neither reader touches the chain).
// So this probe renders a full-HD rough-metal floor under a sky and prints the median GPU time of
// the reading pass and of the bake, chain on against chain off, for SSR on and off. It asserts
// nothing about time -- a time is a property of one GPU -- and is skipped unless RIME_PERF_PROBE
// is set, because on lavapipe or a Debug build the numbers mean nothing and 1080p is slow.
//
//   RIME_PERF_PROBE=1 ./build/release/bin/rime_render_tests -tc="sky specular: per-pass*"
TEST_CASE("sky specular: per-pass GPU cost of the lookup, chain on against off (probe)") {
    if (std::getenv("RIME_PERF_PROBE") == nullptr) {
        MESSAGE("RIME_PERF_PROBE not set -- skipping the per-pass cost probe");
        return;
    }
    auto device = device_or_skip("the per-pass cost probe");
    if (!device)
        return;
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "sky-specular-perf-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.9f;
    md.metallic = 1.0f;
    md.roughness = 0.5f;
    const MaterialId mat = materials.add(md);

    constexpr std::uint32_t kWidth = 1920;
    constexpr std::uint32_t kHeight = 1080;
    constexpr int kWarm = 8;
    constexpr int kFrames = 40;

    const auto median_ms = [](std::vector<double> v) {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    for (const bool ssr : {false, true}) {
        for (const bool chain : {false, true}) {
            LightingSettings ls;
            ls.ssr_enabled = ssr;
            ls.ssr_max_distance = 8.0f;
            ls.ssr_thickness = 0.5f;
            ls.ssr_max_steps = 64;
            SceneRenderer renderer(*device, meshes, materials);
            render::test::disable_temporal_aa(renderer);
            renderer.set_lighting(ls);
            renderer.set_sky(clear_sky());
            renderer.set_sky_specular_prefilter_enabled(chain);

            std::vector<double> reader, total;
            for (int i = 0; i < kWarm + kFrames; ++i) {
                ecs::World world;
                register_render_components(world);
                (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
                core::Transform cam{};
                cam.translation = {0.0f, 1.0f, 0.0f};
                (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{1.1f, 0.1f, 100.0f, true});
                RenderGraph graph(*device);
                graph.reset();
                const SceneRenderer::Output out = renderer.render(graph, world, {kWidth, kHeight});
                graph.export_texture(out.hdr);
                auto cmd = device->begin_commands();
                graph.execute(*cmd);
                const auto timings = graph.submit_and_time(*device, std::move(cmd));
                if (i < kWarm)
                    continue;
                double sum = 0.0;
                double pass = 0.0;
                for (const auto& t : timings) {
                    sum += t.gpu_ms;
                    if (t.name == (ssr ? "ssr-resolve" : "forward-pbr shadowed"))
                        pass = t.gpu_ms;
                }
                reader.push_back(pass);
                total.push_back(sum);
            }
            MESSAGE("1080p rough metal, SSR "
                    << (ssr ? "on " : "off") << " chain " << (chain ? "on " : "off") << ": "
                    << std::string(ssr ? "ssr-resolve" : "forward-pbr shadowed") << " median "
                    << median_ms(reader) << " ms, all passes median " << median_ms(total)
                    << " ms; chain bakes " << renderer.sky_specular_stats().prefilter_filled
                    << " (unchanged sky: bake is one-off)");
        }
    }
}

namespace {

// Analytic box fields, independently sampled as texel-centre signed distances. The renderer's
// extraction and compose paths still run: the proof does not upload a finished clipmap.
float occlusion_box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

assets::MeshSdfAsset occlusion_box_sdf(core::Vec3 half_extents,
                                       std::uint32_t target_resolution = 24) {
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
                const float d = occlusion_box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

struct OcclusionScene {
    ecs::World world;
    ecs::Entity camera;

    OcclusionScene(SceneRenderer& renderer, MeshRegistry& meshes, MaterialRegistry& materials) {
        register_render_components(world);
        PbrMaterialDesc metal{};
        metal.base_color[0] = metal.base_color[1] = metal.base_color[2] = 0.9f;
        metal.metallic = 1.0f;
        metal.roughness =
            0.6f; // resolve's cone is pure fallback, so this tests sky, not screen hits
        const MaterialId mat = materials.add(metal);
        const MeshId plane = meshes.add(make_plane(6.0f), "sdf-specular-metal");
        for (const float x : {0.0f, 12.0f}) {
            core::Transform t{};
            t.translation.x = x;
            (void)world.spawn_with(ecs::WorldTransform{t}, MeshRef{plane}, MaterialRef{mat});
        }
        PbrMaterialDesc wall{};
        wall.base_color[0] = wall.base_color[1] = wall.base_color[2] = 0.0f;
        const MaterialId wall_mat = materials.add(wall);
        const auto place = [&](core::Vec3 half, core::Vec3 at) {
            const SdfSourceId sdf = renderer.register_sdf_source(occlusion_box_sdf(half, 64));
            const MeshId mesh = meshes.add(make_box(half), "sdf-specular-wall");
            core::Transform t{};
            t.translation = at;
            (void)world.spawn_with(
                ecs::WorldTransform{t}, MeshRef{mesh}, MaterialRef{wall_mat}, SdfRef{sdf});
        };
        // Six overlapping 0.5 m slabs: cavity x/z=[-3,3], y=[0,3]. Camera inside, metal on
        // the floor's inner face. The outdoor metal is 12 m away, beyond the 8 m cone reach.
        place({3.5f, 0.25f, 3.5f}, {0.0f, -0.25f, 0.0f});
        place({3.5f, 0.25f, 3.5f}, {0.0f, 3.25f, 0.0f});
        place({0.25f, 2.0f, 3.5f}, {-3.25f, 1.5f, 0.0f});
        place({0.25f, 2.0f, 3.5f}, {3.25f, 1.5f, 0.0f});
        place({3.5f, 2.0f, 0.25f}, {0.0f, 1.5f, -3.25f});
        place({3.5f, 2.0f, 0.25f}, {0.0f, 1.5f, 3.25f});
        // An identical solid floor beneath the outdoor metal catches the self-occlusion defect.
        place({3.5f, 0.25f, 3.5f}, {12.0f, -0.25f, 0.0f});
        core::Transform cam{};
        cam.translation = {0.0f, 1.0f, 0.0f};
        camera = world.spawn_with(ecs::WorldTransform{cam}, Camera{1.1f, 0.1f, 100.0f, true});
    }

    void view(float x) {
        core::Transform cam{};
        cam.translation = {x, 1.0f, 0.0f};
        *world.get<ecs::WorldTransform>(camera) = ecs::WorldTransform{cam};
        world.mark_changed<ecs::WorldTransform>(camera);
    }
};

HdrImage render_occlusion_hdr(rhi::Device& device,
                              SceneRenderer& renderer,
                              OcclusionScene& scene,
                              std::size_t& passes) {
    RenderGraph graph(device);
    graph.reset();
    const auto out = renderer.render(graph, scene.world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    passes = graph.pass_count();
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

} // namespace

TEST_CASE(
    "SDF specular occlusion: enclosed metal loses sky and identical outdoor metal stays bright") {
    auto device = device_or_skip("the SDF specular occlusion proof");
    if (!device)
        return;
    for (const bool ssr : {false, true}) {
        for (const bool chain : {false, true}) {
            CAPTURE(ssr);
            CAPTURE(chain);
            MeshRegistry meshes(*device);
            MaterialRegistry materials;
            SceneRenderer renderer(*device, meshes, materials);
            render::test::disable_temporal_aa(renderer);
            CHECK_FALSE(renderer.sdf_specular_occlusion_enabled());
            renderer.set_sky(clear_sky());
            renderer.set_sky_specular_prefilter_enabled(chain);
            LightingSettings ls{};
            ls.sdf_clipmap_enabled = true;
            ls.ssr_enabled = ssr;
            renderer.set_lighting(ls);
            OcclusionScene scene(renderer, meshes, materials);
            std::size_t passes = 0;
            for (const float x : {0.0f, 12.0f}) {
                CAPTURE(x);
                scene.view(x);
                renderer.set_sdf_specular_occlusion_enabled(false);
                (void)render_occlusion_hdr(*device, renderer, scene, passes); // warm bakes/recentre
                const HdrImage off = render_occlusion_hdr(*device, renderer, scene, passes);
                const std::size_t off_passes = passes;
                renderer.set_sdf_specular_occlusion_enabled(true);
                const HdrImage on = render_occlusion_hdr(*device, renderer, scene, passes);
                CHECK(passes == off_passes); // estimator lives in the reader, no extra pass
                const double off_mean = floor_rows(off).mean;
                const double on_mean = floor_rows(on).mean;
                REQUIRE_MESSAGE(off_mean > 1e-4, "control metal must reflect a nonzero sky");
                MESSAGE("SDF sky: SSR=" << ssr << " chain=" << chain << " camera x=" << x
                                        << " off=" << off_mean << " on=" << on_mean
                                        << " ratio=" << on_mean / off_mean);
                // A sealed cavity has zero visible sky; an open floor has full visibility.
                // The inner patch (rows 66..90, columns 32..63) is wholly floor: its farthest
                // point is <3 m away at this FOV, before the back wall. A white metal has zero
                // diffuse, so these means measure sky specular alone. Allow 10% residual for
                // the bounded/narrow-band cone approximation, and 2% outdoor drift for FP16
                // storage/reconstruction; both are far smaller than a global darkening defect.
                if (x == 0.0f) {
                    CHECK_MESSAGE(on_mean < off_mean * 0.1,
                                  "enclosed metal sky must drop by at least 10x");
                } else {
                    CHECK_MESSAGE(rel(on_mean, off_mean) < 0.02,
                                  "outdoor metal sky must change by less than 2%");
                    for (std::uint32_t y = 66; y <= 90; ++y)
                        for (std::uint32_t px = 32; px < 64; ++px)
                            CHECK_MESSAGE(rel(on.luminance(px, y), off.luminance(px, y)) < 0.02,
                                          "outdoor pixels must retain their sky specular");
                }
                renderer.set_sdf_specular_occlusion_enabled(false);
                const HdrImage off_again = render_occlusion_hdr(*device, renderer, scene, passes);
                CHECK(off_again.rgb == off.rgb); // OFF after ON restores every HDR byte
            }
            CHECK(renderer.sdf_specular_occlusion_stats().enabled_frames == 2);
            CHECK(renderer.sdf_specular_occlusion_stats().disabled_frames == 6);
            CHECK(renderer.sdf_specular_occlusion_stats().unavailable_frames == 0);
        }
    }
}

TEST_CASE(
    "SDF specular occlusion: an unavailable clipmap is counted and the frame is byte identical") {
    auto device = device_or_skip("the unavailable SDF proof");
    if (!device)
        return;
    for (const bool ssr : {false, true}) {
        MeshRegistry meshes(*device);
        const MeshId floor = meshes.add(make_plane(12.0f), "sdf-disabled-floor");
        MaterialRegistry materials;
        PbrMaterialDesc md{};
        md.metallic = 1.0f;
        md.roughness = 0.6f;
        const MaterialId mat = materials.add(md);
        SceneRenderer renderer(*device, meshes, materials);
        render::test::disable_temporal_aa(renderer);
        LightingSettings ls{};
        ls.ssr_enabled = ssr;
        renderer.set_lighting(ls); // no SDF levels have been allocated or composed
        renderer.set_sky(clear_sky());
        const HdrImage off = render_hdr(*device, renderer, floor, mat);
        renderer.set_sdf_specular_occlusion_enabled(true);
        CHECK(renderer.sdf_specular_occlusion_enabled());
        const HdrImage unavailable = render_hdr(*device, renderer, floor, mat);
        CHECK(unavailable.rgb == off.rgb);
        CHECK(renderer.sdf_specular_occlusion_stats().unavailable_frames == 1);
        CHECK(renderer.sdf_specular_occlusion_stats().enabled_frames == 0);
        CHECK(renderer.sdf_specular_occlusion_stats().disabled_frames == 1);
    }
}

TEST_CASE("SDF specular occlusion: frames without a sky reader are counted") {
    auto device = device_or_skip("the SDF no-reader counter proof");
    if (!device)
        return;
    for (const bool ddgi : {false, true}) {
        MeshRegistry meshes(*device);
        MaterialRegistry materials;
        SceneRenderer renderer(*device, meshes, materials);
        render::test::disable_temporal_aa(renderer);
        LightingSettings ls{};
        ls.sdf_clipmap_enabled = true;
        ls.ddgi_enabled = ddgi;
        renderer.set_lighting(ls);
        if (ddgi)
            renderer.set_sky(clear_sky()); // DDGI owns the environment, so there is no sky reader
        renderer.set_sdf_specular_occlusion_enabled(true);
        OcclusionScene scene(renderer, meshes, materials);
        std::size_t passes = 0;
        (void)render_occlusion_hdr(*device, renderer, scene, passes);
        CHECK(renderer.sdf_specular_occlusion_stats().no_reader_frames == 1);
        CHECK(renderer.sdf_specular_occlusion_stats().unavailable_frames == 0);
        CHECK(renderer.sdf_specular_occlusion_stats().enabled_frames == 0);
    }
}

// An opt-in measurement, not a timing assertion. A steady sky/field excludes bake/compose work;
// report each sky reader and the full graph so its cost cannot hide inside another pass. Release,
// 1080p, actual sky on, no lights, identical outdoor metal; clocks are not pinned. The lower floor
// covers only part of this view, so this is a scene measurement rather than a full-screen bound.
TEST_CASE("SDF specular occlusion: per-pass GPU cost (probe)") {
    if (std::getenv("RIME_PERF_PROBE") == nullptr) {
        MESSAGE("RIME_PERF_PROBE not set -- skipping the SDF per-pass cost probe");
        return;
    }
    auto device = device_or_skip("the SDF cost probe");
    if (!device)
        return;
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    for (const bool ssr : {false, true}) {
        MeshRegistry meshes(*device);
        MaterialRegistry materials;
        SceneRenderer renderer(*device, meshes, materials);
        render::test::disable_temporal_aa(renderer);
        LightingSettings ls{};
        ls.sdf_clipmap_enabled = true;
        ls.ssr_enabled = ssr;
        renderer.set_lighting(ls);
        renderer.set_sky(clear_sky());
        OcclusionScene scene(renderer, meshes, materials);
        scene.view(12.0f);
        for (const bool enabled : {false, true}) {
            renderer.set_sdf_specular_occlusion_enabled(enabled);
            std::vector<double> reader, total;
            for (int frame = 0; frame < 48; ++frame) {
                RenderGraph graph(*device);
                graph.reset();
                const auto out = renderer.render(graph, scene.world, {1920, 1080});
                graph.export_texture(out.hdr);
                auto cmd = device->begin_commands();
                graph.execute(*cmd);
                const auto timings = graph.submit_and_time(*device, std::move(cmd));
                if (frame < 8)
                    continue;
                double sum = 0.0;
                double pass = 0.0;
                for (const auto& t : timings) {
                    sum += t.gpu_ms;
                    if (t.name == (ssr ? "ssr-resolve" : "forward-pbr shadowed"))
                        pass = t.gpu_ms;
                }
                reader.push_back(pass);
                total.push_back(sum);
            }
            MESSAGE("SDF 1080p outdoor rough metal, SSR="
                    << ssr << " occlusion=" << enabled << " reader median=" << median(reader)
                    << " ms, graph median=" << median(total)
                    << " ms; adapter=" << device->adapter().name);
        }
    }
}
