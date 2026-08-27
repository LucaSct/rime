// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"

// View-frustum culling (m13.2a, ADR-0035 §2a).
//
// Until now `extract_scene` emitted **literally every** `{WorldTransform, MeshRef, MaterialRef}` in
// the world — the ADR says so in as many words, and adds that this is "fine at six walls, not at a
// city block of per-part render leaves". A destructible block turns one wall into hundreds of parts
// and then into hundreds of debris chunks, most of them behind the camera at any moment. Submitting
// them all is not a small waste; it is the difference between a frame that fits its budget and one
// that does not, and no amount of GPU speed helps, because the cost is paid on the CPU packing
// draws that were never going to be seen.
//
// ── WHY THE COUNTERS ARE PART OF THE FEATURE, NOT A DEBUG EXTRA ───────────────────────────────
//
// ADR-0035 §2a names exactly this pair — "draws submitted vs. draws after frustum cull" — as the
// work-ledger entry m13.2 must add, and gives the reason: *"The cull counter exists so that culling
// degrading to 'culled 0 of 4,000' is a red number rather than a warm frame."*
//
// A cull that silently stops culling costs nothing visible. Every pixel is still right; the frame
// is merely slower, on a machine that is probably fast enough to hide it, until the scene grows and
// somebody wonders why. That is precisely M11's counting rule in its harder half — count the skips
// that STOP happening — and it is why `CullStats` is returned rather than logged.
//
// ── WHAT THIS IS NOT ──────────────────────────────────────────────────────────────────────────
//
//   * **Not occlusion culling.** A wall in front of a room does not cull the room. That needs a
//     depth pyramid and a per-object test against it, and it is a different brick with a different
//     cost model.
//   * **Not a spatial index.** The test is linear over the extracted draws, because extraction is
//     already linear over the world and adding a BVH would mean maintaining one against a world
//     where destruction changes the object set every tick. At the block's scale a plane test per
//     draw is a handful of FLOPs against a cache-friendly array; when a profile says otherwise,
//     the index goes in *under* this same interface.
//   * **Not conservative-exact.** A rotated object is tested by its axis-aligned world box, which
//     is larger than the object. That over-estimates, so it can only ever draw something that was
//     not strictly needed — never cull something visible. Erring in that direction is the only
//     acceptable one: a false cull is a hole in the picture.
namespace rime::render {

// The six planes of a view frustum, each `ax + by + cz + d = 0` with the normal pointing INWARD, so
// a point is inside iff it is on the positive side of all six.
//
// Order is fixed and worth knowing when debugging: left, right, bottom, top, near, far.
struct Frustum {
    core::Vec4 planes[6];
};

// Extract the frustum from a clip-from-world matrix (the Gribb–Hartmann method: each plane is a sum
// or difference of two ROWS of the matrix, because a clip-space inequality like `-w <= x` is, after
// substituting the matrix product, exactly a plane equation in world space).
//
// The near plane is `row2` alone rather than `row3 + row2`, because this engine targets **Vulkan
// clip space**, where z runs 0..w rather than -w..w (ADR-0004). Getting that wrong does not produce
// an obviously broken picture — it produces a near plane in the wrong place, which culls correctly
// almost everywhere and wrongly right in front of the camera.
//
// The planes are normalized, so `distance()` below is a true signed distance in world units. That
// costs six square roots per frame and buys the ability to reason about margins in metres.
[[nodiscard]] Frustum frustum_from_view_proj(const core::Mat4& view_proj) noexcept;

// Is an axis-aligned world box at least partly inside the frustum?
//
// The standard "positive vertex" test: for each plane, pick the box corner furthest along the
// plane's normal; if even that corner is behind the plane, every corner is, and the box is out.
// One dot product per plane, no branching per corner.
//
// Returns true for a box the test cannot rule out — including the classic false positive where a
// box straddles two planes' outside regions without intersecting the frustum. That costs a draw and
// never costs a pixel, which is the right side to be wrong on.
[[nodiscard]] bool aabb_in_frustum(const Frustum& frustum, core::Vec3 min, core::Vec3 max) noexcept;

// The world-space AABB of a local box under a transform — the conservative one: all eight corners
// transformed, then re-bounded. Cheaper closed forms exist for affine matrices, and they are worth
// having when this shows up in a profile; eight transforms is 24 multiply-adds and is not that yet.
void transform_aabb(const core::Mat4& model,
                    core::Vec3 local_min,
                    core::Vec3 local_max,
                    core::Vec3& out_min,
                    core::Vec3& out_max) noexcept;

// The ledger entry (ADR-0035 §2a). `submitted` is what survives to the GPU; `culled` is what the
// frustum removed. Their sum is what extraction produced.
//
// **A run where `culled` is 0 and `submitted` is large is the failure this exists to catch** — it
// means the cull has stopped culling, which is invisible in every pixel and in every frame time on
// a machine fast enough to absorb it.
struct CullStats {
    std::uint64_t submitted = 0;
    std::uint64_t culled = 0;

    [[nodiscard]] std::uint64_t considered() const noexcept { return submitted + culled; }
};

} // namespace rime::render
