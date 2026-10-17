// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "rime/assets/destructible_asset.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"

namespace rime::block_play {

// STRUCTURAL SUPPORT FOR THE AUTHORED BLOCK (play/city path only).
//
// The problem: every building slab is its own STATIC destructible instance, and the engine's
// support solve runs per instance, seeded by that instance's COOKED anchors. A floor cook anchors
// every part, so a floor stays "supported" even after every wall under it is gone -- it hangs in
// the air. Nothing in the authored scene models "this slab rests on that slab".
//
// The technique: a CONNECTIVITY (reachability) support model, the same idea the engine already
// uses inside one instance, lifted to the whole building.
//   1. Build one graph per building. Nodes are parts (across every slab of the building). Edges
//      are (a) the cooked bonds inside a slab and (b) cross-slab world-AABB contacts within
//      `contact_tolerance`. AABBs are an over-approximation of touching, so a contact edge may
//      exist where real geometry only nearly touches; that errs toward STANDING, which is the
//      safe direction for a scene we do not want to demolish by mistake.
//   2. Roots are ONLY parts whose bounds touch the ground plane. The permanent cooked pattern
//      anchors are deliberately NOT roots: treating them as roots is the very bug.
//   3. When the set of alive parts changes, run a BFS from the alive roots. Any alive part the
//      BFS does not reach has lost its load path and is released.
//   4. Release goes through the NORMAL damage-op stream: one tiny-radius (0.001 m) damage op at
//      the part's world centre of mass with magnitude above its health. It is therefore recorded
//      in `committed_ops()` and replicated like any other hit, so clients replay it. We never use
//      `apply_detach_set` (a correction: no events, no committed ops) and invent no client state.
//
// WHAT THIS IS NOT: not cohesive whole-floor physics, not FEM, no load or moment model. It is a
// conservative yes/no "is there any chain of touching parts down to the ground". A cantilever
// that is connected but would snap in reality stays up.
//
// FAN-OUT: `apply_damage` tests each part's cooked AABB against the blast, not its convex hull,
// so a 1 mm blast at one part's COM can also land inside a neighbour's overlapping AABB. We
// count the ops we issue and the deaths we then observe separately (see Stats) rather than
// pretending one op releases exactly one part.
//
// Server-side only: call `step` on the authoritative peer, before its `destruction.update`.
class PlayBuildingSupport {
public:
    struct Config {
        float ground_y = 0.0f;           // flat authored street plane (verified per-build by Stats)
        float ground_tolerance = 0.02f;  // a part this close to the plane counts as touching it
        float contact_tolerance = 0.02f; // cross-slab AABB gap still treated as touching
        float release_radius = 0.001f;   // metres: the "normal COM damage op" radius
        // Release at most this many parts per step(), across all buildings. A collapse is thus a
        // cascade over several ticks rather than one burst: a single tick that kills a thousand
        // parts emits a thousand replicated ops at once, and the probe measured exactly that
        // shape (>~1000 ops in one tick) leaving a client mirror diverged from the server even
        // with this leaf switched off. Spreading the load is the cheap, deterministic fix.
        std::uint32_t max_releases_per_step = 96;
    };

    struct Stats {
        std::uint32_t buildings = 0;
        std::uint32_t quarantined_buildings = 0; // initial graph did not reach ground: never stepped
        std::uint32_t nodes = 0;                 // parts across supported (non-quarantined) buildings
        std::uint32_t bond_edges = 0;
        std::uint32_t contact_edges = 0;
        std::uint32_t roots = 0;
        std::uint32_t initial_unreachable_parts = 0; // summed over quarantined buildings
        std::uint32_t missing_geometry = 0;          // instances whose asset was never registered
        std::uint64_t solves = 0;                    // BFS runs (only on membership change)
        std::uint64_t membership_changes = 0;        // buildings whose alive bits changed
        std::uint64_t com_ops_issued = 0;            // apply_damage calls made by this leaf
        std::uint64_t targets_released = 0;          // issued-at parts later seen not alive
        std::uint64_t deferred_by_cap = 0;           // release decisions pushed to a later step
        std::uint64_t other_deaths_after_issue = 0;  // non-target deaths in a building we acted on
                                                     // (fan-out, OR a concurrent weapon hit)
    };

    // Two constructors rather than `Config = {}`: a defaulted argument of a nested type with
    // member initialisers is ill-formed inside the enclosing class (the type is not yet complete).
    PlayBuildingSupport() = default;
    explicit PlayBuildingSupport(const Config& config) : config_(config) {}

    // Copy the cooked COM/AABB/bond data for one asset. False for an empty or non-finite asset.
    [[nodiscard]] bool register_geometry(std::uint64_t asset_id, const assets::DestructibleAsset& asset);

    // Convenience: read every blockkit cook from `cooked` and register it. False on any failure.
    [[nodiscard]] bool register_cooked_dir(const std::filesystem::path& cooked);

    // Construct the graphs. Call AFTER server binding, with every part still alive. Returns false
    // if any building was quarantined or any geometry was missing (details in stats()); a false
    // return is a report, not an error -- the supported buildings still work.
    [[nodiscard]] bool build(ecs::World& world, destruction::DestructionWorld& destruction);

    // Detect alive-bit changes, re-solve the changed buildings, queue release ops. Call once per
    // tick BEFORE the authoritative `destruction.update`.
    void step(destruction::DestructionWorld& destruction);

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool active() const noexcept { return !buildings_.empty(); }

private:
    struct Geometry {
        struct Part {
            core::Vec3 com;
            core::Vec3 bmin;
            core::Vec3 bmax;
        };
        std::vector<Part> parts;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> bonds;
    };

    struct Node {
        std::uint32_t instance = 0;
        std::uint32_t part = 0;
        core::Vec3 bmin; // world AABB at build time (all parts alive, slabs static)
        core::Vec3 bmax;
        bool root = false;
    };

    struct Building {
        std::uint32_t id = 0;
        std::vector<Node> nodes;
        std::vector<std::vector<std::uint32_t>> adjacency;
        std::vector<std::uint8_t> alive;  // cached alive bits (the BFS trigger)
        std::vector<std::uint8_t> issued; // we already queued a release for this node
        bool quarantined = false;
        bool backlog = false; // unsupported parts remain that the per-step cap deferred
    };

    // BFS from alive roots; `reached[i]` set for every alive node with a path. Returns count.
    static std::uint32_t reach(const Building& b,
                               const std::vector<std::uint8_t>& alive,
                               std::vector<std::uint8_t>& reached);

    Config config_;
    std::unordered_map<std::uint64_t, Geometry> geometry_;
    std::vector<Building> buildings_;
    Stats stats_;
};

} // namespace rime::block_play
