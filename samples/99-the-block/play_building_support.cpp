// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_building_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

#include "rime/assets/cooked_reader.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/core/math.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/platform/filesystem.hpp"

namespace rime::block_play {

namespace {

bool finite3(const core::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Closed-interval overlap on all three axes, with `tol` of slack: "do these boxes touch".
bool boxes_touch(const core::Vec3& amin,
                 const core::Vec3& amax,
                 const core::Vec3& bmin,
                 const core::Vec3& bmax,
                 float tol) {
    return amin.x - tol <= bmax.x && bmin.x - tol <= amax.x && amin.y - tol <= bmax.y &&
           bmin.y - tol <= amax.y && amin.z - tol <= bmax.z && bmin.z - tol <= amax.z;
}

} // namespace

bool PlayBuildingSupport::register_geometry(std::uint64_t asset_id,
                                            const assets::DestructibleAsset& asset) {
    if (asset.parts.empty()) {
        return false;
    }
    Geometry g;
    g.parts.reserve(asset.parts.size());
    for (const assets::DestructiblePart& p : asset.parts) {
        if (!finite3(p.com) || !finite3(p.aabb_min) || !finite3(p.aabb_max)) {
            return false;
        }
        g.parts.push_back({p.com, p.aabb_min, p.aabb_max});
    }
    for (const assets::DestructibleBond& b : asset.bonds) {
        if (b.a >= g.parts.size() || b.b >= g.parts.size() || b.a == b.b) {
            return false;
        }
        g.bonds.emplace_back(b.a, b.b);
    }
    geometry_[asset_id] = std::move(g);
    return true;
}

bool PlayBuildingSupport::register_cooked_dir(const std::filesystem::path& cooked) {
    for (const blockkit::CookSpec& spec : blockkit::cook_specs()) {
        const auto bytes = platform::read_file(cooked / (std::string(spec.name) + ".rdest"));
        if (!bytes) {
            return false;
        }
        assets::AssetError err{};
        const auto asset = assets::read_destructible(*bytes, err);
        if (!asset || !register_geometry(spec.asset, *asset)) {
            return false;
        }
    }
    return true;
}

bool PlayBuildingSupport::build(ecs::World& world, destruction::DestructionWorld& destruction) {
    buildings_.clear();
    stats_ = Stats{};

    // Collect every building slab. Sorted by (building, instance) so graph indices -- and with
    // them BFS visit order and the order release ops are queued -- are deterministic no matter
    // how the ECS happens to iterate.
    struct Slab {
        std::uint32_t building;
        std::uint32_t instance;
        std::uint64_t asset;
        bool unit_scale;
    };

    std::vector<Slab> slabs;
    world
        .query<destruction::Destructible,
               destruction::DestructibleInstanceRef,
               blockkit::SlabRole>()
        .for_each([&](ecs::Entity e,
                      destruction::Destructible& d,
                      destruction::DestructibleInstanceRef& ref,
                      blockkit::SlabRole& role) {
            if (role.building == blockkit::kNoBuilding ||
                ref.instance == destruction::kUnboundInstance ||
                !blockkit::slab_kind::is_destructible(role.kind) ||
                role.kind == blockkit::slab_kind::kCrate) {
                return;
            }
            bool unit = true;
            if (const auto* wt = world.get<ecs::WorldTransform>(e)) {
                const core::Vec3& s = wt->value.scale;
                unit = std::fabs(s.x - 1.0f) < 1e-3f && std::fabs(s.y - 1.0f) < 1e-3f &&
                       std::fabs(s.z - 1.0f) < 1e-3f;
            }
            slabs.push_back({role.building, ref.instance, d.asset, unit});
        });
    std::sort(slabs.begin(), slabs.end(), [](const Slab& a, const Slab& b) {
        return a.building != b.building ? a.building < b.building : a.instance < b.instance;
    });

    bool all_ok = true;
    for (std::size_t i = 0; i < slabs.size();) {
        std::size_t j = i;
        while (j < slabs.size() && slabs[j].building == slabs[i].building) {
            ++j;
        }
        Building b;
        b.id = slabs[i].building;
        bool refuse = false;
        // First node index of each slab, for the cross-slab contact pass below.
        std::vector<std::size_t> slab_first;
        for (std::size_t s = i; s < j; ++s) {
            slab_first.push_back(b.nodes.size());
            const auto it = geometry_.find(slabs[s].asset);
            if (it == geometry_.end()) {
                ++stats_.missing_geometry;
                refuse = true;
                continue;
            }
            const Geometry& g = it->second;
            const destruction::InstanceId inst{slabs[s].instance, 0};
            if (!slabs[s].unit_scale || destruction.instance_part_count(inst) != g.parts.size()) {
                // Non-unit scale or a pattern/geometry mismatch: world bounds below would be
                // wrong, and a wrong bound could demolish a building. Refuse, do not guess.
                refuse = true;
                continue;
            }
            for (std::uint32_t p = 0; p < g.parts.size(); ++p) {
                // World AABB of the cooked box carried through the instance placement: rotate
                // the 8 corners about the part's COM, then translate to the part's world COM.
                // Same convention `apply_damage` measures against, so what we call "touching"
                // and what a damage op can reach agree.
                const core::Transform pl = destruction.part_placement(inst, p);
                const auto& gp = g.parts[p];
                core::Vec3 lo{1e30f, 1e30f, 1e30f};
                core::Vec3 hi{-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < 8; ++c) {
                    const core::Vec3 corner{(c & 1) ? gp.bmax.x : gp.bmin.x,
                                            (c & 2) ? gp.bmax.y : gp.bmin.y,
                                            (c & 4) ? gp.bmax.z : gp.bmin.z};
                    const core::Vec3 r = core::rotate(
                        pl.rotation,
                        core::Vec3{corner.x - gp.com.x, corner.y - gp.com.y, corner.z - gp.com.z});
                    const core::Vec3 w{
                        pl.translation.x + r.x, pl.translation.y + r.y, pl.translation.z + r.z};
                    lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                    hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                }
                Node n;
                n.instance = slabs[s].instance;
                n.part = p;
                n.bmin = lo;
                n.bmax = hi;
                n.root = lo.y <= config_.ground_y + config_.ground_tolerance;
                b.nodes.push_back(n);
            }
        }
        slab_first.push_back(b.nodes.size());

        b.adjacency.assign(b.nodes.size(), {});
        auto link = [&](std::size_t x, std::size_t y, std::uint32_t& counter) {
            b.adjacency[x].push_back(static_cast<std::uint32_t>(y));
            b.adjacency[y].push_back(static_cast<std::uint32_t>(x));
            ++counter;
        };
        // (a) cooked bonds inside each slab. Node index = slab's first node + part index.
        if (!refuse) {
            for (std::size_t s = i; s < j; ++s) {
                const Geometry& g = geometry_.at(slabs[s].asset);
                const std::size_t base = slab_first[s - i];
                for (const auto& bond : g.bonds) {
                    link(base + bond.first, base + bond.second, stats_.bond_edges);
                }
            }
            // (b) cross-slab AABB contacts. O(n^2) over a building's parts, once at build time
            // (a few thousand pairs); never per frame.
            for (std::size_t s = i; s < j; ++s) {
                for (std::size_t t = s + 1; t < j; ++t) {
                    for (std::size_t x = slab_first[s - i]; x < slab_first[s - i + 1]; ++x) {
                        for (std::size_t y = slab_first[t - i]; y < slab_first[t - i + 1]; ++y) {
                            if (boxes_touch(b.nodes[x].bmin,
                                            b.nodes[x].bmax,
                                            b.nodes[y].bmin,
                                            b.nodes[y].bmax,
                                            config_.contact_tolerance)) {
                                link(x, y, stats_.contact_edges);
                            }
                        }
                    }
                }
            }
        }

        b.alive.assign(b.nodes.size(), 1);
        b.issued.assign(b.nodes.size(), 0);

        // Initial safety check, WITHOUT applying any damage: every part must already have a path
        // to the ground. If not, our model of this building is wrong (or the scene is), and
        // demolishing it at startup would be a guess. Quarantine and report instead.
        std::vector<std::uint8_t> reached;
        const std::uint32_t got = refuse ? 0 : reach(b, b.alive, reached);
        if (refuse || got != b.nodes.size()) {
            b.quarantined = true;
            ++stats_.quarantined_buildings;
            stats_.initial_unreachable_parts += static_cast<std::uint32_t>(b.nodes.size()) - got;
            all_ok = false;
        } else {
            stats_.nodes += static_cast<std::uint32_t>(b.nodes.size());
            for (const Node& n : b.nodes) {
                stats_.roots += n.root ? 1u : 0u;
            }
        }
        ++stats_.buildings;
        buildings_.push_back(std::move(b));
        i = j;
    }
    return all_ok;
}

std::uint32_t PlayBuildingSupport::reach(const Building& b,
                                         const std::vector<std::uint8_t>& alive,
                                         std::vector<std::uint8_t>& reached) {
    reached.assign(b.nodes.size(), 0);
    std::vector<std::uint32_t> queue;
    queue.reserve(b.nodes.size());
    for (std::uint32_t i = 0; i < b.nodes.size(); ++i) {
        if (alive[i] != 0 && b.nodes[i].root) {
            reached[i] = 1;
            queue.push_back(i);
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        for (const std::uint32_t nb : b.adjacency[queue[head]]) {
            if (alive[nb] != 0 && reached[nb] == 0) {
                reached[nb] = 1;
                queue.push_back(nb);
            }
        }
    }
    return static_cast<std::uint32_t>(queue.size());
}

void PlayBuildingSupport::step(destruction::DestructionWorld& destruction) {
    std::vector<std::uint8_t> now;
    std::vector<std::uint8_t> reached;
    std::uint32_t budget = config_.max_releases_per_step;
    for (Building& b : buildings_) {
        if (b.quarantined) {
            continue;
        }
        // Linear alive-bit scan -- the cache that keeps the BFS off the per-frame path. A pure
        // read of DestructionWorld state, a few hundred calls per building.
        now.resize(b.nodes.size());
        bool changed = false;
        for (std::size_t i = 0; i < b.nodes.size(); ++i) {
            now[i] = destruction.part_alive(destruction::InstanceId{b.nodes[i].instance, 0},
                                            b.nodes[i].part)
                         ? 1
                         : 0;
            changed = changed || now[i] != b.alive[i];
        }
        if (!changed && !b.backlog) {
            continue;
        }
        if (changed) {
            ++stats_.membership_changes;
        }
        bool acted = b.issued.end() != std::find(b.issued.begin(), b.issued.end(), std::uint8_t{1});
        for (std::size_t i = 0; i < b.nodes.size(); ++i) {
            if (b.alive[i] != 0 && now[i] == 0) {
                if (b.issued[i] != 0) {
                    ++stats_.targets_released;
                } else if (acted) {
                    ++stats_.other_deaths_after_issue;
                }
            }
        }
        b.alive = now;
        ++stats_.solves;
        reach(b, b.alive, reached);
        b.backlog = false;
        for (std::size_t i = 0; i < b.nodes.size(); ++i) {
            if (b.alive[i] == 0 || reached[i] != 0 || b.issued[i] != 0) {
                continue;
            }
            if (budget == 0) {
                b.backlog = true; // revisit next step even if no alive bit changes meanwhile
                ++stats_.deferred_by_cap;
                continue;
            }
            --budget;
            // Lost its load path. Release through the ordinary op stream: radius 1 mm at the
            // part's world COM, amount above its remaining health, so the op kills it (a killed
            // part leaves as its own debris chunk, ADR-0029 section 2) and replicates.
            const destruction::InstanceId inst{b.nodes[i].instance, 0};
            const float amount = destruction.part_health(inst, b.nodes[i].part) + 1.0f;
            destruction.apply_damage(inst,
                                     destruction.part_placement(inst, b.nodes[i].part).translation,
                                     config_.release_radius,
                                     amount,
                                     core::Vec3{0.0f, 0.0f, 0.0f});
            b.issued[i] = 1;
            ++stats_.com_ops_issued;
        }
    }
}

} // namespace rime::block_play
