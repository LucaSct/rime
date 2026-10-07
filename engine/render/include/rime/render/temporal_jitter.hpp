// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"

// Camera jitter for temporal anti-aliasing (ADR-0078, step 1b).
//
// TAA works by rendering the SAME scene every frame with the sampling grid nudged by a different
// sub-pixel amount, then averaging the frames. Each frame samples a slightly different point inside
// every pixel, so after N frames the average has seen N distinct sub-pixel positions: edges that
// were a hard step become a smooth ramp, and thin features stop flickering in and out. This header
// is only the "nudge" half. It lands switched OFF and changes no pixel; the motion-vector and
// resolve bricks consume it. (Jitter with no resolve is strictly worse than none: the image
// wobbles half a pixel per frame with nothing averaging it away.)
//
// ---- Why Halton, and not random offsets ---------------------------------------------------------
// The Halton sequence is a LOW-DISCREPANCY sequence: each new point is placed in the largest gap
// left by the previous ones (base 2 bisects the interval, base 3 trisects it). Random offsets
// clump -- after 8 random frames two samples often land almost on top of each other while a whole
// quadrant of the pixel is never visited, so the average converges slowly and noisily. Halton
// guarantees the 8 frames spread out over the pixel. Two different prime bases (2, 3) for x and y
// keep the two axes uncorrelated, so the points fill the 2D pixel rather than lying on a line.
//
// ---- Why a period of 8, not 16 ------------------------------------------------------------------
// Sixteen would be smoother once converged. But destruction invalidates the history constantly in
// this engine -- a collapsing building rejects its own history every frame -- so reaching a usable
// average in 8 frames beats a better average at 16 that we will rarely be allowed to reach.
//
// ---- Why the PROJECTION is jittered and never the view ------------------------------------------
// Jittering the view matrix moves the camera. That moves the shadow-map frusta and every
// view-dependent term (specular, fog, SSR rays), and they would all crawl. Jittering the projection
// moves only where the scene lands on the pixel grid, which is the entire intent.
//
// ---- Why the culling frustum stays UNJITTERED ---------------------------------------------------
// The frustum is derived from a view-projection to decide which draws are visible. If it came from
// the jittered matrix it would wobble by half a pixel every frame, and objects sitting exactly on
// its edge would pop in and out -- a new flicker caused by the fix for flicker. Culling only needs
// the true camera, so SceneRenderer keeps an explicitly unjittered view-projection for it (and for
// the motion-vector brick, so a velocity is pure geometric motion rather than geometry plus jitter).
namespace rime::render {

// A sub-pixel camera offset sequence. Hold one per view; advance it once per rendered frame. It is
// a plain value with no statics and no clock: two peers rendered in one process each own one and
// never share state.
class TemporalJitter {
public:
    // Frames before the pattern repeats.
    static constexpr std::uint32_t kPeriod = 8;

    // Sub-pixel offset for frame `index`, in PIXELS, each component in [-0.5, 0.5). Positive x
    // moves the rendered image toward +x (right), positive y toward +y (down, the framebuffer's
    // y). The sequence is centred so its mean over one period is zero: a biased sequence would
    // shift the whole image permanently. Note that "Halton - 0.5" is NOT zero-mean over 8 frames
    // (8 base-2 points average 0.5703, not 0.5), so the centre is the period's own mean.
    [[nodiscard]] static core::Vec2 offset_for(std::uint64_t index) noexcept;

    [[nodiscard]] core::Vec2 current() const noexcept { return offset_for(index_); }
    void advance() noexcept { index_ = (index_ + 1) % kPeriod; }
    void reset() noexcept { index_ = 0; }
    [[nodiscard]] std::uint64_t index() const noexcept { return index_; }

private:
    std::uint64_t index_ = 0;
};

// `proj` as `core::perspective` returns it, with a sub-pixel shift of `offset_px` applied.
// A zero offset (or a zero-sized target) returns `proj` bit-identically.
//
// How: `core::Mat4` is column-major with v' = M v, so row 3 of a perspective matrix is the
// clip-space w. NDC x = clip.x / clip.w; to move the image by `d` pixels we need NDC to move by
// 2d/width (NDC spans 2 units across `width` pixels), i.e. clip.x += (2d/width) * clip.w --
// "add that multiple of row 3 to row 0". Likewise row 1 for y; Vulkan's NDC y and framebuffer y
// both point down, so +y pixels is +y NDC and no flip is needed here.
[[nodiscard]] core::Mat4 jitter_projection(const core::Mat4& proj,
                                           core::Vec2 offset_px,
                                           std::uint32_t width,
                                           std::uint32_t height) noexcept;

// The radical-inverse Halton sequence, exposed because it is independently testable against
// published values. `index` is 1-based: halton(1, 2) == 0.5.
[[nodiscard]] float halton(std::uint32_t index, std::uint32_t base) noexcept;

} // namespace rime::render
