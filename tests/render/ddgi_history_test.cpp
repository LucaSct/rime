// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "ddgi_remap.comp.spv.h"
#include "render_test_support.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/render_graph.hpp"

using namespace rime;
using namespace rime::render;

namespace {
struct Params {
    std::int32_t shift[4]{};
    std::uint32_t dims[4]{3, 3, 2, 9};
};

void prove_gpu_remap(rhi::Device& dev, std::array<int, 3> shift, bool full_reset) {
    constexpr std::uint32_t total = 18;
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Compute;
    sd.spirv = ddgi_remap_comp_spv;
    sd.spirv_size_bytes = sizeof(ddgi_remap_comp_spv);
    auto shader = dev.create_shader(sd);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {1, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {2, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {3, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {4, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
    };
    rhi::ComputePipelineDesc pd{};
    pd.shader = shader;
    pd.bindings = bindings;
    auto pipeline = dev.create_compute_pipeline(pd);

    std::array<rhi::TextureHandle, 4> textures{};
    std::array<std::vector<std::uint8_t>, 2> seeded;
    for (std::uint32_t atlas = 0; atlas < 2; ++atlas) {
        const std::uint32_t tile = atlas == 0 ? 8 : 16;
        const std::uint32_t width = 9 * tile;
        const std::uint32_t height = 2 * tile;
        rhi::TextureDesc td{};
        td.extent = {width, height};
        td.format = atlas == 0 ? rhi::Format::RGBA16Float : rhi::Format::RG32Float;
        // Sampled: the sources are imported in ShaderRead (write_texture's post-condition), and
        // SHADER_READ_ONLY_OPTIMAL is only a legal layout for an image created with it.
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled |
                   rhi::TextureUsage::TransferDst | rhi::TextureUsage::TransferSrc;
        textures[atlas] = dev.create_texture(td);
        textures[atlas + 2] = dev.create_texture(td);
        auto& bytes = seeded[atlas];
        bytes.resize(static_cast<std::size_t>(width) * height * 8);
        for (std::uint32_t probe = 0; probe < total; ++probe) {
            for (std::uint32_t y = 0; y < tile; ++y) {
                for (std::uint32_t x = 0; x < tile; ++x) {
                    const auto offset = (static_cast<std::size_t>(probe / 9 * tile + y) * width +
                                         probe % 9 * tile + x) *
                                        8;
                    if (atlas == 0) {
                        std::uint16_t rgba[4] = {
                            static_cast<std::uint16_t>(0x3c00 + probe * 64 + y * tile + x),
                            static_cast<std::uint16_t>(0x4000 + probe),
                            static_cast<std::uint16_t>(0x4400 + y * tile + x),
                            0x3c00};
                        std::memcpy(bytes.data() + offset, rgba, 8);
                    } else {
                        float moments[2] = {static_cast<float>(1000 * probe + 16 * y + x),
                                            static_cast<float>(2000 * probe + 32 * y + 2 * x)};
                        std::memcpy(bytes.data() + offset, moments, 8);
                    }
                }
            }
        }
        dev.write_texture(textures[atlas], bytes.data(), bytes.size());
    }

    RenderGraph graph(dev);
    graph.reset();
    const RGTexture src_i = graph.import_texture(textures[0], rhi::ResourceState::ShaderRead);
    const RGTexture src_v = graph.import_texture(textures[1], rhi::ResourceState::ShaderRead);
    const RGTexture dst_i = graph.import_texture(textures[2], rhi::ResourceState::Undefined);
    const RGTexture dst_v = graph.import_texture(textures[3], rhi::ResourceState::Undefined);
    Params params{};
    params.shift[0] = shift[0];
    params.shift[1] = shift[1];
    params.shift[2] = shift[2];
    params.shift[3] = full_reset ? 1 : 0;
    const auto slice = graph.push_frame_data(&params, sizeof(params));
    const RGTexture reads[] = {src_i, src_v};
    const RGTexture writes[] = {dst_i, dst_v};
    RenderGraph::ComputePassDesc desc{};
    desc.storage_read = reads;
    desc.storage_write = writes;
    graph.add_compute_pass("ddgi-remap-proof", desc, [=](rhi::CommandBuffer& cmd) {
        cmd.bind_compute_pipeline(pipeline);
        for (std::uint32_t i = 0; i < 4; ++i)
            cmd.bind_storage_image(i, textures[i]);
        cmd.bind_uniform_buffer(4, slice.buffer, slice.offset, slice.size);
        cmd.dispatch(1, 1, total);
    });
    auto cmd = dev.begin_commands();
    graph.execute(*cmd);
    dev.submit_blocking(*cmd);

    for (std::uint32_t atlas = 0; atlas < 2; ++atlas) {
        const std::uint32_t tile = atlas == 0 ? 8 : 16;
        const std::uint32_t width = 9 * tile;
        const auto actual = test::read_texture(dev, textures[atlas + 2], width, 2 * tile, 8);
        for (int qz = 0; qz < 2; ++qz)
            for (int qy = 0; qy < 3; ++qy)
                for (int qx = 0; qx < 3; ++qx) {
                    const int sx = qx + shift[0], sy = qy + shift[1], sz = qz + shift[2];
                    const bool survives =
                        !full_reset && sx >= 0 && sx < 3 && sy >= 0 && sy < 3 && sz >= 0 && sz < 2;
                    const int dst = qx + 3 * qy + 9 * qz;
                    const int src = sx + 3 * sy + 9 * sz;
                    for (std::uint32_t y = 0; y < tile; ++y)
                        for (std::uint32_t x = 0; x < tile; ++x) {
                            const auto dest_offset =
                                (static_cast<std::size_t>(dst / 9 * tile + y) * width +
                                 dst % 9 * tile + x) *
                                8;
                            const auto src_offset =
                                (static_cast<std::size_t>(src / 9 * tile + y) * width +
                                 src % 9 * tile + x) *
                                8;
                            const std::uint8_t zeros[8]{};
                            CHECK(std::memcmp(actual.data() + dest_offset,
                                              survives ? seeded[atlas].data() + src_offset : zeros,
                                              8) == 0);
                        }
                }
    }
    for (auto texture : textures)
        dev.destroy(texture);
    dev.destroy(pipeline);
    dev.destroy(shader);
}
} // namespace

TEST_CASE("ddgi history: GPU remap preserves both complete atlas tiles and clears exposed tiles") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan device required for DDGI remap proof");
        return;
    }
    prove_gpu_remap(*device, {1, 0, 0}, false);
    prove_gpu_remap(*device, {-1, 0, 0}, false);
    prove_gpu_remap(*device, {1, -1, 1}, false);
    prove_gpu_remap(*device, {-2, 1, -1}, false);
    prove_gpu_remap(*device, {0, 0, 0}, true);
}

// ── The production chain: three queued shifts through DdgiProbes::add() itself ──────────────────
// The direct proof above runs the remap shader once, between textures the test owns. That cannot
// see the part that actually broke: DdgiProbes ping-pongs a persistent PAIR of atlases, so the
// texture a remap reads this frame is the one the next shift writes into, and with frames queued
// that write can be on the GPU while the read is still in flight. Every storage access is one
// graph state (StorageReadWrite), so nothing the graph compares would ever have emitted a barrier
// there. This case drives the real add() through +x, then -x, then a diagonal, submitting all
// three without waiting, and reads the atlases only after the last one finishes.
//
// Why survivors stay EXACT: hysteresis 1.0 makes every primed probe's blend
// `mix(new, old, 1.0) == old` bit for bit (new is finite: the scene is empty, unlit and has a
// black sky), so a surviving tile must hold its seeded signature exactly — every texel, the
// octahedral border ring included, and both visibility moments. A probe the lattice exposes is
// first-primed (hysteresis 0) from a black, empty scene: zero irradiance and a visibility no larger
// than the trace distance, which no seeded signature can be mistaken for.
namespace {
constexpr std::uint32_t kChainX = 3, kChainY = 3, kChainZ = 2;
constexpr std::uint32_t kChainProbes = kChainX * kChainY * kChainZ;
constexpr std::uint32_t kChainPerRow = kChainX * kChainY;
constexpr float kChainTrace = 8.0f;

std::size_t
tile_texel_offset(std::uint32_t probe, std::uint32_t tile, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t width = kChainPerRow * tile;
    return (static_cast<std::size_t>(probe / kChainPerRow * tile + y) * width +
            probe % kChainPerRow * tile + x) *
           8;
}

// Probe p, texel (x,y): a half-float irradiance whose red names the probe AND the texel, and two
// float moments far above anything a fresh trace can produce (<= kChainTrace, kChainTrace^2).
std::array<std::vector<std::uint8_t>, 2> chain_signatures() {
    std::array<std::vector<std::uint8_t>, 2> out;
    out[0].resize(static_cast<std::size_t>(kChainPerRow) * 8 * 2 * 8 * 8);
    out[1].resize(static_cast<std::size_t>(kChainPerRow) * 16 * 2 * 16 * 8);
    for (std::uint32_t p = 0; p < kChainProbes; ++p) {
        for (std::uint32_t y = 0; y < 8; ++y)
            for (std::uint32_t x = 0; x < 8; ++x) {
                const std::uint16_t rgba[4] = {
                    static_cast<std::uint16_t>(0x3c00 + p * 64 + y * 8 + x),
                    static_cast<std::uint16_t>(0x4000 + p),
                    static_cast<std::uint16_t>(0x4400 + y * 8 + x),
                    0x3c00};
                std::memcpy(out[0].data() + tile_texel_offset(p, 8, x, y), rgba, 8);
            }
        for (std::uint32_t y = 0; y < 16; ++y)
            for (std::uint32_t x = 0; x < 16; ++x) {
                const float moments[2] = {static_cast<float>(100000 + 1000 * p + 16 * y + x),
                                          static_cast<float>(300000 + 2000 * p + 32 * y + 2 * x)};
                std::memcpy(out[1].data() + tile_texel_offset(p, 16, x, y), moments, 8);
            }
    }
    return out;
}

// Where each lattice slot's history came from after a run of shifts: a seeded probe index, or -1
// for a slot some shift exposed. The CPU statement of the remap the GPU must have performed.
std::array<int, kChainProbes> chain_model(const std::vector<std::array<int, 3>>& shifts) {
    std::array<int, kChainProbes> origin{};
    for (std::uint32_t q = 0; q < kChainProbes; ++q)
        origin[q] = static_cast<int>(q);
    for (const auto& s : shifts) {
        std::array<int, kChainProbes> next{};
        for (int z = 0; z < static_cast<int>(kChainZ); ++z)
            for (int y = 0; y < static_cast<int>(kChainY); ++y)
                for (int x = 0; x < static_cast<int>(kChainX); ++x) {
                    const int sx = x + s[0], sy = y + s[1], sz = z + s[2];
                    const bool survives =
                        sx >= 0 && sy >= 0 && sz >= 0 && sx < static_cast<int>(kChainX) &&
                        sy < static_cast<int>(kChainY) && sz < static_cast<int>(kChainZ);
                    next[x + kChainX * (y + kChainY * z)] =
                        survives ? origin[sx + kChainX * (sy + kChainY * sz)] : -1;
                }
        origin = next;
    }
    return origin;
}

// What slot q holds: the seeded probe whose WHOLE signature it carries (irradiance rgb of every
// texel incl. the border, alpha zeroed by the blend's own store, both moments), -1 for a freshly
// primed tile, or -2 for anything else — a torn copy, a lost visibility copy, a stale tile.
int classify_chain_tile(std::uint32_t q,
                        const std::vector<std::uint8_t>& irr,
                        const std::vector<std::uint8_t>& vis,
                        const std::array<std::vector<std::uint8_t>, 2>& seeded) {
    for (std::uint32_t p = 0; p < kChainProbes; ++p) {
        bool same = true;
        for (std::uint32_t y = 0; y < 8 && same; ++y)
            for (std::uint32_t x = 0; x < 8 && same; ++x) {
                const auto at = tile_texel_offset(q, 8, x, y);
                std::uint16_t alpha = 0xffff;
                std::memcpy(&alpha, irr.data() + at + 6, 2);
                same = std::memcmp(irr.data() + at,
                                   seeded[0].data() + tile_texel_offset(p, 8, x, y),
                                   6) == 0 &&
                       alpha == 0;
            }
        for (std::uint32_t y = 0; y < 16 && same; ++y)
            for (std::uint32_t x = 0; x < 16 && same; ++x)
                same = std::memcmp(vis.data() + tile_texel_offset(q, 16, x, y),
                                   seeded[1].data() + tile_texel_offset(p, 16, x, y),
                                   8) == 0;
        if (same)
            return static_cast<int>(p);
    }
    bool fresh = true;
    for (std::uint32_t y = 0; y < 8 && fresh; ++y)
        for (std::uint32_t x = 0; x < 8 && fresh; ++x) {
            std::uint16_t rgba[4];
            std::memcpy(rgba, irr.data() + tile_texel_offset(q, 8, x, y), 8);
            for (std::uint16_t c : rgba)
                fresh = fresh && test::half_to_float(c) == 0.0f;
        }
    for (std::uint32_t y = 0; y < 16 && fresh; ++y)
        for (std::uint32_t x = 0; x < 16 && fresh; ++x) {
            float m[2];
            std::memcpy(m, vis.data() + tile_texel_offset(q, 16, x, y), 8);
            fresh = std::isfinite(m[0]) && std::isfinite(m[1]) && m[0] >= 0.0f && m[1] >= 0.0f &&
                    m[0] <= kChainTrace * 1.0001f && m[1] <= kChainTrace * kChainTrace * 1.0001f;
        }
    return fresh ? -1 : -2;
}

int count_differences(const std::array<int, kChainProbes>& a,
                      const std::array<int, kChainProbes>& b) {
    int n = 0;
    for (std::uint32_t q = 0; q < kChainProbes; ++q)
        n += a[q] != b[q] ? 1 : 0;
    return n;
}

// Counts the two log lines that name a barrier defect this chain could produce: a synchronization
// hazard (only when the validation layer runs with sync validation, e.g.
// VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1 — otherwise it can never fire, which is why the report
// pairs it with a falsification run) and the backend's own "the caller's claimed state is not the
// image's real layout" warning, which fires with no layer at all.
class ChainLogCounter {
public:
    ChainLogCounter() {
        core::set_log_sink([this](const core::LogRecord& r) {
            if (r.message.find("SYNC-HAZARD") != std::string_view::npos)
                ++hazards_;
            if (r.message.find("disagrees with the tracked layout") != std::string_view::npos)
                ++stale_states_;
            std::fprintf(stderr, "%.*s\n", static_cast<int>(r.message.size()), r.message.data());
        });
    }

    ~ChainLogCounter() { core::set_log_sink({}); }

    ChainLogCounter(const ChainLogCounter&) = delete;
    ChainLogCounter& operator=(const ChainLogCounter&) = delete;

    [[nodiscard]] int hazards_seen() const { return hazards_; }

    [[nodiscard]] int stale_states_seen() const { return stale_states_; }

private:
    int hazards_ = 0;
    int stale_states_ = 0;
};
} // namespace

TEST_CASE("ddgi history: three queued production shifts carry every surviving tile exactly") {
    rhi::DeviceDesc dd{};
    dd.enable_validation = true; // dropped gracefully when the layer is not installed
    auto device = rhi::create_device(dd);
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan device required for the DDGI production remap chain");
        return;
    }
    ChainLogCounter log;

    LightingSettings settings{};
    settings.ddgi_probe_count_x = kChainX;
    settings.ddgi_probe_count_y = kChainY;
    settings.ddgi_probe_count_z = kChainZ;
    settings.ddgi_probe_spacing = 1.0f;
    settings.ddgi_rays_per_probe = 16;
    settings.ddgi_max_trace_distance = kChainTrace;
    settings.ddgi_hysteresis = 1.0f; // a primed probe keeps its stored value bit for bit
    DdgiLightingInputs lighting{};   // no sun
    lighting.sky_radiance[0] = lighting.sky_radiance[1] = lighting.sky_radiance[2] = 0.0f;

    SdfClipmap clipmap(*device); // never given an instance: the trace sees an empty world
    DdgiProbes ddgi(*device);
    RenderGraph graph(*device);
    graph.set_frames_in_flight(3); // three frames' CPU-written slices are live at once below

    // A lattice cell (cx,cy,cz) is selected by a camera half a spacing inside it: the lattice is
    // centred on the camera, (count-1)/2 spacings to either side, then floored to the spacing.
    const auto camera_for = [](int cx, int cy, int cz) {
        return core::Vec3{static_cast<float>(cx) + 1.5f,
                          static_cast<float>(cy) + 1.5f,
                          static_cast<float>(cz) + 1.0f};
    };
    const core::Vec3 clip_camera = camera_for(0, 0, 0); // the clipmap stays put; only DDGI moves
    const auto record = [&](core::Vec3 camera) {
        graph.reset();
        clipmap.add(graph, clip_camera);
        (void)ddgi.add(graph, clipmap, camera, lighting, settings);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        return cmd;
    };

    // Prime every probe, then overwrite both live atlases with the per-probe signatures.
    {
        auto cmd = record(camera_for(0, 0, 0));
        REQUIRE(device->submit_blocking(*cmd));
        REQUIRE(ddgi.stats().newly_primed == kChainProbes);
    }
    const auto seeded = chain_signatures();
    device->write_texture(ddgi.irradiance_atlas(), seeded[0].data(), seeded[0].size());
    device->write_texture(ddgi.visibility_atlas(), seeded[1].data(), seeded[1].size());

    const std::vector<std::array<int, 3>> shifts = {{1, 0, 0}, {-1, 0, 0}, {1, 1, -1}};
    const std::array<std::array<int, 3>, 3> cells = {{{1, 0, 0}, {0, 0, 0}, {1, 1, -1}}};
    const std::uint32_t expected_kept[3] = {12, 12, 4};
    std::array<rhi::SubmitTicket, 3> tickets{};
    for (std::size_t f = 0; f < 3; ++f) {
        auto cmd = record(camera_for(cells[f][0], cells[f][1], cells[f][2]));
        CHECK(ddgi.stats().grid_shifted);
        CHECK(ddgi.stats().kept_history == expected_kept[f]);
        tickets[f] = device->submit(std::move(cmd)); // no wait, no readback between frames
        REQUIRE(tickets[f].is_valid());
    }
    for (const rhi::SubmitTicket t : tickets)
        device->wait(t);

    const rhi::Extent2D irr_ext = ddgi.irradiance_atlas_extent();
    const rhi::Extent2D vis_ext = ddgi.visibility_atlas_extent();
    REQUIRE(irr_ext.width == kChainPerRow * kDdgiIrradianceTileSize);
    REQUIRE(vis_ext.width == kChainPerRow * kDdgiVisibilityTileSize);
    const auto irr =
        test::read_texture(*device, ddgi.irradiance_atlas(), irr_ext.width, irr_ext.height, 8);
    const auto vis =
        test::read_texture(*device, ddgi.visibility_atlas(), vis_ext.width, vis_ext.height, 8);

    const auto expected = chain_model(shifts);
    std::array<int, kChainProbes> observed{};
    int survivors = 0;
    for (std::uint32_t q = 0; q < kChainProbes; ++q) {
        observed[q] = classify_chain_tile(q, irr, vis, seeded);
        CAPTURE(q);
        CHECK(observed[q] == expected[q]);
        survivors += expected[q] >= 0 ? 1 : 0;
    }
    CHECK(survivors == 4); // the diagonal leaves exactly a 2x2x1 block of seeded history

    // Sensitivity control: the scene can tell the right remap from the plausible wrong ones. A
    // sign error, a remap that never moved anything, and a chain that dropped the middle shift
    // would each have produced a different, still self-consistent atlas — and this one is not it.
    const std::vector<std::array<int, 3>> negated = {{-1, 0, 0}, {1, 0, 0}, {-1, -1, 1}};
    const std::vector<std::array<int, 3>> dropped_middle = {{1, 0, 0}, {1, 1, -1}};
    CHECK(count_differences(observed, chain_model(negated)) == 8);
    CHECK(count_differences(observed, chain_model({})) == 18);
    CHECK(count_differences(observed, chain_model(dropped_middle)) == 4);

    CHECK(log.hazards_seen() == 0);
    CHECK(log.stale_states_seen() == 0);
}
