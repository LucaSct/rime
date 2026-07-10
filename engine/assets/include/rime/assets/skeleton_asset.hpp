// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/transform.hpp"

// A skeleton: the joint hierarchy and bind pose a skinned mesh deforms against (M6.7, AN0). This is
// the *import* half of skeletal animation — the cooked runtime form. Per-frame GPU palette skinning
// is AN1 (M7); here the data model and the CPU sampler (clip_asset.hpp) exist and are proven
// GPU-free, because the cooked formats must be settled before either the cooker or the renderer can
// stand on them (the M6.1 reader-before-cooker discipline).
//
// The skinning equation this exists to feed (derived in docs/math/skinning.md): a vertex v bound to
// joint j with weight w is deformed to  Σ_j  w_j · (world_j · inverseBind_j) · v.  `inverseBind_j`
// takes v from model space into joint j's *bind-local* space (where the joint sits at its rest
// pose); `world_j` is the joint's *animated* world placement. Their product is one entry of the
// "palette" the sampler produces and the GPU (later) reads.
namespace rime::assets {

// The most joints a skeleton (or a clip bound to one) may declare. Skinned-mesh vertices reference
// joints with u16 indices, so a joint beyond 65536 could never be addressed by a vertex — the cap
// is that natural ceiling, not an arbitrary limit. The cooked-clip reader also uses it to bound the
// dense per-joint table it allocates from a file's declared joint count (a corrupt count can't ask
// for a runaway allocation) before the loader has a skeleton to cross-check against.
inline constexpr std::uint32_t kMaxJoints = 65536;

// One joint (bone). Joints are stored in a **topological order** — every joint's parent has a
// smaller index than the joint itself — so a single forward pass composes local poses into world
// poses without recursion or a work queue. A root joint has parent == kNoParent.
struct Joint {
    static constexpr std::int32_t kNoParent = -1;

    std::int32_t parent = kNoParent; // index into Skeleton::joints, or kNoParent for a root
    std::uint64_t name_hash = 0;     // FNV-1a of the joint's name — lookup without storing strings
    core::Mat4 inverse_bind{};       // model space → this joint's bind-local space
    core::Transform local_bind{};    // the joint's default local pose (used where a clip is silent)
};

// A skeleton is just its joints (hierarchy + bind pose). A skinning palette has one Mat4 per joint
// in this same order: palette[i] corresponds to joints[i].
struct Skeleton {
    std::vector<Joint> joints;

    [[nodiscard]] std::size_t joint_count() const noexcept { return joints.size(); }

    // Find a joint by its name's FNV-1a hash; returns Joint::kNoParent (-1) if absent. A linear
    // scan: skeletons are small (tens to low-hundreds of joints) and this is a load-time / tooling
    // convenience (the cooker resolves animation targets to joint indices), never a per-frame path.
    [[nodiscard]] std::int32_t find(std::uint64_t name_hash) const noexcept {
        for (std::size_t i = 0; i < joints.size(); ++i) {
            if (joints[i].name_hash == name_hash) {
                return static_cast<std::int32_t>(i);
            }
        }
        return Joint::kNoParent;
    }

    // True if the joints are in valid topological order (every parent precedes its child, and every
    // parent index is in range). The sampler relies on this; the cooker guarantees it, and the
    // reader will validate it. Exposed so tests and the future loader can assert it cheaply.
    [[nodiscard]] bool is_topologically_ordered() const noexcept {
        for (std::size_t i = 0; i < joints.size(); ++i) {
            const std::int32_t p = joints[i].parent;
            if (p != Joint::kNoParent &&
                (p < 0 || static_cast<std::size_t>(p) >= i)) { // parent must exist and precede i
                return false;
            }
        }
        return true;
    }
};

} // namespace rime::assets
