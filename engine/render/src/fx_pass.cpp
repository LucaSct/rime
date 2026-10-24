// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/fx_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "particle.frag.spv.h"
#include "particle.vert.spv.h"
#include "rime/render/passes.hpp" // kHdrFormat / kDepthFormat — one place decides the formats

namespace rime::render {
namespace {

[[nodiscard]] rhi::ShaderHandle make_shader(rhi::Device& device,
                                            rhi::ShaderStage stage,
                                            const std::uint32_t* words,
                                            std::size_t bytes,
                                            std::string_view name) {
    rhi::ShaderDesc sd{};
    sd.stage = stage;
    sd.spirv = words;
    sd.spirv_size_bytes = bytes;
    sd.debug_name = name;
    return device.create_shader(sd);
}

// The push block, mirroring particle.vert's. 112 bytes — inside the 128-byte floor every Vulkan
// implementation guarantees, so this needs no capability check.
struct FxPush {
    core::Mat4 view_proj;
    float right[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float up[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float depth[4] = {}; // NDC depth = A + B / view distance; fade width and enable
};

static_assert(sizeof(FxPush) == 112, "FxPush must match particle.vert's push_constant block");

} // namespace

FxParticlePass::FxParticlePass(rhi::Device& device, std::uint32_t max_particles, FxBlendMode blend)
    : device_(device), blend_(blend), capacity_(max_particles) {
    vertex_shader_ = make_shader(device,
                                 rhi::ShaderStage::Vertex,
                                 particle_vert_spv,
                                 sizeof(particle_vert_spv),
                                 "particle.vert");
    fragment_shader_ = make_shader(device,
                                   rhi::ShaderStage::Fragment,
                                   particle_frag_spv,
                                   sizeof(particle_frag_spv),
                                   "particle.frag");

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };

    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = kHdrFormat;  // blend into HDR, before tonemap — see the header
    pd.cull = rhi::CullMode::None; // a billboard has no meaningful winding
    pd.blend = blend_ == FxBlendMode::SmokeAlpha ? rhi::BlendMode::Alpha : rhi::BlendMode::Additive;
    // TEST, DO NOT WRITE. Particles must be occluded by the world (a puff behind a wall is behind
    // it) and must never occlude anything. Smoke sorts its CPU instances back to front because
    // alpha compositing depends on order; additive instances retain caller order.
    pd.depth_test = true;
    pd.depth_write = false;
    pd.depth_compare = rhi::CompareOp::LessEqual;
    pd.depth_format = kDepthFormat;
    pd.bindings = bindings;
    // The pipeline layout must DECLARE the push range the shader statically uses; without it
    // Vulkan refuses the layout and every draw is a validation error. (It was: "uses push-constant
    // statically but vkCmdPushConstants was not called", which points at the call site rather than
    // at the missing declaration.)
    pd.push_constant_size = sizeof(FxPush);
    pd.debug_name = blend_ == FxBlendMode::SmokeAlpha ? "fx-smoke"
                    : blend_ == FxBlendMode::Flame    ? "fx-flame"
                                                      : "fx-particles";
    pipeline_ = device.create_graphics_pipeline(pd);

    rhi::SamplerDesc sampler{};
    sampler.min_filter = sampler.mag_filter = rhi::Filter::Nearest;
    sampler.address_mode = rhi::AddressMode::ClampToEdge;
    depth_sampler_ = device_.create_sampler(sampler);
    (void)set_frames_in_flight(2);
}

bool FxParticlePass::set_frames_in_flight(std::uint32_t frames) {
    if (submitted_ || frames == std::numeric_limits<std::uint32_t>::max())
        return false;
    for (rhi::BufferHandle instance : instances_)
        device_.destroy(instance);
    instances_.clear();
    next_slot_ = 0;
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(std::max(capacity_, 1u)) * sizeof(GpuParticle);
    bd.usage = rhi::BufferUsage::Storage;
    // CpuToGpu: this buffer is rewritten wholesale every frame the pass runs, which is the case
    // host-visible memory exists for. A device-local buffer plus a staging copy would be faster to
    // read and slower to write, and the read side here is one fetch per particle per vertex.
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.debug_name = blend_ == FxBlendMode::Flame ? "fx-flame-instances" : "fx-particle-instances";
    for (std::uint32_t i = 0; i < frames + 1; ++i)
        instances_.push_back(device_.create_buffer(bd));
    return true;
}

FxParticlePass::~FxParticlePass() {
    for (rhi::BufferHandle instance : instances_)
        device_.destroy(instance);
    device_.destroy(depth_sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void FxParticlePass::add(RenderGraph& graph,
                         RGTexture hdr,
                         RGTexture depth,
                         std::span<const GpuParticle> particles,
                         const FxView& view,
                         const FxSettings& settings) {
    // THE GATE, and it is a structural one. Not declaring the pass means no pipeline bind, no
    // barrier, and no attachment load — so `enabled = false` leaves the frame byte-identical to a
    // build without this file in it. A pass that ran and drew zero instances would be *almost*
    // that, and a regression bridge cannot be built on almost (ADR-0032 §11, ADR-0035 §5).
    if (!settings.enabled || particles.empty()) {
        return;
    }

    const std::uint32_t budget = std::min(capacity_, settings.max_particles);
    // The global intensity is folded in HERE rather than in the shader, so the shader stays a pure
    // function of what it is handed and the CPU keeps one place where "how bright is FX" is
    // decided. `intensity = 0` therefore draws the pass with black particles — which is
    // deliberately NOT the same as the gate being off, and the proof tells the two apart.
    std::vector<GpuParticle> scaled;
    scaled.reserve(std::min<std::size_t>(particles.size(), budget));
    const float scale = std::max(settings.intensity, 0.0f);
    if (blend_ != FxBlendMode::Additive) {
        const core::Vec3 back = core::cross(view.right, view.up);
        const auto finite3 = [](core::Vec3 v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const double back_length_sq = static_cast<double>(back.x) * back.x +
                                      static_cast<double>(back.y) * back.y +
                                      static_cast<double>(back.z) * back.z;
        if (!finite3(view.right) || !finite3(view.up) || !finite3(back) ||
            !(back_length_sq > 1.0e-12) || !std::isfinite(settings.intensity)) {
            dropped_ += particles.size();
            return;
        }
        for (const GpuParticle& particle : particles) {
            bool valid =
                std::isfinite(particle.position_size[3]) && particle.position_size[3] >= 0.0f;
            for (float value : particle.position_size)
                valid = valid && std::isfinite(value);
            for (float value : particle.color_alpha)
                valid = valid && std::isfinite(value);
            if (!valid || scaled.size() == budget) {
                ++dropped_;
                continue;
            }
            GpuParticle candidate = particle;
            candidate.color_alpha[0] *= scale;
            candidate.color_alpha[1] *= scale;
            candidate.color_alpha[2] *= scale;
            if (!std::isfinite(candidate.color_alpha[0]) ||
                !std::isfinite(candidate.color_alpha[1]) ||
                !std::isfinite(candidate.color_alpha[2])) {
                ++dropped_;
                continue;
            }
            scaled.push_back(candidate);
        }
        // Only straight-alpha smoke needs sorting; flame is additive and retains caller order.
        if (blend_ == FxBlendMode::SmokeAlpha)
            std::stable_sort(
                scaled.begin(), scaled.end(), [back](const GpuParticle& a, const GpuParticle& b) {
                    const auto projection = [back](const GpuParticle& p) {
                        return static_cast<double>(p.position_size[0]) * back.x +
                               static_cast<double>(p.position_size[1]) * back.y +
                               static_cast<double>(p.position_size[2]) * back.z;
                    };
                    return projection(a) < projection(b);
                });
    } else {
        const std::uint32_t count = static_cast<std::uint32_t>(
            std::min<std::size_t>(particles.size(), static_cast<std::size_t>(budget)));
        dropped_ += particles.size() - count;
        scaled.assign(particles.begin(), particles.begin() + count);
        for (GpuParticle& particle : scaled) {
            particle.color_alpha[0] *= scale;
            particle.color_alpha[1] *= scale;
            particle.color_alpha[2] *= scale;
        }
    }
    if (scaled.empty())
        return;
    const std::uint32_t count = static_cast<std::uint32_t>(scaled.size());
    const rhi::BufferHandle instances = instances_[next_slot_];
    next_slot_ = (next_slot_ + 1) % static_cast<std::uint32_t>(instances_.size());
    device_.write_buffer(instances, scaled.data(), scaled.size() * sizeof(GpuParticle), 0);
    submitted_ = true;
    drawn_ += count;

    FxPush push{};
    push.view_proj = view.view_proj;
    push.right[0] = view.right.x;
    push.right[1] = view.right.y;
    push.right[2] = view.right.z;
    push.right[3] = blend_ == FxBlendMode::SmokeAlpha ? 1.0f
                    : blend_ == FxBlendMode::Flame    ? 2.0f
                                                      : 0.0f;
    push.up[0] = view.up.x;
    push.up[1] = view.up.y;
    push.up[2] = view.up.z;
    // Perspective depth reconstruction: rows 2 and 3 of clip-from-world share the camera's
    // forward axis. Their ratio gives A, and subtracting translation gives B in z = A + B/w.
    // This recovers metric view distance without assuming the camera's near/far planes. For
    // orthographic views (constant w), the row-2 length directly converts NDC gaps to metres.
    const auto& m = view.view_proj;
    const core::Vec3 z{m.at(2, 0), m.at(2, 1), m.at(2, 2)};
    const core::Vec3 w{m.at(3, 0), m.at(3, 1), m.at(3, 2)};
    const float w2 = core::dot(w, w);
    if (w2 > 1.0e-8f) {
        push.depth[0] = core::dot(z, w) / w2;
        push.depth[1] = m.at(2, 3) - push.depth[0] * m.at(3, 3);
        push.depth[3] = 1.0f;
    } else {
        push.depth[1] = std::abs(m.at(3, 3)) / std::max(core::length(z), 1.0e-8f);
    }
    push.depth[2] = 0.18f;

    // LOAD the HDR target — this pass blends with a frame the forward pass already shaded, so a
    // Clear here would erase it. Depth is loaded READ-ONLY, which is what tells the graph this pass
    // only tests against it and lets it order without a write-after-write barrier.
    const RGColorAttachment colors[] = {{hdr, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    RGDepthAttachment depth_att{};
    depth_att.texture = depth;
    depth_att.load = rhi::LoadOp::Load;
    // Later translucent passes load the same depth. Read-only prevents shader writes, but
    // DontCare would still allow the render-pass store operation to discard the attachment.
    depth_att.store = rhi::StoreOp::Store;
    depth_att.read_only = true;

    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    const RGTexture sampled[] = {depth};
    desc.sampled = sampled;

    const rhi::PipelineHandle pipeline = pipeline_;
    const std::uint32_t instance_count = count;
    const rhi::SamplerHandle sampler = depth_sampler_;
    graph.add_raster_pass(blend_ == FxBlendMode::SmokeAlpha ? "fx-smoke"
                          : blend_ == FxBlendMode::Flame    ? "fx-flame"
                                                            : "fx-particles",
                          desc,
                          [pipeline, instances, instance_count, push, sampler, depth, &graph](
                              rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipeline);
                              cmd.bind_storage_buffer(0, instances);
                              cmd.bind_texture(1, graph.physical(depth), sampler);
                              cmd.push_constants(&push, sizeof(push));
                              // Six vertices synthesised per particle; the vertex stage reads the
                              // array by gl_InstanceIndex. No vertex buffer is bound because there
                              // is nothing to bind.
                              cmd.draw(6, instance_count);
                          });
}

} // namespace rime::render
