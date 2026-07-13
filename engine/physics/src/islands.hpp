// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/physics/body.hpp"
#include "solver.hpp"

// ISLANDS (M7.5): partition the awake world into independent SIMULATION ISLANDS — the connected
// components of the "contact graph" whose nodes are dynamic bodies and whose edges are the contact
// constraints between two dynamic bodies. Two islands share no dynamic body, which is the property
// everything downstream leans on:
//
//   * Parallelism — each island's solve reads and writes only its own bodies (immovable bodies are
//     read-only in the solver, see solver.hpp), so islands run concurrently on the job system with
//     no locks and no data races, and the result is BIT-IDENTICAL to solving them one after another
//     (ADR-0026's determinism contract, for any thread count).
//   * Sleeping — a whole island deactivates or wakes as a unit; a body cannot sleep while a
//     neighbour it is stacked on is still moving, and "is anything in my island awake?" is exactly
//     the connected-component question.
//
// The crucial modelling choice: STATIC and KINEMATIC bodies are NOT nodes. A contact against the
// ground does not merge the two boxes resting on it into one island — otherwise the whole floor
// would collapse into a single island that never parallelizes and never sleeps piecewise. Only a
// contact whose BOTH ends are dynamic unions two components.
//
// Private header (src/), invisible above the PhysicsWorld seam.
namespace rime::physics {

// The island partition as a pair of CSR (compressed-sparse-row) arrays. Island k owns:
//   * dynamic bodies      bodies[body_offsets[k] .. body_offsets[k + 1])            (dense indices)
//   * contact constraints constraints[constraint_offsets[k] .. constraint_offsets[k + 1])
// where the constraint entries are INDICES into the caller's constraint list. Both partitions are
// canonical: islands are numbered by their lowest-indexed dynamic body, bodies within an island are
// dense-sorted, and constraints keep their original (canonical broadphase) order — so the whole
// structure is a pure function of the inputs, independent of thread count.
//
// The trailing buffers are reusable scratch: build_islands() refills them in place, so a warmed-up
// world does no per-step island allocation once the vectors reach their high-water capacity (the
// full allocator story lands at M7.6; this just avoids the obvious per-tick churn).
struct IslandSet {
    std::vector<std::uint32_t> bodies;
    std::vector<std::uint32_t> body_offsets; // size island_count + 1
    std::vector<std::uint32_t> constraints;
    std::vector<std::uint32_t> constraint_offsets; // size island_count + 1
    std::size_t island_count = 0;

    // --- reusable scratch (not part of the result) ---
    std::vector<std::uint32_t> parent;  // union-find forest over dense body indices
    std::vector<std::uint32_t> root_id; // union-find root -> island id
    std::vector<std::uint32_t>
        body_island;                   // dense body -> island id (kInvalidIsland if not a node)
    std::vector<std::uint32_t> cursor; // counting-sort write cursors

    void clear() noexcept {
        bodies.clear();
        body_offsets.clear();
        constraints.clear();
        constraint_offsets.clear();
        island_count = 0;
    }
};

// Sentinel for "this body is not an island node" (static/kinematic) or "not yet assigned".
inline constexpr std::uint32_t kInvalidIsland = 0xFFFFFFFFu;

// -------------------------------------------------------------------- sleeping thresholds -------
// A body is a SLEEP CANDIDATE while both its linear and angular speed stay below these bounds; once
// EVERY body in an island has been a candidate for kTimeToSleep seconds the island deactivates —
// its bodies freeze (velocities zeroed) and are skipped by integration and the solver until
// something wakes them. Speeds are compared squared in the hot loop. The values are the familiar
// small numbers (Box2D uses ~0.01 m/s linear); set a touch higher here so a settled stack — whose
// bodies still carry a few mm/s of solver residual within the NGS slop — sleeps within about half a
// second, while a genuine slow slide stays awake. Constants for v1 (ADR-0026 defers per-body /
// per-world tuning knobs); the numbers live here beside the partition they gate.
inline constexpr float kLinearSleepThreshold = 0.05f;  // m/s
inline constexpr float kAngularSleepThreshold = 0.05f; // rad/s
inline constexpr float kTimeToSleep = 0.5f;            // seconds

// Build the island partition of `body_count` bodies given their motion types and the tick's contact
// constraints (each carries the two bodies' dense indices, canonical order). Fills `out` in CSR.
//
// Determinism (ADR-0026): union by smaller index — so a set's root is always its lowest dense
// member — plus a first-seen scan in dense order to number islands, means the same inputs always
// produce the same numbering and the same within-island order. Nothing here reads a clock, a
// pointer identity, or an unordered container.
inline void build_islands(std::size_t body_count,
                          std::span<const std::uint8_t> motion,
                          std::span<const ContactConstraint> constraints,
                          IslandSet& out) {
    out.clear();

    const auto is_node = [&](std::uint32_t i) noexcept {
        return motion[i] == static_cast<std::uint8_t>(MotionType::Dynamic);
    };

    // --- Union-find over the dynamic bodies. Path halving flattens find() as it walks; union by
    // smaller index keeps each set rooted at its lowest dense member (the canonical
    // representative).
    out.parent.resize(body_count);
    for (std::uint32_t i = 0; i < body_count; ++i) {
        out.parent[i] = i;
    }
    std::uint32_t* parent = out.parent.data();
    const auto find = [parent](std::uint32_t x) noexcept {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]]; // halve the path toward the root
            x = parent[x];
        }
        return x;
    };
    const auto unite = [&](std::uint32_t a, std::uint32_t b) noexcept {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        (a < b ? parent[b] : parent[a]) = (a < b ? a : b); // smaller index becomes the root
    };

    // Only a dynamic–dynamic contact merges two islands (see the header note on static anchors).
    for (const ContactConstraint& c : constraints) {
        if (is_node(c.body_a) && is_node(c.body_b)) {
            unite(c.body_a, c.body_b);
        }
    }

    // --- Number the islands. Scan bodies in dense order; the first time a root is seen it claims
    // the next island id, so island 0 holds the lowest-indexed dynamic body and the numbering is a
    // pure function of the partition, not of iteration luck.
    out.root_id.assign(body_count, kInvalidIsland);
    out.body_island.assign(body_count, kInvalidIsland);
    std::uint32_t island_count = 0;
    for (std::uint32_t i = 0; i < body_count; ++i) {
        if (!is_node(i)) {
            continue;
        }
        const std::uint32_t r = find(i);
        if (out.root_id[r] == kInvalidIsland) {
            out.root_id[r] = island_count++;
        }
        out.body_island[i] = out.root_id[r];
    }
    out.island_count = island_count;

    // --- Counting-sort the dynamic bodies into their islands (CSR). Scanning dense order with an
    // advancing cursor keeps each island's members dense-sorted.
    out.body_offsets.assign(island_count + 1, 0);
    for (std::uint32_t i = 0; i < body_count; ++i) {
        if (is_node(i)) {
            ++out.body_offsets[out.body_island[i] + 1];
        }
    }
    for (std::uint32_t k = 0; k < island_count; ++k) {
        out.body_offsets[k + 1] += out.body_offsets[k];
    }
    out.bodies.resize(out.body_offsets[island_count]);
    out.cursor.assign(out.body_offsets.begin(), out.body_offsets.begin() + island_count);
    for (std::uint32_t i = 0; i < body_count; ++i) {
        if (is_node(i)) {
            out.bodies[out.cursor[out.body_island[i]]++] = i;
        }
    }

    // --- Counting-sort the constraints into their islands. A constraint belongs to the island of
    // its dynamic endpoint (both endpoints share one island; a constraint against a
    // static/kinematic body uses the dynamic one). build_contacts never emits a both-immovable
    // constraint, so at least one endpoint is always a node. Scanning in the constraints' canonical
    // order preserves within-island order.
    const auto island_of = [&](const ContactConstraint& c) noexcept {
        return is_node(c.body_a) ? out.body_island[c.body_a] : out.body_island[c.body_b];
    };
    out.constraint_offsets.assign(island_count + 1, 0);
    for (const ContactConstraint& c : constraints) {
        ++out.constraint_offsets[island_of(c) + 1];
    }
    for (std::uint32_t k = 0; k < island_count; ++k) {
        out.constraint_offsets[k + 1] += out.constraint_offsets[k];
    }
    out.constraints.resize(out.constraint_offsets[island_count]);
    out.cursor.assign(out.constraint_offsets.begin(),
                      out.constraint_offsets.begin() + island_count);
    for (std::uint32_t ci = 0; ci < constraints.size(); ++ci) {
        out.constraints[out.cursor[island_of(constraints[ci])]++] = ci;
    }
}

} // namespace rime::physics
