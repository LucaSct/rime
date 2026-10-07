// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/physics/world.hpp"
#include "rime/terrain_collision/terrain_collision.hpp"

// Shared fixture for the m19.8c proofs: a tile source whose load completions the TEST delivers, in
// whatever order it likes, and a headless server-shaped simulation that drives the barrier.
//
// There is no thread and no sleep anywhere in here. "Asynchronous" means only this: a request does
// not become Ready until some later `pump()`, and the order in which the loads due at one pump
// complete is a policy the test chooses. That is the whole of what real IO does to this module —
// and it makes "the same session under a different completion order" an exact, repeatable input
// rather than something a scheduler might or might not produce.
namespace rime_test {

using namespace rime;
namespace tc = rime::terrain_collision;

inline constexpr float kDt = 1.0f / 60.0f;
inline constexpr float kTile = 16.0f;         // metres per tile
inline constexpr std::uint32_t kCells = 16;   // cells per tile side: 17 x 17 samples, 1 m apart
inline constexpr float kHeightScale = 0.001f; // metres per quantisation step
inline constexpr std::uint64_t kTileBytes =
    std::uint64_t{kCells + 1} * (kCells + 1) * sizeof(std::uint16_t);

// How the loads that fall due at the same pump are ordered.
enum class Order : std::uint8_t { Fifo, Lifo, Shuffled };

// splitmix64 — a tiny, well-mixed generator. Used instead of <random> so a seed names the same
// permutation on every standard library.
[[nodiscard]] inline std::uint64_t splitmix(std::uint64_t& s) noexcept {
    s += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

class FakeTileSource final : public tc::TileSource {
public:
    struct Options {
        // The manifest: every coordinate in this inclusive rectangle has a tile.
        std::int32_t min_x = -8, max_x = 8, min_z = -8, max_z = 8;
        std::uint32_t revision = 1;
        std::uint32_t latency = 2; // pumps between a request and its completion (>= 1)
        std::uint32_t jitter = 0;  // plus a seeded 0..jitter extra pumps, per request
        Order order = Order::Fifo;
        std::uint64_t seed = 1;
        // Terrain height in metres at a WORLD position. A function of world coordinates, so two
        // tiles sharing an edge quantise the very same numbers along it and the seam is watertight
        // by construction.
        std::function<double(double, double)> height = [](double, double) { return 0.0; };
    };

    explicit FakeTileSource(Options options) : options_(std::move(options)), rng_(options_.seed) {}

    // ── Per-tile overrides, for the refusal proofs ────────────────────────────────────────────
    void set_latency(std::int32_t x, std::int32_t z, std::uint32_t pumps) {
        latency_[{x, z}] = pumps;
    }

    void set_payload_revision(std::int32_t x, std::int32_t z, std::uint32_t revision) {
        payload_revision_[{x, z}] = revision;
    }

    void set_fails(std::int32_t x, std::int32_t z) { fails_[{x, z}] = true; }

    void set_misplaced(std::int32_t x, std::int32_t z) { misplaced_[{x, z}] = true; }

    // What an over-release from somewhere else does to a holder: the slot is evicted under it, so
    // its handle goes Stale. Returns false if no live slot holds `key`.
    bool invalidate(const tc::TileKey& key) {
        for (Slot& slot : slots_) {
            if (slot.occupied && slot.key == key) {
                evict(slot);
                return true;
            }
        }
        return false;
    }

    // Called from inside pump() as each load completes. A correct system has no use for this: it
    // exists so the falsification can install from a completion callback and show the proof turn
    // red.
    std::function<void(const tc::TileKey&)> on_complete;

    // Deliver every load that has fallen due, in the configured order.
    void pump() {
        ++pump_count_;
        std::vector<std::uint32_t> due;
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].occupied && slots_[i].state == assets::AssetState::Loading &&
                slots_[i].due_pump <= pump_count_) {
                due.push_back(i);
            }
        }
        // `due` is in slot order here; requests are made in key order, so that is FIFO.
        if (options_.order == Order::Lifo) {
            std::reverse(due.begin(), due.end());
        } else if (options_.order == Order::Shuffled) {
            for (std::size_t i = due.size(); i > 1; --i) {
                std::swap(due[i - 1], due[splitmix(rng_) % i]);
            }
        }
        for (const std::uint32_t index : due) {
            Slot& slot = slots_[index];
            const std::pair<std::int32_t, std::int32_t> at{slot.key.x, slot.key.z};
            if (fails_.count(at) != 0) {
                slot.state = assets::AssetState::Failed;
            } else {
                slot.asset = build(slot.key, misplaced_.count(at) != 0);
                slot.state = assets::AssetState::Ready;
            }
            completions.emplace_back(pump_count_, slot.key);
            if (on_complete) {
                on_complete(slot.key);
            }
        }
    }

    // ── TileSource ────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] bool
    current_revision(std::int32_t x, std::int32_t z, std::uint32_t& revision) const override {
        if (x < options_.min_x || x > options_.max_x || z < options_.min_z || z > options_.max_z) {
            return false;
        }
        revision = options_.revision;
        return true;
    }

    [[nodiscard]] assets::HeightfieldAssetHandle request(const tc::TileKey& key) override {
        ++requests;
        std::uint32_t index = 0;
        while (index < slots_.size() && slots_[index].occupied) {
            ++index;
        }
        if (index == slots_.size()) {
            slots_.emplace_back();
        }
        Slot& slot = slots_[index];
        slot.occupied = true;
        slot.key = key;
        slot.state = assets::AssetState::Loading;
        const std::pair<std::int32_t, std::int32_t> at{key.x, key.z};
        std::uint32_t latency = options_.latency;
        if (const auto it = latency_.find(at); it != latency_.end()) {
            latency = it->second;
        }
        if (options_.jitter > 0) {
            latency += static_cast<std::uint32_t>(splitmix(rng_) % (options_.jitter + 1));
        }
        slot.due_pump = pump_count_ + std::max<std::uint32_t>(latency, 1);
        ++live_;
        peak_live = std::max(peak_live, live_);
        assets::HeightfieldAssetHandle handle;
        handle.index = index;
        handle.generation = slot.generation;
        return handle;
    }

    [[nodiscard]] assets::AssetState state(assets::HeightfieldAssetHandle handle) const override {
        const Slot* slot = live(handle);
        return slot != nullptr ? slot->state : assets::AssetState::Stale;
    }

    [[nodiscard]] tc::TilePayload resolve(assets::HeightfieldAssetHandle handle) const override {
        const Slot* slot = live(handle);
        if (slot == nullptr || slot->state != assets::AssetState::Ready) {
            return {};
        }
        std::uint32_t revision = slot->key.revision;
        if (const auto it = payload_revision_.find({slot->key.x, slot->key.z});
            it != payload_revision_.end()) {
            revision = it->second;
        }
        return {&slot->asset, revision};
    }

    bool release(assets::HeightfieldAssetHandle handle) override {
        if (live(handle) == nullptr) {
            ++stale_releases;
            return false;
        }
        ++releases;
        evict(slots_[handle.index]);
        return true;
    }

    [[nodiscard]] std::size_t live_slots() const noexcept { return live_; }

    [[nodiscard]] std::uint64_t pump_count() const noexcept { return pump_count_; }

    std::uint64_t requests = 0;
    std::uint64_t releases = 0;
    std::uint64_t stale_releases = 0;
    std::size_t peak_live = 0;
    // (pump, key) in the order loads completed — the thing the permutations actually permute, and
    // what a test compares to show its runs really were different.
    std::vector<std::pair<std::uint64_t, tc::TileKey>> completions;

private:
    struct Slot {
        bool occupied = false;
        std::uint32_t generation = 0;
        tc::TileKey key{};
        assets::AssetState state = assets::AssetState::Loading;
        std::uint64_t due_pump = 0;
        assets::HeightfieldAsset asset{};
    };

    [[nodiscard]] const Slot* live(assets::HeightfieldAssetHandle handle) const noexcept {
        if (!handle.is_valid() || handle.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index];
        return (slot.occupied && slot.generation == handle.generation) ? &slot : nullptr;
    }

    void evict(Slot& slot) {
        slot.occupied = false;
        ++slot.generation;
        slot.asset = assets::HeightfieldAsset{};
        --live_;
    }

    [[nodiscard]] assets::HeightfieldAsset build(const tc::TileKey& key, bool misplaced) const {
        assets::HeightfieldAsset a;
        a.columns = kCells + 1;
        a.rows = kCells + 1;
        a.cell_size_x = kTile / static_cast<float>(kCells);
        a.cell_size_z = kTile / static_cast<float>(kCells);
        a.origin = {static_cast<float>(key.x) * kTile + (misplaced ? 3.0f : 0.0f),
                    0.0f,
                    static_cast<float>(key.z) * kTile};
        a.height_scale = kHeightScale;
        a.height_offset = 0.0f;
        a.samples.resize(std::size_t{a.columns} * a.rows);
        std::uint16_t lo = 65535, hi = 0;
        for (std::uint32_t j = 0; j < a.rows; ++j) {
            for (std::uint32_t i = 0; i < a.columns; ++i) {
                const double wx = double(key.x) * kTile + double(i) * a.cell_size_x;
                const double wz = double(key.z) * kTile + double(j) * a.cell_size_z;
                const double q = std::round(options_.height(wx, wz) / double(kHeightScale));
                const auto s = static_cast<std::uint16_t>(std::clamp(q, 0.0, 65535.0));
                a.samples[i + std::size_t{a.columns} * j] = s;
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
        }
        a.min_sample = lo;
        a.max_sample = hi;
        return a;
    }

    Options options_;
    std::uint64_t rng_;
    std::vector<Slot> slots_;
    std::size_t live_ = 0;
    std::uint64_t pump_count_ = 0;
    std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t> latency_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t> payload_revision_;
    std::map<std::pair<std::int32_t, std::int32_t>, bool> fails_;
    std::map<std::pair<std::int32_t, std::int32_t>, bool> misplaced_;
};

[[nodiscard]] inline physics::Aabb box_around(core::Vec3 centre, core::Vec3 half) noexcept {
    physics::Aabb b;
    b.min = centre - half;
    b.max = centre + half;
    return b;
}

// A headless, server-shaped simulation: a physics world, the terrain module, and a handful of
// simple bodies. It exists to drive the barrier in the documented order and nothing else.
//
// Every body enters through `request_admission` — including the ones a test spawns at tick 0. That
// is the protocol, and it is why `stalls_admitted_body` reads zero in every test that is not
// deliberately provoking it.
struct Sim {
    // What the swept bounds do not cover: a tick of gravity, solver correction, a little slack.
    static constexpr float kMargin = 0.25f;

    struct Actor {
        std::uint64_t entity = 0;
        physics::BodyId body{};
        core::Vec3 half{0.5f, 0.5f, 0.5f};
        bool driven = false;
        core::Vec3 drive{0.0f, 0.0f, 0.0f}; // horizontal velocity re-applied each tick (an "input")
    };

    struct Spawn {
        std::uint64_t entity = 0;
        physics::BodyDesc desc{};
        core::Vec3 half{0.5f, 0.5f, 0.5f};
        bool driven = false;
    };

    Sim(FakeTileSource::Options source_options, const tc::Config& config)
        : source(std::move(source_options)), terrain(config, source, world) {}

    [[nodiscard]] static tc::Config default_config() {
        tc::Config c;
        c.tile_size_x = kTile;
        c.tile_size_z = kTile;
        return c;
    }

    // Ask for a body to be put into the world. Created at once if its ground is installed,
    // otherwise held out and created by the barrier that admits it.
    tc::AdmissionResult spawn(const Spawn& s) {
        const tc::AdmissionResult result =
            terrain.request_admission(s.entity, box_around(s.desc.position, s.half));
        if (result == tc::AdmissionResult::Admitted) {
            create(s);
        } else {
            held.push_back(s);
        }
        return result;
    }

    tc::AdmissionResult spawn_sphere(std::uint64_t entity,
                                     core::Vec3 position,
                                     core::Vec3 velocity = {0.0f, 0.0f, 0.0f},
                                     bool driven = false) {
        Spawn s;
        s.entity = entity;
        s.desc.shape.type = physics::ShapeType::Sphere;
        s.desc.shape.radius = 0.5f;
        s.desc.position = position;
        s.desc.linear_velocity = velocity;
        s.desc.friction = 0.4f;
        s.driven = driven;
        return spawn(s);
    }

    // A teleport: the body leaves the simulation NOW and re-enters wherever its ground allows.
    tc::AdmissionResult teleport(std::uint64_t entity, core::Vec3 position) {
        Spawn s;
        s.entity = entity;
        for (auto it = actors.begin(); it != actors.end(); ++it) {
            if (it->entity == entity) {
                s.half = it->half;
                s.driven = it->driven;
                world.destroy_body(it->body);
                actors.erase(it);
                break;
            }
        }
        s.desc.shape.type = physics::ShapeType::Sphere;
        s.desc.shape.radius = 0.5f;
        s.desc.position = position;
        s.desc.friction = 0.4f;
        return spawn(s);
    }

    [[nodiscard]] Actor* actor(std::uint64_t entity) {
        for (Actor& a : actors) {
            if (a.entity == entity) {
                return &a;
            }
        }
        return nullptr;
    }

    [[nodiscard]] physics::BodyState state_of(std::uint64_t entity) {
        physics::BodyState s;
        if (const Actor* a = actor(entity)) {
            (void)world.get_body_state(a->body, s);
        }
        return s;
    }

    void set_drive(std::uint64_t entity, core::Vec3 velocity) {
        if (Actor* a = actor(entity)) {
            a->driven = true;
            a->drive = velocity;
        }
    }

    // ONE TICK, in the barrier order terrain_collision.hpp documents. Returns the commit status;
    // on Failed the tick was not simulated.
    tc::CommitStatus step() {
        // Loads complete "in the background" — between barriers, never inside one.
        source.pump();

        // 1. Freeze inputs: the driven bodies' velocities are this fixture's player input.
        for (const Actor& a : actors) {
            if (!a.driven) {
                continue;
            }
            physics::BodyState s;
            if (world.get_body_state(a.body, s)) {
                s.linear_velocity.x = a.drive.x;
                s.linear_velocity.z = a.drive.z;
                (void)world.set_body_state(a.body, s);
            }
        }

        // 2. Demands: every body, asleep or not, swept over the tick. No camera anywhere.
        demands.clear();
        for (const Actor& a : actors) {
            physics::BodyState s;
            if (world.get_body_state(a.body, s)) {
                demands.push_back(
                    {tc::swept_bounds(
                         box_around(s.position, a.half), s.linear_velocity, kDt, kMargin),
                     tc::DemandKind::Body});
            }
        }
        for (const physics::Aabb& sweep : sweeps) {
            demands.push_back({sweep, tc::DemandKind::Sweep});
        }
        const tc::Plan plan = terrain.plan(tick, demands);

        // 3–8. Commit; while it says Waiting the tick does not move, only the loader does.
        tc::CommitStatus status = terrain.try_commit(plan);
        while (status == tc::CommitStatus::Waiting) {
            ++waiting_pumps;
            if (waiting_pumps > 100000) {
                return status; // a stall that never ends is a test failure, not a hang
            }
            source.pump();
            status = terrain.try_commit(plan);
        }
        if (status != tc::CommitStatus::Ready) {
            return status;
        }

        // The entities this barrier admitted enter the simulation now.
        for (const std::uint64_t entity : terrain.admitted()) {
            for (auto it = held.begin(); it != held.end(); ++it) {
                if (it->entity == entity) {
                    create(*it);
                    held.erase(it);
                    break;
                }
            }
            admission_ticks.emplace_back(entity, tick);
        }

        // 9. Simulate.
        world.step(kDt);
        hashes.push_back(world.world_hash());
        ++tick;
        return status;
    }

    bool run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            if (step() != tc::CommitStatus::Ready) {
                return false;
            }
        }
        return true;
    }

    FakeTileSource source;
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain;

    std::vector<Actor> actors;
    std::vector<Spawn> held; // deferred: not in the world
    std::vector<physics::Aabb> sweeps;
    std::vector<tc::Demand> demands;
    std::uint64_t tick = 0;
    std::uint64_t waiting_pumps = 0;
    std::vector<std::uint64_t> hashes; // world_hash after each simulated tick
    std::vector<std::pair<std::uint64_t, std::uint64_t>> admission_ticks; // (entity, tick)

private:
    void create(const Spawn& s) {
        Actor a;
        a.entity = s.entity;
        a.half = s.half;
        a.driven = s.driven;
        a.drive = {s.desc.linear_velocity.x, 0.0f, s.desc.linear_velocity.z};
        a.body = world.create_body(s.desc);
        actors.push_back(a);
    }
};

} // namespace rime_test
