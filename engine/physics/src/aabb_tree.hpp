// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "rime/physics/aabb.hpp"

// A dynamic AABB tree (bounding-volume hierarchy) — the broadphase acceleration structure, and from
// M7.7/M7.9 also the ray/sweep and triangle-midphase engine (one structure, several customers).
//
// The idea: wrap every body in a *fattened* AABB (a small margin around its exact bounds) stored in
// a leaf; internal nodes bound their two children. To find what might touch a box you descend only
// the branches whose bounds overlap it — turning the O(n²) "test every pair" into roughly O(n log
// n). The fat margin means a body that moves a little stays inside its leaf, so most frames
// re-insert nothing. Insertion places a new leaf next to the sibling that grows total surface area
// least (the Surface-Area Heuristic: query cost scales with surface area, so minimizing it keeps
// the tree fast).
//
// Nodes are indices into a pooled array (stable as the pool grows) with a free list for O(1) reuse
// — the same data-oriented handle discipline as the body pool. This header lives under src/
// (PRIVATE): it is an implementation detail, invisible above the PhysicsWorld seam.
namespace rime::physics {

class AabbTree {
public:
    static constexpr std::int32_t kNull = -1;
    // Fat-AABB margin (metres). Big enough that small per-step motion doesn't re-insert; small
    // enough that fat boxes don't over-report. Tuned properly against the debris regime at M7.10.
    static constexpr float kMargin = 0.1f;

    // Insert a leaf bounding `tight`, tagged with `user` (the body's stable slot id). Returns the
    // proxy (node index), stable until destroy_proxy.
    [[nodiscard]] std::int32_t create_proxy(const Aabb& tight, std::uint32_t user) {
        const std::int32_t leaf = allocate_node();
        nodes_[leaf].aabb = expanded(tight, kMargin);
        nodes_[leaf].user = user;
        nodes_[leaf].height = 0;
        nodes_[leaf].child1 = kNull;
        nodes_[leaf].child2 = kNull;
        insert_leaf(leaf);
        return leaf;
    }

    void destroy_proxy(std::int32_t proxy) {
        remove_leaf(proxy);
        free_node(proxy);
    }

    // Re-bound a proxy to `tight`. If the current fat box still contains it, nothing happens (the
    // common case) and false is returned; otherwise the leaf is re-inserted with a fresh fat box
    // and true is returned.
    bool move_proxy(std::int32_t proxy, const Aabb& tight) {
        if (contains(nodes_[proxy].aabb, tight)) {
            return false;
        }
        remove_leaf(proxy);
        nodes_[proxy].aabb = expanded(tight, kMargin);
        insert_leaf(proxy);
        return true;
    }

    [[nodiscard]] const Aabb& proxy_aabb(std::int32_t proxy) const noexcept {
        return nodes_[proxy].aabb;
    }

    [[nodiscard]] bool empty() const noexcept { return root_ == kNull; }

    // Invoke fn(user) for every leaf whose fat AABB overlaps `aabb`. Read-only, so it is safe to
    // call from parallel query jobs once M7.5 wires them. Recursion depth is the tree height.
    template <class Fn> void query(const Aabb& aabb, Fn&& fn) const { query_node(root_, aabb, fn); }

    // Invoke fn(user) for every leaf whose fat AABB the ray (origin `o`, unit direction `dir`)
    // reaches within [0, tmax] — the broadphase half of a raycast (M7.7). Descends only the
    // branches the ray crosses, so it visits O(log n) nodes, not all of them; the caller's exact
    // ray-vs-shape test then confirms each reported leaf and tracks the nearest. `tmax` is the
    // descent bound (typically Ray::max_distance); the reciprocal direction is computed once here
    // and reused down the whole descent (see ray_hits_aabb).
    template <class Fn> void query_ray(core::Vec3 o, core::Vec3 dir, float tmax, Fn&& fn) const {
        const core::Vec3 inv_d{1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z};
        query_ray_node(root_, o, inv_d, tmax, fn);
    }

    // Test hook: every internal node bounds its children and the parent links are consistent.
    [[nodiscard]] bool validate() const { return root_ == kNull || validate_node(root_); }

private:
    struct Node {
        Aabb aabb;
        std::uint32_t user = 0;
        std::int32_t parent = kNull;
        std::int32_t child1 = kNull;
        std::int32_t child2 = kNull;
        std::int32_t height = 0;
        std::int32_t next = kNull; // free-list link when this node is free

        [[nodiscard]] bool is_leaf() const noexcept { return child1 == kNull; }
    };

    std::vector<Node> nodes_;
    std::int32_t root_ = kNull;
    std::int32_t free_ = kNull;

    std::int32_t allocate_node() {
        if (free_ == kNull) {
            nodes_.push_back(Node{});
            return static_cast<std::int32_t>(nodes_.size() - 1);
        }
        const std::int32_t idx = free_;
        free_ = nodes_[idx].next;
        nodes_[idx] = Node{};
        return idx;
    }

    void free_node(std::int32_t n) {
        nodes_[n].height = -1;
        nodes_[n].next = free_;
        free_ = n;
    }

    // Recompute AABB + height from `index` up to the root (after a child changed).
    //
    // Box2D additionally applies an AVL-style *tree rotation* at each step of this walk, swapping
    // subtrees to bound the height under adversarial insertion orders. We deliberately defer that
    // to M7.10 (perf tuning, measured against the debris stress harness): rotations change only the
    // tree's *shape* — every leaf remains bounded by all of its ancestors either way — so queries
    // stay exact without them; only the worst-case descent depth suffers. SAH insertion already
    // produces reasonable trees for game scenes, and we do not optimize unmeasured.
    void refit(std::int32_t index) {
        while (index != kNull) {
            const std::int32_t c1 = nodes_[index].child1;
            const std::int32_t c2 = nodes_[index].child2;
            nodes_[index].height = 1 + std::max(nodes_[c1].height, nodes_[c2].height);
            nodes_[index].aabb = merge(nodes_[c1].aabb, nodes_[c2].aabb);
            index = nodes_[index].parent;
        }
    }

    void insert_leaf(std::int32_t leaf) {
        if (root_ == kNull) {
            root_ = leaf;
            nodes_[leaf].parent = kNull;
            return;
        }
        const Aabb leaf_aabb = nodes_[leaf].aabb;

        // Descend from the root, each step taking the child that makes the cheapest home for the
        // leaf (the SAH cost = area of the box that would result). Stop when creating a new parent
        // right here is cheaper than descending into either child.
        std::int32_t index = root_;
        while (!nodes_[index].is_leaf()) {
            const std::int32_t c1 = nodes_[index].child1;
            const std::int32_t c2 = nodes_[index].child2;
            const float area = surface_area(nodes_[index].aabb);
            const float combined_area = surface_area(merge(nodes_[index].aabb, leaf_aabb));
            const float cost = 2.0f * combined_area; // make a new parent at `index`
            const float inheritance =
                2.0f * (combined_area - area); // extra area pushed to descendants
            const auto descend_cost = [&](std::int32_t child) {
                const float direct = surface_area(merge(leaf_aabb, nodes_[child].aabb));
                return nodes_[child].is_leaf()
                           ? direct + inheritance
                           : (direct - surface_area(nodes_[child].aabb)) + inheritance;
            };
            const float cost1 = descend_cost(c1);
            const float cost2 = descend_cost(c2);
            if (cost < cost1 && cost < cost2) {
                break;
            }
            index = cost1 < cost2 ? c1 : c2;
        }
        const std::int32_t sibling = index;

        // Splice a new internal parent above the sibling, holding {sibling, leaf}.
        const std::int32_t old_parent = nodes_[sibling].parent;
        const std::int32_t new_parent = allocate_node(); // may reallocate — index everything below
        nodes_[new_parent].parent = old_parent;
        nodes_[new_parent].aabb = merge(leaf_aabb, nodes_[sibling].aabb);
        nodes_[new_parent].height = nodes_[sibling].height + 1;
        nodes_[new_parent].child1 = sibling;
        nodes_[new_parent].child2 = leaf;
        nodes_[sibling].parent = new_parent;
        nodes_[leaf].parent = new_parent;
        if (old_parent != kNull) {
            if (nodes_[old_parent].child1 == sibling) {
                nodes_[old_parent].child1 = new_parent;
            } else {
                nodes_[old_parent].child2 = new_parent;
            }
        } else {
            root_ = new_parent;
        }
        refit(nodes_[leaf].parent);
    }

    void remove_leaf(std::int32_t leaf) {
        if (leaf == root_) {
            root_ = kNull;
            return;
        }
        const std::int32_t parent = nodes_[leaf].parent;
        const std::int32_t grand = nodes_[parent].parent;
        const std::int32_t sibling =
            nodes_[parent].child1 == leaf ? nodes_[parent].child2 : nodes_[parent].child1;
        if (grand != kNull) {
            // Replace `parent` with `sibling` and drop the now-childless parent node.
            if (nodes_[grand].child1 == parent) {
                nodes_[grand].child1 = sibling;
            } else {
                nodes_[grand].child2 = sibling;
            }
            nodes_[sibling].parent = grand;
            free_node(parent);
            refit(grand);
        } else {
            root_ = sibling;
            nodes_[sibling].parent = kNull;
            free_node(parent);
        }
    }

    template <class Fn> void query_node(std::int32_t n, const Aabb& aabb, Fn& fn) const {
        if (n == kNull) {
            return;
        }
        const Node& node = nodes_[n];
        if (!overlaps(node.aabb, aabb)) {
            return;
        }
        if (node.is_leaf()) {
            fn(node.user);
        } else {
            query_node(node.child1, aabb, fn);
            query_node(node.child2, aabb, fn);
        }
    }

    template <class Fn>
    void query_ray_node(std::int32_t n, core::Vec3 o, core::Vec3 inv_d, float tmax, Fn& fn) const {
        if (n == kNull) {
            return;
        }
        const Node& node = nodes_[n];
        if (!ray_hits_aabb(node.aabb, o, inv_d, tmax)) {
            return;
        }
        if (node.is_leaf()) {
            fn(node.user);
        } else {
            query_ray_node(node.child1, o, inv_d, tmax, fn);
            query_ray_node(node.child2, o, inv_d, tmax, fn);
        }
    }

    [[nodiscard]] bool validate_node(std::int32_t n) const {
        const Node& node = nodes_[n];
        if (node.is_leaf()) {
            return node.child2 == kNull;
        }
        const std::int32_t c1 = node.child1;
        const std::int32_t c2 = node.child2;
        if (nodes_[c1].parent != n || nodes_[c2].parent != n) {
            return false;
        }
        if (!contains(node.aabb, nodes_[c1].aabb) || !contains(node.aabb, nodes_[c2].aabb)) {
            return false;
        }
        return validate_node(c1) && validate_node(c2);
    }
};

} // namespace rime::physics
