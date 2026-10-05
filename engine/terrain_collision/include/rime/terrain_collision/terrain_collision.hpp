// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/shape.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// Regional terrain collision (m19.8c, ADR-0068).
//
// ── THE PROBLEM ───────────────────────────────────────────────────────────────────────────────
//
// A Battlefield-scale map is thousands of heightfield tiles, and a server cannot keep every one of
// them in its physics world. So collision STREAMS: a tile is installed while something simulated is
// near it and removed when nothing is. That is easy to build and very easy to build wrong, because
// "is the ground there?" now has an answer that depends on a disk — and a simulation whose outcome
// depends on which read finished first is not a simulation two machines, or two runs of one
// machine, can agree on.
//
// ── THE RULE THAT MAKES IT DETERMINISTIC ──────────────────────────────────────────────────────
//
// *Load completions only populate storage.* A finished load never installs a tile, never selects
// one, never expires a pin and never advances time. Everything that changes the physics world
// happens in ONE place — the tick barrier, `try_commit` — which reads readiness there and only
// there, and applies every mutation in `TileKey` order. Two loads finishing in either order between
// two barriers therefore produce the same world: the barrier sees the same READY SET and walks it
// in the same order. (tests/terrain_collision proves it by permuting completions, and proves the
// test can fail by letting a completion install directly.)
//
// ── THE BARRIER, IN ORDER ─────────────────────────────────────────────────────────────────────
//
// This is the single documented place for the order. The caller does steps 1 and 9; `plan` is
// step 2; `try_commit` is steps 3–8.
//
//   1. FREEZE the tick's inputs (the application: drain commands, decide spawns and teleports,
//      call `request_admission` / `pin` / `unpin` for them).
//   2. COMPUTE the immutable demand and eviction lists — `plan(tick, demands)`. A pure function of
//      the demands, the pins and the manifest. It does not look at what has finished loading.
//   3. REQUEST the assets the plan wants and this module does not yet hold.
//   4. INSTALL every wanted tile that is READY: validate it, register the shape, create the static
//      body.
//   5. STALL CHECK (the safety net): if an already-admitted body's bounds need a tile that is still
//      not installed, stop here and return `Waiting`. Nothing below runs and the tick is not
//      simulated; the caller pumps its loader and calls `try_commit` again WITH THE SAME PLAN.
//   6. EVALUATE deferred admissions: an entity is admitted only if every tile under its bounds is
//      installed.
//   7. DEACTIVATE outgoing tiles: destroy the body, unregister the shape, release the asset handle
//      exactly once.
//   8. JOURNAL — each install and each deactivation appends `(tick, key, activate|deactivate)`.
//   9. RUN gameplay and physics (the application).
//
// ── DEFERRED ADMISSION, AND WHY IT IS NOT THE STALL ───────────────────────────────────────────
//
// A spawn or a teleport is the one moment a body can land on terrain nobody saw it approaching. The
// owner's decision (ADR-0068) is that this must not freeze the server: the ENTITY waits, the world
// does not. `request_admission` answers `Deferred`, the entity stays out of the simulation, its
// tiles are requested and pinned, and each barrier re-asks. Everyone else keeps ticking.
//
// The stall in step 5 is the other case and is deliberately not the normal path: a body that is
// ALREADY simulated cannot be held out without inventing what happens to everything touching it,
// so if the prefetch envelope was too small or the disk too slow, the whole tick waits rather than
// stepping that body over ground that is not there. It has its own counter, and that counter
// reading zero is the claim that the envelope is sized correctly.
//
// ── WHAT "DEMAND" MEANS ───────────────────────────────────────────────────────────────────────
//
// Every simulated body — players, vehicles, debris, SLEEPERS INCLUDED (a sleeping crate still rests
// on its tile) — contributes the box it can occupy during this tick: its bounds swept by its
// velocity over `dt`, plus a margin (`swept_bounds` below). Projectile and hitscan sweeps
// contribute theirs. The camera never does: what one client is looking at must not be able to
// change what the simulation collides with.
//
// From those boxes: tiles UNDER a demand are required this tick; tiles within
// `activate_margin_tiles` (one tile width) are requested and installed as they become ready — the
// prefetch envelope; tiles stay installed until they are further than `retain_margin_tiles` (two
// widths) from every demand, or while pinned. The gap between the two is the hysteresis that stops
// a body pacing across a boundary from loading and unloading the same tile every step.
//
// ── THREADING ─────────────────────────────────────────────────────────────────────────────────
//
// One thread, at the barrier — the thread that owns the PhysicsWorld, between steps (every physics
// call made here is "not safe concurrently with step()"). No globals, no hidden ordering: one
// instance per PhysicsWorld, so a server and a client in one process each own theirs.
//
// ── NOT HERE ──────────────────────────────────────────────────────────────────────────────────
//
// Immutable terrain only: no deformation, no GPU, no network transport, no island freezing, no
// LOD, no byte budget beyond the explicit cap on the required set. See ADR-0068 for the bricks that
// add them.
namespace rime::terrain_collision {

// One terrain tile: its integer grid coordinate and the content revision. Ordered x, then z, then
// revision — the order every mutation list in this module is sorted by, which is what makes body
// and shape ids a function of the tick history rather than of load timing.
struct TileKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::uint32_t revision = 0;

    friend constexpr auto operator<=>(const TileKey&, const TileKey&) noexcept = default;
};

// What a TileSource hands back for a Ready handle: the samples, and the revision those samples
// actually are. The revision is checked against the key at install — a payload that is not the
// revision the plan asked for is REFUSED, not installed under the wrong name.
struct TilePayload {
    const assets::HeightfieldAsset* asset = nullptr;
    std::uint32_t revision = 0;
};

// Where tiles come from. The seam exists for two reasons: the manifest (which coordinate has
// terrain, at which revision) is the application's, and the proof needs a source whose completions
// it can deliver in any order it likes. `AssetServerTileSource` (asset_server_source.hpp) is the
// production implementation over the m19.8b streamed handles.
//
// The contract mirrors AssetServer's streamed path (ADR-0067): every `request` takes one ownership
// and is balanced by exactly one `release`; `state` moves Loading → Ready | Failed, and reports
// Stale for a handle whose ownership is gone. Implementations must change what `state` returns only
// in response to their own pump — never call back into the TerrainCollision.
class TileSource {
public:
    virtual ~TileSource() = default;

    // The manifest. False means "no terrain at this coordinate" (the edge of the world, a hole in
    // the map): such a coordinate is never required, requested or waited for.
    [[nodiscard]] virtual bool
    current_revision(std::int32_t x, std::int32_t z, std::uint32_t& revision) const = 0;

    [[nodiscard]] virtual assets::HeightfieldAssetHandle request(const TileKey& key) = 0;
    [[nodiscard]] virtual assets::AssetState state(assets::HeightfieldAssetHandle handle) const = 0;
    [[nodiscard]] virtual TilePayload resolve(assets::HeightfieldAssetHandle handle) const = 0;
    virtual bool release(assets::HeightfieldAssetHandle handle) = 0;
};

// Why a demand exists. Both kinds are REQUIRED (the tick will not simulate without their tiles);
// they are separate so the stall counters say which one the envelope failed.
enum class DemandKind : std::uint8_t {
    Body,  // an admitted, simulated body (player, vehicle, debris — awake or asleep)
    Sweep, // a projectile or hitscan query that will be evaluated this tick
};

struct Demand {
    physics::Aabb bounds{};
    DemandKind kind = DemandKind::Body;
};

// The box a body can occupy during one tick: its bounds, extended along where its velocity carries
// it over `dt`, then grown by `margin` on every side. The margin is the caller's statement of what
// the velocity does NOT cover — acceleration within the tick, rotation of a long body, controller
// probes below the feet, solver correction. It is stated, not hidden, because an envelope nobody
// can see is one nobody can size.
[[nodiscard]] physics::Aabb
swept_bounds(const physics::Aabb& bounds, core::Vec3 velocity, float dt, float margin) noexcept;

// Why a tile is pinned. Counted per reason on each tile, so "why is this still resident" has an
// answer finer than a single number.
enum class PinReason : std::uint8_t {
    Query,     // an explicit gameplay query that will run against this ground
    History,   // rollback/replay history that may be re-simulated over it
    Admission, // a deferred admission waiting for it (taken internally)
};
inline constexpr std::size_t kPinReasonCount = 3;

struct PinToken {
    std::uint64_t id = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return id != 0; }
};

enum class TileState : std::uint8_t {
    Absent,    // no record
    Pending,   // wanted (pinned), not yet requested — requests only happen at the barrier
    Requested, // load requested, not installed
    Installed, // shape registered, static body live
    Failed,    // the load failed or its content was refused; not retried while still wanted
};

enum class CommitStatus : std::uint8_t {
    Ready,   // the barrier is complete; simulate the tick
    Waiting, // an admitted body needs a tile that is not installed; do NOT simulate, retry
    Failed,  // explicit refusal: budget overflow, a required tile that cannot be installed, or a
             // plan that is not the latest one. Nothing below the failing step was applied.
};

enum class AdmissionResult : std::uint8_t {
    Admitted, // every tile under the bounds is installed; the entity may enter the simulation
    Deferred, // held out; re-evaluated at each barrier, reported through `admitted()`
};

enum class JournalOp : std::uint8_t { Activate, Deactivate };

// One line of the activation journal. `key.revision` is the revision; it is not repeated.
struct JournalEntry {
    std::uint64_t tick = 0;
    TileKey key{};
    JournalOp op = JournalOp::Activate;

    friend constexpr bool operator==(const JournalEntry&, const JournalEntry&) noexcept = default;
};

// The immutable output of `plan`: what this tick needs, wants and lets go of. All lists are sorted
// by TileKey and free of duplicates. Treat it as opaque and hand it back to `try_commit`.
struct Plan {
    std::uint64_t tick = 0;
    std::uint64_t serial = 0;      // which `plan` call produced this; an older plan is refused
    std::vector<TileKey> required; // under a demand: must be installed to simulate
    std::vector<std::uint8_t> required_kinds; // parallel to `required`: bit 0 Body, bit 1 Sweep
    std::vector<TileKey> activate;      // required ∪ prefetch envelope ∪ pinned: request + install
    std::vector<TileKey> deactivate;    // held now, outside the retain envelope and unpinned
    std::size_t required_or_pinned = 0; // what the budget cap is compared against
    bool overflow = false;              // required_or_pinned exceeds Config::max_required_tiles
};

struct Config {
    // World position of tile (0, 0)'s local origin, and the tile pitch. Tile (x, z) covers
    // [origin.x + x*tile_size_x, origin.x + (x+1)*tile_size_x] in X and likewise in Z; it is a
    // column — height takes no part in which tile a box is over.
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    float tile_size_x = 64.0f;
    float tile_size_z = 64.0f;

    // The prefetch envelope and the retain envelope, in tile widths beyond a demand's bounds.
    // `retain` below `activate` would evict what was just requested, so it is raised to match.
    float activate_margin_tiles = 1.0f;
    float retain_margin_tiles = 2.0f;

    // A prefetched tile that is ready is installed at the first barrier at or after
    // `requested tick + activation_lead_ticks`. With 0 it is installed at the first barrier that
    // finds it ready — so WHICH tick that is depends on how long the load took. A lead longer than
    // the load makes the install tick a function of the request tick alone, i.e. independent of
    // load timing and not only of completion order. A tile a body REQUIRES is never held for the
    // lead. The cost of a lead is envelope: the tile is usable that many ticks later.
    std::uint32_t activation_lead_ticks = 0;

    // The most tiles the simulation may REQUIRE at once (under a demand, or pinned). Exceeding it
    // fails the commit explicitly — it is never trimmed, because every way of trimming a required
    // set is a way of simulating over missing ground.
    std::size_t max_required_tiles = 256;

    // Passed to HeightfieldDesc::thickness and BodyDesc::friction for every tile body.
    float thickness = 1.0f;
    float friction = 0.5f;
};

// Guardrail 5 (CLAUDE.md): every skip, drop, defer and refusal has a counter, and each one is moved
// by a test in tests/terrain_collision. Monotonic unless marked as a gauge.
struct Counters {
    std::uint64_t requests = 0;           // TileSource::request calls
    std::uint64_t requests_cancelled = 0; // released before the load was ever installed
    std::uint64_t installs = 0;
    std::uint64_t evictions = 0; // installed tiles deactivated
    std::uint64_t load_failures = 0;
    // A ready prefetch tile left uninstalled this barrier because its lead has not elapsed.
    std::uint64_t installs_held_for_lead = 0;
    // A planned deactivation skipped because the tile was pinned after the plan was made.
    std::uint64_t evictions_skipped_pinned = 0;

    std::uint64_t admissions_immediate = 0;    // request_admission answered Admitted
    std::uint64_t admissions_deferred = 0;     // …answered Deferred
    std::uint64_t admitted_after_deferral = 0; // a deferred entity admitted at a barrier
    std::uint64_t admission_retries = 0;       // barriers a deferred entity was re-asked and held
    std::uint64_t admissions_cancelled = 0;

    // TICKS stalled, by the kind of demand whose tile was missing, and how many `Waiting` answers
    // those ticks cost in total. `stalls_admitted_body` is the safety net's counter: ~0 in a
    // correctly-enveloped session.
    std::uint64_t stalls_admitted_body = 0;
    std::uint64_t stalls_sweep = 0;
    std::uint64_t stall_retries = 0;

    std::uint64_t pins_taken = 0;
    std::uint64_t pins_released = 0;
    std::uint64_t pins_active = 0; // gauge; includes admission pins

    // Refusals, by reason.
    std::uint64_t refused_stale_handle = 0;      // the source no longer honours our handle
    std::uint64_t refused_revision_mismatch = 0; // a payload or a pin at the wrong revision
    std::uint64_t refused_invalid_content =
        0; // geometry that does not fit the grid, or physics said no
    std::uint64_t refused_budget_overflow = 0; // PLANS whose required set exceeded the cap
    std::uint64_t refused_unknown_pin = 0;     // unpin of a token that is not held
    std::uint64_t refused_stale_plan = 0;      // try_commit with a plan that is not the latest
    std::uint64_t refused_required_failed = 0; // a required tile is in TileState::Failed
    std::uint64_t coverage_refusals = 0;       // require_coverage answered false

    std::uint64_t tiles_installed = 0;     // gauge
    std::uint64_t bytes_resident = 0;      // gauge: sample bytes of installed tiles
    std::uint64_t peak_bytes_resident = 0; // high-water mark of the above
};

class TerrainCollision {
public:
    // Borrows the source and the world; both must outlive this object. The destructor removes
    // every tile body it created and releases every handle it holds.
    TerrainCollision(const Config& config, TileSource& source, physics::PhysicsWorld& world);
    ~TerrainCollision();

    TerrainCollision(const TerrainCollision&) = delete;
    TerrainCollision& operator=(const TerrainCollision&) = delete;

    // Barrier step 2. Pure with respect to load readiness. One plan per tick; a newer plan
    // supersedes an older one.
    [[nodiscard]] Plan plan(std::uint64_t tick, std::span<const Demand> demands);

    // Barrier steps 3–8. On `Waiting`, pump the loader and call again with the SAME plan; the tick
    // must not be simulated and its inputs must not be consumed until this returns `Ready`.
    [[nodiscard]] CommitStatus try_commit(const Plan& plan);

    // Entities admitted by the most recent `Ready` commit, in ascending entity order. The caller
    // puts them into the simulation now and includes them in the next tick's demands; their tiles
    // stay pinned until that next `plan`. Empty after a `Waiting` or `Failed` commit, and
    // emptied by the next `plan`.
    [[nodiscard]] std::span<const std::uint64_t> admitted() const noexcept { return admitted_; }

    // Pin every tile under `bounds` for a reason other than a simulated body. A pinned tile is
    // requested and installed at the barrier and is never evicted, but — unlike a demand — it does
    // not stall the tick: ask `covers` before relying on it. `revision` is the content revision
    // the caller's data was built against; if any tile under the bounds is at a different one the
    // pin is REFUSED (invalid token, counted), because replaying history over other ground than it
    // was recorded on is exactly the bug the revision exists to catch.
    [[nodiscard]] PinToken
    pin(const physics::Aabb& bounds, std::uint32_t revision, PinReason reason = PinReason::Query);
    bool unpin(PinToken token);

    // Ask to put `entity` (the caller's own id) into the simulation at `bounds`. Call during
    // barrier step 1. `Deferred` holds it out and pins its tiles; a repeat request for an entity
    // already deferred replaces its bounds.
    [[nodiscard]] AdmissionResult request_admission(std::uint64_t entity,
                                                    const physics::Aabb& bounds);
    bool cancel_admission(std::uint64_t entity);

    [[nodiscard]] std::size_t deferred_admissions() const noexcept { return deferred_.size(); }

    // Is every tile under `bounds` installed? (A coordinate with no terrain in the manifest counts
    // as covered: there is nothing to stand on there in any revision.) `require_coverage` is the
    // same question, counted when the answer is no — what a client asks before predicting.
    [[nodiscard]] bool covers(const physics::Aabb& bounds) const;
    [[nodiscard]] bool require_coverage(const physics::Aabb& bounds);

    [[nodiscard]] TileState tile_state(const TileKey& key) const noexcept;
    [[nodiscard]] std::uint32_t pin_count(const TileKey& key, PinReason reason) const noexcept;
    // The static body of an installed tile; the null id otherwise.
    [[nodiscard]] physics::BodyId tile_body(const TileKey& key) const noexcept;

    // The activation journal since the last clear, in the order the mutations were applied. It
    // grows without bound: whoever consumes it (a recorder today, the activation transport in the
    // next brick) drains it with `clear_journal`.
    [[nodiscard]] std::span<const JournalEntry> journal() const noexcept { return journal_; }

    void clear_journal() noexcept { journal_.clear(); }

    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    struct Record {
        TileKey key{};
        assets::HeightfieldAssetHandle handle{};
        physics::HeightfieldId shape{};
        physics::BodyId body{};
        std::uint32_t pins[kPinReasonCount] = {0, 0, 0};
        std::uint64_t requested_tick = 0;
        std::uint64_t bytes = 0;
        TileState state = TileState::Pending;

        [[nodiscard]] std::uint32_t pin_total() const noexcept {
            return pins[0] + pins[1] + pins[2];
        }
    };

    struct Pin {
        std::uint64_t id = 0;
        PinReason reason = PinReason::Query;
        std::vector<TileKey> keys; // sorted
    };

    struct Deferred {
        std::uint64_t entity = 0;
        physics::Aabb bounds{};
        PinToken pin{};
    };

    // Every manifest tile whose column `bounds`, grown by `margin_tiles` tile widths, touches.
    // Appended to `out` unsorted. Returns false if a tile's revision differs from `*must_match`.
    bool collect_keys(const physics::Aabb& bounds,
                      float margin_tiles,
                      std::vector<TileKey>& out,
                      const std::uint32_t* must_match = nullptr) const;

    [[nodiscard]] Record* find(const TileKey& key) noexcept;
    [[nodiscard]] const Record* find(const TileKey& key) const noexcept;
    Record& find_or_create(const TileKey& key);

    PinToken take_pin(std::vector<TileKey> keys, PinReason reason);
    bool drop_pin(std::uint64_t id);

    // Barrier step 4 for one record. `required` lifts the activation lead.
    void try_install(Record& record, std::uint64_t tick, bool required);
    // Barrier step 7 for one record (which is erased by the caller).
    void deactivate(Record& record, std::uint64_t tick);
    void fail(Record& record);

    Config config_{};
    TileSource& source_;
    physics::PhysicsWorld& world_;

    std::vector<Record> records_;    // sorted by key
    std::vector<Pin> pins_;          // sorted by id (ids only grow)
    std::vector<Deferred> deferred_; // sorted by entity
    std::vector<std::uint64_t> admitted_;
    // Admission pins of the entities in `admitted_`: dropped by the next `plan`, by which time the
    // admitted bodies are demands in their own right.
    std::vector<PinToken> admitted_pins_;
    std::vector<JournalEntry> journal_;

    std::uint64_t next_pin_id_ = 1;
    std::uint64_t plan_serial_ = 0;     // serial of the newest plan
    bool plan_open_ = false;            // …and whether it is still to be committed
    std::uint64_t stalled_serial_ = 0;  // the plan last counted as a stalled tick
    std::uint64_t overflow_serial_ = 0; // the plan last counted as a budget overflow

    Counters counters_{};
};

} // namespace rime::terrain_collision
