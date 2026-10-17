// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the Vulkan descriptor-cache brick: the backend may promote uniform buffers to DYNAMIC
// descriptors and reuse identical descriptor sets within an encoder, and NOTHING a caller of the
// public RHI can observe may change — every claim below is a pixel (or storage-buffer word) read
// back through the public API, run twice: once with the cache on and once with
// RIME_VK_DESCRIPTOR_CACHE=0, which is the pre-cache path verbatim. Both runs must produce the same
// expected values.
//
// The cache's EFFECT is witnessed separately, without widening the RHI: with
// RIME_VK_DESCRIPTOR_STATS=1 the backend logs one "rhi.vk.descriptors:" counter line per encoder,
// and this test captures it through the core log sink. The counters are claims about behaviour,
// not mirrors of the implementation: "only the slice moved, so no new set was written", "a fresh
// encoder never hits a previous encoder's sets", "more distinct sets than the cache holds still
// draws every one correctly".
//
// The two runs share one process, so the environment variables are set before each device is
// created and restored afterwards (the backend reads them exactly once, at device creation).
//
// Not covered here, because no public-API fixture can observe it: whether a DIFFERENT explicit
// range on the same buffer produces different pixels (a shader reading inside both ranges sees the
// same bytes) — that case is covered by the counter witness only; and devices whose
// maxDescriptorSetUniformBuffersDynamic is below 2 (the spec floor is 8, so none exist).

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "descriptor_cache.comp.spv.h"
#include "descriptor_cache.frag.spv.h"
#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/rhi.hpp"
#include "ubo.frag.spv.h"
#include "ubo.vert.spv.h"

namespace {

using namespace rime::rhi;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

void put_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : ""); // "" removes the variable on Windows
#else
    if (value != nullptr)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

// Set an environment variable for one scope and put the previous value (or its absence) back.
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name); old != nullptr)
            old_ = old;
        put_env(name, value);
    }

    ~EnvGuard() { put_env(name_, old_ ? old_->c_str() : nullptr); }

    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

// Captures the backend's per-encoder stats lines; forwards everything else to stderr so a real
// error during the test is still visible. Restores the default sink and the log level on exit.
class StatsCapture {
public:
    StatsCapture() : old_level_(rime::core::log_level()) {
        rime::core::set_log_level(rime::core::LogLevel::Info); // the stats line is Info
        rime::core::set_log_sink([this](const rime::core::LogRecord& r) {
            if (r.message.starts_with("rhi.vk.descriptors:"))
                lines_.emplace_back(r.message);
            else
                std::fprintf(
                    stderr, "%.*s\n", static_cast<int>(r.message.size()), r.message.data());
        });
    }

    ~StatsCapture() {
        rime::core::set_log_sink({});
        rime::core::set_log_level(old_level_);
    }

    StatsCapture(const StatsCapture&) = delete;
    StatsCapture& operator=(const StatsCapture&) = delete;

    // The line the one encoder just submitted emitted; exactly one is expected per submission.
    std::string take() {
        REQUIRE(lines_.size() == 1);
        std::string line = std::move(lines_.front());
        lines_.clear();
        return line;
    }

private:
    rime::core::LogLevel old_level_;
    std::vector<std::string> lines_;
};

std::uint64_t stat(const std::string& line, std::string_view name) {
    const std::string needle = " " + std::string(name) + "=";
    const std::size_t at = line.find(needle);
    REQUIRE_MESSAGE(at != std::string::npos, "missing counter '" << name << "' in: " << line);
    return std::stoull(line.substr(at + needle.size()));
}

// 256 bytes is the worst-case minUniformBufferOffsetAlignment the spec allows, so every slice
// offset below is valid — and dynamically encodable — on every device without querying limits.
constexpr std::uint64_t kSlice = 256;

struct Rgba {
    int r, g, b, a;
};

// One render target plus its readback buffer; columns are addressed by a 1x1 scissor.
struct Target {
    TextureHandle color;
    BufferHandle readback;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> px;

    Rgba at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
        return {px[i + 0], px[i + 1], px[i + 2], px[i + 3]};
    }
};

void check_px(const Target& t, std::uint32_t x, std::uint32_t y, Rgba want) {
    const Rgba got = t.at(x, y);
    CAPTURE(x);
    CAPTURE(y);
    CHECK(std::abs(got.r - want.r) <= 1);
    CHECK(std::abs(got.g - want.g) <= 1);
    CHECK(std::abs(got.b - want.b) <= 2); // the LINEAR sample is a 0.3/0.7 blend
    CHECK(got.a == 255);                  // cleared to alpha 0: an undrawn pixel fails here
}

// Runs every scenario against a fresh device created under the CURRENT environment.
void run_scenarios(bool cache_on) {
    DeviceDesc desc{};
    desc.enable_validation = true; // Exercise dynamic offsets under validation even in Release.
    auto device = create_device(desc);
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping descriptor-cache proof");
        return;
    }
    StatsCapture stats;

    const auto shader = [&](ShaderStage stage, const auto& spv, const char* name) {
        ShaderDesc d{};
        d.stage = stage;
        d.spirv = spv;
        d.spirv_size_bytes = sizeof(spv);
        d.debug_name = name;
        return device->create_shader(d);
    };
    const ShaderHandle vsh = shader(ShaderStage::Vertex, ubo_vert_spv, "ubo.vert");
    const ShaderHandle ubo_fsh = shader(ShaderStage::Fragment, ubo_frag_spv, "ubo.frag");
    const ShaderHandle dc_fsh =
        shader(ShaderStage::Fragment, descriptor_cache_frag_spv, "descriptor_cache.frag");
    const ShaderHandle csh =
        shader(ShaderStage::Compute, descriptor_cache_comp_spv, "descriptor_cache.comp");

    // P: two uniform buffers + a texture, declared in DESCENDING, interleaved binding order so a
    // backend that fed dynamic offsets in declaration (or call) order would swap red and green.
    const BindingDesc p_bindings[] = {{3, BindingType::UniformBuffer, StageMask::Fragment},
                                      {2, BindingType::CombinedImageSampler, StageMask::Fragment},
                                      {1, BindingType::UniformBuffer, StageMask::Fragment}};
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = dc_fsh;
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.bindings = p_bindings;
    pd.debug_name = "descriptor-cache-P";
    const PipelineHandle pipe_p = device->create_graphics_pipeline(pd);

    // Q: the single-UBO pipeline of ubo_test (binding 0, whole vec4 color).
    const BindingDesc q_bindings[] = {{0, BindingType::UniformBuffer, StageMask::Fragment}};
    pd.fragment_shader = ubo_fsh;
    pd.bindings = q_bindings;
    pd.debug_name = "descriptor-cache-Q";
    const PipelineHandle pipe_q = device->create_graphics_pipeline(pd);

    // C: compute accumulate (UBO at 4, storage at 5).
    const BindingDesc c_bindings[] = {{4, BindingType::UniformBuffer, StageMask::Compute},
                                      {5, BindingType::StorageBuffer, StageMask::Compute}};
    ComputePipelineDesc cpd{};
    cpd.shader = csh;
    cpd.bindings = c_bindings;
    cpd.debug_name = "descriptor-cache-C";
    const PipelineHandle pipe_c = device->create_compute_pipeline(cpd);
    REQUIRE(pipe_p.is_valid());
    REQUIRE(pipe_q.is_valid());
    REQUIRE(pipe_c.is_valid());

    // U: four color slices; slice k is ((k+1)*51, 0, 0, 255) — red 51, 102, 153, 204.
    BufferDesc ubd{};
    ubd.size = 4 * kSlice;
    ubd.usage = BufferUsage::Uniform;
    ubd.memory = MemoryUsage::CpuToGpu;
    ubd.debug_name = "descriptor-cache-U";
    const BufferHandle ubo = device->create_buffer(ubd);
    for (std::uint32_t k = 0; k < 4; ++k) {
        const float c[4] = {static_cast<float>((k + 1) * 51) / 255.0f, 0.0f, 0.0f, 1.0f};
        device->write_buffer(ubo, c, sizeof(c), k * kSlice);
    }
    const auto slice = [](std::uint32_t k) { return k * kSlice; };
    const auto red = [](std::uint32_t k) { return static_cast<int>((k + 1) * 51); };

    // T1/T2: 2x1 textures whose texel 1 differs in blue (255 vs 100); texel 0 is blue 0.
    const auto make_tex = [&](std::uint8_t blue, const char* name) {
        TextureDesc td{};
        td.extent = {2, 1};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
        td.debug_name = name;
        const TextureHandle t = device->create_texture(td);
        const std::uint8_t texels[8] = {0, 0, 0, 255, 0, 0, blue, 255};
        device->write_texture(t, texels, sizeof(texels));
        return t;
    };
    const TextureHandle tex1 = make_tex(255, "descriptor-cache-T1");
    const TextureHandle tex2 = make_tex(100, "descriptor-cache-T2");
    const auto make_sampler = [&](Filter f) {
        SamplerDesc sd{};
        sd.mag_filter = f;
        sd.min_filter = f;
        sd.address_mode = AddressMode::ClampToEdge;
        return device->create_sampler(sd);
    };
    const SamplerHandle nearest = make_sampler(Filter::Nearest);
    const SamplerHandle linear = make_sampler(Filter::Linear);
    // u = 0.6 on two texels: NEAREST -> texel 1; LINEAR -> 0.3*texel0 + 0.7*texel1.
    constexpr int kT1Nearest = 255, kT2Nearest = 100, kT1Linear = 178;

    std::vector<Target> targets;
    const auto make_target = [&](std::uint32_t w, std::uint32_t h) {
        Target t;
        t.width = w;
        t.height = h;
        TextureDesc td{};
        td.extent = {w, h};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        td.debug_name = "descriptor-cache-target";
        t.color = device->create_texture(td);
        BufferDesc rbd{};
        rbd.size = static_cast<std::uint64_t>(w) * h * 4;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = "descriptor-cache-readback";
        t.readback = device->create_buffer(rbd);
        return t;
    };
    const auto begin_pass = [&](CommandBuffer& cmd, const Target& t) {
        RenderingInfo ri{};
        ri.color.target = t.color;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {0.0f, 0.0f, 0.0f, 0.0f}; // alpha 0: every drawn pixel writes alpha 1
        cmd.begin_rendering(ri);
    };
    const auto use = [&](CommandBuffer& cmd, PipelineHandle pipe, const Target& t) {
        cmd.bind_pipeline(pipe);
        Viewport vp{};
        vp.width = static_cast<float>(t.width);
        vp.height = static_cast<float>(t.height);
        vp.max_depth = 1.0f;
        cmd.set_viewport(vp);
    };
    const auto draw_at = [&](CommandBuffer& cmd, std::uint32_t x, std::uint32_t y) {
        Rect2D sc{};
        sc.x = static_cast<std::int32_t>(x);
        sc.y = static_cast<std::int32_t>(y);
        sc.width = 1;
        sc.height = 1;
        cmd.set_scissor(sc);
        cmd.draw(3);
    };
    // Submit, read every target back, and return the encoder's one stats line.
    const auto submit = [&](std::unique_ptr<CommandBuffer> cmd, std::vector<Target*> ts) {
        for (Target* t : ts)
            cmd->copy_texture_to_buffer(t->color, t->readback);
        REQUIRE(device->submit_blocking(*cmd));
        for (Target* t : ts) {
            t->px.resize(static_cast<std::size_t>(t->width) * t->height * 4);
            device->read_buffer(t->readback, t->px.data(), t->px.size(), 0);
        }
        cmd.reset();
        const std::string line = stats.take();
        CHECK(line.find(cache_on ? "cache=on" : "cache=off") != std::string::npos);
        // Every bind is either a cache hit or a freshly written set — nothing else.
        CHECK(stat(line, "binds") == stat(line, "allocations") + stat(line, "hits"));
        return line;
    };

    {
        INFO("two dynamic UBOs bound in unsorted order; offsets, texture and sampler vary");
        Target t = make_target(5, 1);
        auto cmd = device->begin_commands();
        begin_pass(*cmd, t);
        use(*cmd, pipe_p, t);
        // Column 0. Bound 3-then-1: caller order must not matter either.
        cmd->bind_uniform_buffer(3, ubo, slice(3), 16);
        cmd->bind_uniform_buffer(1, ubo, slice(0), 16);
        cmd->bind_texture(2, tex1, nearest);
        draw_at(*cmd, 0, 0);
        // Column 1: ONLY the two slices moved — with the cache on this must be a re-bind of the
        // same set with new dynamic offsets, and still read the new slices.
        cmd->bind_uniform_buffer(3, ubo, slice(2), 16);
        cmd->bind_uniform_buffer(1, ubo, slice(1), 16);
        draw_at(*cmd, 1, 0);
        // Column 2: a different texture.
        cmd->bind_texture(2, tex2, nearest);
        draw_at(*cmd, 2, 0);
        // Column 3: column 0's buffers and texture, a different sampler.
        cmd->bind_uniform_buffer(3, ubo, slice(3), 16);
        cmd->bind_uniform_buffer(1, ubo, slice(0), 16);
        cmd->bind_texture(2, tex1, linear);
        draw_at(*cmd, 3, 0);
        // Column 4: exactly column 0 again.
        cmd->bind_texture(2, tex1, nearest);
        draw_at(*cmd, 4, 0);
        cmd->end_rendering();
        const std::string line = submit(std::move(cmd), {&t});

        check_px(t, 0, 0, {red(0), red(3), kT1Nearest, 255});
        check_px(t, 1, 0, {red(1), red(2), kT1Nearest, 255});
        check_px(t, 2, 0, {red(1), red(2), kT2Nearest, 255});
        check_px(t, 3, 0, {red(0), red(3), kT1Linear, 255});
        check_px(t, 4, 0, {red(0), red(3), kT1Nearest, 255});

        CHECK(stat(line, "draws") == 5);
        CHECK(stat(line, "binds") == 5);
        CHECK(stat(line, "static_fallback") == 0);
        if (cache_on) {
            // Three distinct descriptor contents; column 1 can only be a hit (it follows its key
            // directly), column 4 is one unless a three-way bucket collision evicted column 0.
            CHECK(stat(line, "allocations") >= 3);
            CHECK(stat(line, "allocations") <= 4);
            CHECK(stat(line, "dynamic_binds") == 5);
        } else {
            CHECK(stat(line, "allocations") == 5);
            CHECK(stat(line, "hits") == 0);
            CHECK(stat(line, "dynamic_binds") == 0);
        }
        targets.push_back(std::move(t));
    }

    {
        INFO("explicit range vs size 0 (through the end of the buffer) on one buffer");
        Target t = make_target(4, 1);
        auto cmd = device->begin_commands();
        begin_pass(*cmd, t);
        use(*cmd, pipe_q, t);
        cmd->bind_uniform_buffer(0, ubo, slice(0), 16); // explicit range
        draw_at(*cmd, 0, 0);
        cmd->bind_uniform_buffer(0, ubo, slice(0), 32); // same bytes, different range
        draw_at(*cmd, 1, 0);
        // size 0 at a NONZERO offset: "from slice 1 to the end". A whole-size descriptor cannot
        // be rebased to offset 0 (its range would then cover past the end), so the backend must
        // keep the offset in the descriptor — and the draw must still read slice 1.
        cmd->bind_uniform_buffer(0, ubo, slice(1), 0);
        draw_at(*cmd, 2, 0);
        cmd->bind_uniform_buffer(0, ubo, slice(1), 16); // explicit again: column 0's descriptor
        draw_at(*cmd, 3, 0);
        cmd->end_rendering();
        const std::string line = submit(std::move(cmd), {&t});

        check_px(t, 0, 0, {red(0), 0, 0, 255});
        check_px(t, 1, 0, {red(0), 0, 0, 255});
        check_px(t, 2, 0, {red(1), 0, 0, 255});
        check_px(t, 3, 0, {red(1), 0, 0, 255});

        if (cache_on) {
            // Ranges 16 and 32 must be different sets (the range IS descriptor content), and the
            // size-0 bind is the one that cannot be dynamic.
            CHECK(stat(line, "allocations") >= 3);
            CHECK(stat(line, "static_fallback") == 1);
        } else {
            CHECK(stat(line, "allocations") == 4);
            CHECK(stat(line, "static_fallback") == 0);
        }
        targets.push_back(std::move(t));
    }

    {
        INFO("A->B->A pipelines, graphics and compute bind points, compute RMW through a hit");
        BufferDesc cbd{};
        cbd.size = 2 * kSlice;
        cbd.usage = BufferUsage::Uniform;
        cbd.memory = MemoryUsage::CpuToGpu;
        cbd.debug_name = "descriptor-cache-ops";
        const BufferHandle ops = device->create_buffer(cbd);
        const std::uint32_t op0[4] = {5, 0, 0, 0}; // add 5 to element 0
        const std::uint32_t op1[4] = {7, 1, 0, 0}; // add 7 to element 1
        device->write_buffer(ops, op0, sizeof(op0), 0);
        device->write_buffer(ops, op1, sizeof(op1), kSlice);

        BufferDesc sbd{};
        sbd.size = 4 * sizeof(std::uint32_t);
        sbd.usage = BufferUsage::Storage;
        sbd.memory = MemoryUsage::GpuToCpu;
        sbd.debug_name = "descriptor-cache-acc";
        const BufferHandle acc = device->create_buffer(sbd);
        const std::uint32_t zeros[4] = {0, 0, 0, 0};
        device->write_buffer(acc, zeros, sizeof(zeros), 0);

        Target t = make_target(3, 1);
        auto cmd = device->begin_commands();
        cmd->bind_compute_pipeline(pipe_c);
        cmd->bind_uniform_buffer(4, ops, 0, 16);
        cmd->bind_storage_buffer(5, acc);
        cmd->dispatch(1, 1, 1);
        // Same attachments re-bound: the second dispatch reuses the set and must see the first's
        // write (the set names memory; the post-dispatch barrier orders it).
        cmd->bind_uniform_buffer(4, ops, 0, 16);
        cmd->dispatch(1, 1, 1);

        begin_pass(*cmd, t);
        use(*cmd, pipe_p, t);
        cmd->bind_uniform_buffer(1, ubo, slice(0), 16);
        cmd->bind_texture(2, tex1, nearest);
        cmd->bind_uniform_buffer(3, ubo, slice(3), 16);
        draw_at(*cmd, 0, 0);
        use(*cmd, pipe_q, t);
        cmd->bind_uniform_buffer(0, ubo, slice(1), 16);
        draw_at(*cmd, 1, 0);
        use(*cmd, pipe_p, t); // back to A; its attachments persisted through B untouched
        draw_at(*cmd, 2, 0);
        cmd->end_rendering();

        // Back to the compute bind point, a different slice of the same buffer.
        cmd->bind_compute_pipeline(pipe_c);
        cmd->bind_uniform_buffer(4, ops, kSlice, 16);
        cmd->dispatch(1, 1, 1);
        const std::string line = submit(std::move(cmd), {&t});

        check_px(t, 0, 0, {red(0), red(3), kT1Nearest, 255});
        check_px(t, 1, 0, {red(1), 0, 0, 255});
        check_px(t, 2, 0, {red(0), red(3), kT1Nearest, 255});

        std::uint32_t got[4] = {};
        device->read_buffer(acc, got, sizeof(got), 0);
        CHECK(got[0] == 10u);
        CHECK(got[1] == 7u);
        CHECK(got[2] == 0u);

        CHECK(stat(line, "draws") == 6);
        CHECK(stat(line, "binds") == 6);
        if (cache_on) {
            CHECK(stat(line, "allocations") >= 3); // C, P, Q
            CHECK(stat(line, "hits") >= 1);        // the re-bound dispatch, at least
        } else {
            CHECK(stat(line, "allocations") == 6);
        }
        device->destroy(acc);
        device->destroy(ops);
        targets.push_back(std::move(t));
    }

    {
        INFO("more distinct sets than the cache holds: evictions never corrupt recorded draws");
        // 300 columns, each with its own descriptor contents (a distinct explicit range), drawn
        // twice in one encoder: row 0 in order, row 1 in reverse. 300 > the 256-entry cache.
        // Pool maxSets is not a portable allocation-failure boundary on Vulkan 1.1+:
        // https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateDescriptorSets.html
        // Drivers may satisfy these allocations in one pool; assert cache eviction and pixels.
        constexpr std::uint32_t kCols = 300;
        const auto color = [](std::uint32_t i) {
            return Rgba{static_cast<int>((i & 15u) * 16u + 8u),
                        static_cast<int>((i >> 4) * 12u + 8u),
                        0,
                        255};
        };
        BufferDesc rbd{};
        rbd.size = kCols * kSlice + 16 * kCols;
        rbd.usage = BufferUsage::Uniform;
        rbd.memory = MemoryUsage::CpuToGpu;
        rbd.debug_name = "descriptor-cache-many";
        const BufferHandle many = device->create_buffer(rbd);
        for (std::uint32_t i = 0; i < kCols; ++i) {
            const Rgba c = color(i);
            const float f[4] = {
                static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f, 0.0f, 1.0f};
            device->write_buffer(many, f, sizeof(f), i * kSlice);
        }

        Target t = make_target(kCols, 2);
        auto cmd = device->begin_commands();
        begin_pass(*cmd, t);
        use(*cmd, pipe_q, t);
        for (std::uint32_t row = 0; row < 2; ++row) {
            for (std::uint32_t x = 0; x < kCols; ++x) {
                const std::uint32_t i = row == 0 ? x : kCols - 1 - x;
                cmd->bind_uniform_buffer(0, many, i * kSlice, 16ull * (i + 1));
                draw_at(*cmd, x, row);
            }
        }
        cmd->end_rendering();
        const std::string line = submit(std::move(cmd), {&t});

        for (std::uint32_t x = 0; x < kCols; ++x) {
            check_px(t, x, 0, color(x));
            check_px(t, x, 1, color(kCols - 1 - x));
        }
        CHECK(stat(line, "binds") == 2 * kCols);
        CHECK(stat(line, "pools") >= 1);
        if (cache_on) {
            CHECK(stat(line, "allocations") >= kCols);
            CHECK(stat(line, "evictions") >= 1);
        } else {
            CHECK(stat(line, "allocations") == 2 * kCols);
            CHECK(stat(line, "evictions") == 0);
        }
        device->destroy(many);
        targets.push_back(std::move(t));
    }

    {
        INFO("repeated submissions recycle the pools; a new encoder never hits an old set");
        // Submission 1 caches T1's set. Its pool is reset and handed out again, so submission 2's
        // FIRST allocation (T2) very likely reuses that very VkDescriptorSet. If the cache
        // survived the encoder, submission 2's T1 draw would hit the stale entry and sample T2.
        for (int sub = 0; sub < 3; ++sub) {
            CAPTURE(sub);
            Target t = make_target(2, 1);
            auto cmd = device->begin_commands();
            begin_pass(*cmd, t);
            use(*cmd, pipe_p, t);
            cmd->bind_uniform_buffer(1, ubo, slice(0), 16);
            cmd->bind_uniform_buffer(3, ubo, slice(3), 16);
            if (sub > 0) {
                cmd->bind_texture(2, tex2, nearest);
                draw_at(*cmd, 0, 0);
            }
            cmd->bind_texture(2, tex1, nearest);
            draw_at(*cmd, 1, 0);
            cmd->end_rendering();
            const std::string line = submit(std::move(cmd), {&t});

            if (sub > 0)
                check_px(t, 0, 0, {red(0), red(3), kT2Nearest, 255});
            check_px(t, 1, 0, {red(0), red(3), kT1Nearest, 255});
            CHECK(stat(line, "hits") == 0);
            CHECK(stat(line, "allocations") == (sub > 0 ? 2u : 1u));
            targets.push_back(std::move(t));
        }
    }

    for (Target& t : targets) {
        device->destroy(t.readback);
        device->destroy(t.color);
    }
    device->destroy(linear);
    device->destroy(nearest);
    device->destroy(tex2);
    device->destroy(tex1);
    device->destroy(ubo);
    device->destroy(pipe_c);
    device->destroy(pipe_q);
    device->destroy(pipe_p);
    device->destroy(csh);
    device->destroy(dc_fsh);
    device->destroy(ubo_fsh);
    device->destroy(vsh);
}

} // namespace

TEST_CASE("vulkan descriptor cache: pixels identical with the cache on and off") {
    EnvGuard stats_env("RIME_VK_DESCRIPTOR_STATS", "1");
    for (const bool cache_on : {true, false}) {
        CAPTURE(cache_on);
        EnvGuard cache_env("RIME_VK_DESCRIPTOR_CACHE", cache_on ? "1" : "0");
        run_scenarios(cache_on);
    }
}
