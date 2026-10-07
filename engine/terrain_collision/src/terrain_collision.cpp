// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/terrain_collision/terrain_collision.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "rime/core/diagnostics/log.hpp"
#include "rime/physics/world.hpp"

namespace rime::terrain_collision {

namespace {

// The widest run of tiles one box may span along an axis. A box wider than this is not a body, it
// is a bug (a NaN, an uninitialised bound, a velocity of 1e30) — and walking billions of
// coordinates to find that out would hang the barrier. It is reported as a budget overflow, which
// is what it is: more ground than the simulation is allowed to require.
constexpr std::int64_t kMaxTileSpan = 1024;

constexpr std::uint8_t kBodyBit = 1u << 0;
constexpr std::uint8_t kSweepBit = 1u << 1;

[[nodiscard]] std::uint8_t kind_bit(DemandKind kind) noexcept {
    return kind == DemandKind::Body ? kBodyBit : kSweepBit;
}

[[nodiscard]] bool finite(const physics::Aabb& b) noexcept {
    return std::isfinite(b.min.x) && std::isfinite(b.min.z) && std::isfinite(b.max.x) &&
           std::isfinite(b.max.z);
}

// The tile index a world coordinate falls in. Done in double so that a coordinate a few kilometres
// out still lands on the right side of a seam; `floor`, not truncation, so negative coordinates
// index downward instead of folding onto tile 0.
[[nodiscard]] std::int64_t tile_index(float value, float origin, float size) noexcept {
    return static_cast<std::int64_t>(std::floor(
        (static_cast<double>(value) - static_cast<double>(origin)) / static_cast<double>(size)));
}

void sort_unique(std::vector<TileKey>& keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

[[nodiscard]] bool contains(const std::vector<TileKey>& sorted, const TileKey& key) noexcept {
    return std::binary_search(sorted.begin(), sorted.end(), key);
}

// Does the cooked tile actually occupy the grid cell its key names? A tile that is the wrong size
// or sits somewhere else would install without complaint and leave a strip of the world with two
// grounds or none, so it is checked rather than assumed. The tolerance is relative to the tile
// pitch: cooked extents are a product of floats and need not be bit-equal to the configured pitch.
[[nodiscard]] bool fits_grid(const assets::HeightfieldAsset& asset,
                             const TileKey& key,
                             const Config& config) noexcept {
    if (asset.columns < 2 || asset.rows < 2) {
        return false;
    }
    if (asset.samples.size() != std::size_t{asset.columns} * std::size_t{asset.rows}) {
        return false;
    }
    const double extent_x = static_cast<double>(asset.columns - 1) * asset.cell_size_x;
    const double extent_z = static_cast<double>(asset.rows - 1) * asset.cell_size_z;
    const double want_x = static_cast<double>(config.origin.x) +
                          static_cast<double>(key.x) * static_cast<double>(config.tile_size_x);
    const double want_z = static_cast<double>(config.origin.z) +
                          static_cast<double>(key.z) * static_cast<double>(config.tile_size_z);
    const double tol_x = 1e-3 * static_cast<double>(config.tile_size_x);
    const double tol_z = 1e-3 * static_cast<double>(config.tile_size_z);
    return std::fabs(extent_x - static_cast<double>(config.tile_size_x)) <= tol_x &&
           std::fabs(extent_z - static_cast<double>(config.tile_size_z)) <= tol_z &&
           std::fabs(static_cast<double>(asset.origin.x) - want_x) <= tol_x &&
           std::fabs(static_cast<double>(asset.origin.z) - want_z) <= tol_z;
}

} // namespace

physics::Aabb
swept_bounds(const physics::Aabb& bounds, core::Vec3 velocity, float dt, float margin) noexcept {
    const core::Vec3 travel = velocity * dt;
    physics::Aabb out;
    out.min = {std::min(bounds.min.x, bounds.min.x + travel.x) - margin,
               std::min(bounds.min.y, bounds.min.y + travel.y) - margin,
               std::min(bounds.min.z, bounds.min.z + travel.z) - margin};
    out.max = {std::max(bounds.max.x, bounds.max.x + travel.x) + margin,
               std::max(bounds.max.y, bounds.max.y + travel.y) + margin,
               std::max(bounds.max.z, bounds.max.z + travel.z) + margin};
    return out;
}

TerrainCollision::TerrainCollision(const Config& config,
                                   TileSource& source,
                                   physics::PhysicsWorld& world)
    : config_(config), source_(source), world_(world) {
    // Retaining less than is activated would evict a tile at the barrier after the one that
    // requested it, forever. Raise rather than assert: the envelope is tuning data.
    config_.activate_margin_tiles = std::max(config_.activate_margin_tiles, 0.0f);
    config_.retain_margin_tiles =
        std::max(config_.retain_margin_tiles, config_.activate_margin_tiles);
}

TerrainCollision::~TerrainCollision() {
    // Same order as a barrier deactivation, in key order, so tearing a world down frees ids in a
    // reproducible sequence too.
    for (Record& record : records_) {
        if (record.state == TileState::Installed) {
            world_.destroy_body(record.body);
            (void)world_.unregister_heightfield(record.shape);
        }
        // Only a load still in flight holds a handle (an installed tile released at install).
        if (record.handle.is_valid()) {
            (void)source_.release(record.handle);
        }
    }
}

bool TerrainCollision::collect_keys(const physics::Aabb& bounds,
                                    float margin_tiles,
                                    std::vector<TileKey>& out,
                                    const std::uint32_t* must_match) const {
    const float grow_x = margin_tiles * config_.tile_size_x;
    const float grow_z = margin_tiles * config_.tile_size_z;
    // Both ends use the same floor, so a box whose edge lies exactly ON a seam reaches the tile on
    // the far side of it. That is deliberate: a body touching a seam is resting on both tiles.
    const std::int64_t x0 =
        tile_index(bounds.min.x - grow_x, config_.origin.x, config_.tile_size_x);
    const std::int64_t x1 =
        tile_index(bounds.max.x + grow_x, config_.origin.x, config_.tile_size_x);
    const std::int64_t z0 =
        tile_index(bounds.min.z - grow_z, config_.origin.z, config_.tile_size_z);
    const std::int64_t z1 =
        tile_index(bounds.max.z + grow_z, config_.origin.z, config_.tile_size_z);
    bool matched = true;
    for (std::int64_t x = x0; x <= x1; ++x) {
        for (std::int64_t z = z0; z <= z1; ++z) {
            TileKey key;
            key.x = static_cast<std::int32_t>(x);
            key.z = static_cast<std::int32_t>(z);
            if (!source_.current_revision(key.x, key.z, key.revision)) {
                continue; // no terrain at this coordinate in the manifest
            }
            if (must_match != nullptr && key.revision != *must_match) {
                matched = false;
            }
            out.push_back(key);
        }
    }
    return matched;
}

namespace {

// Whether `bounds` (grown by the margin) is a box this module will enumerate at all. Checked
// before collect_keys so that the loop above is always bounded.
[[nodiscard]] bool
enumerable(const physics::Aabb& bounds, float margin_tiles, const Config& config) noexcept {
    if (!finite(bounds) || bounds.min.x > bounds.max.x || bounds.min.z > bounds.max.z) {
        return false;
    }
    const float gx = margin_tiles * config.tile_size_x;
    const float gz = margin_tiles * config.tile_size_z;
    const std::int64_t sx = tile_index(bounds.max.x + gx, config.origin.x, config.tile_size_x) -
                            tile_index(bounds.min.x - gx, config.origin.x, config.tile_size_x);
    const std::int64_t sz = tile_index(bounds.max.z + gz, config.origin.z, config.tile_size_z) -
                            tile_index(bounds.min.z - gz, config.origin.z, config.tile_size_z);
    return sx < kMaxTileSpan && sz < kMaxTileSpan;
}

} // namespace

TerrainCollision::Record* TerrainCollision::find(const TileKey& key) noexcept {
    const auto it = std::lower_bound(records_.begin(),
                                     records_.end(),
                                     key,
                                     [](const Record& r, const TileKey& k) { return r.key < k; });
    return (it != records_.end() && it->key == key) ? &*it : nullptr;
}

const TerrainCollision::Record* TerrainCollision::find(const TileKey& key) const noexcept {
    const auto it = std::lower_bound(records_.begin(),
                                     records_.end(),
                                     key,
                                     [](const Record& r, const TileKey& k) { return r.key < k; });
    return (it != records_.end() && it->key == key) ? &*it : nullptr;
}

TerrainCollision::Record& TerrainCollision::find_or_create(const TileKey& key) {
    const auto it = std::lower_bound(records_.begin(),
                                     records_.end(),
                                     key,
                                     [](const Record& r, const TileKey& k) { return r.key < k; });
    if (it != records_.end() && it->key == key) {
        return *it;
    }
    Record record;
    record.key = key;
    return *records_.insert(it, record);
}

TileState TerrainCollision::tile_state(const TileKey& key) const noexcept {
    const Record* record = find(key);
    return record != nullptr ? record->state : TileState::Absent;
}

std::uint32_t TerrainCollision::pin_count(const TileKey& key, PinReason reason) const noexcept {
    const Record* record = find(key);
    return record != nullptr ? record->pins[static_cast<std::size_t>(reason)] : 0u;
}

physics::BodyId TerrainCollision::tile_body(const TileKey& key) const noexcept {
    const Record* record = find(key);
    return (record != nullptr && record->state == TileState::Installed) ? record->body
                                                                        : physics::BodyId{};
}

// ── Pins ──────────────────────────────────────────────────────────────────────────────────────

PinToken TerrainCollision::take_pin(std::vector<TileKey> keys, PinReason reason) {
    sort_unique(keys);
    for (const TileKey& key : keys) {
        // A pin on a tile nobody holds yet creates a PENDING record. It is not requested here:
        // requests are barrier step 3, so that when a load starts is a function of the tick the
        // pin was frozen into and not of the moment gameplay happened to ask.
        ++find_or_create(key).pins[static_cast<std::size_t>(reason)];
    }
    Pin pin;
    pin.id = next_pin_id_++;
    pin.reason = reason;
    pin.keys = std::move(keys);
    pins_.push_back(std::move(pin));
    ++counters_.pins_taken;
    ++counters_.pins_active;
    return PinToken{pins_.back().id};
}

bool TerrainCollision::drop_pin(std::uint64_t id) {
    const auto it =
        std::find_if(pins_.begin(), pins_.end(), [id](const Pin& p) { return p.id == id; });
    if (it == pins_.end()) {
        return false;
    }
    for (const TileKey& key : it->keys) {
        // A pinned record is never erased, so it is still here. The record itself is NOT removed
        // now even if this was its last pin: removal mutates the physics world, and that is the
        // barrier's job.
        if (Record* record = find(key)) {
            --record->pins[static_cast<std::size_t>(it->reason)];
        }
    }
    pins_.erase(it);
    ++counters_.pins_released;
    --counters_.pins_active;
    return true;
}

PinToken
TerrainCollision::pin(const physics::Aabb& bounds, std::uint32_t revision, PinReason reason) {
    if (!enumerable(bounds, 0.0f, config_)) {
        ++counters_.refused_budget_overflow;
        return {};
    }
    std::vector<TileKey> keys;
    if (!collect_keys(bounds, 0.0f, keys, &revision)) {
        ++counters_.refused_revision_mismatch;
        return {};
    }
    return take_pin(std::move(keys), reason);
}

bool TerrainCollision::unpin(PinToken token) {
    if (!token.is_valid() || !drop_pin(token.id)) {
        ++counters_.refused_unknown_pin;
        return false;
    }
    return true;
}

// ── Coverage and admission ────────────────────────────────────────────────────────────────────

bool TerrainCollision::covers(const physics::Aabb& bounds) const {
    if (!enumerable(bounds, 0.0f, config_)) {
        return false;
    }
    std::vector<TileKey> keys;
    (void)collect_keys(bounds, 0.0f, keys);
    return std::all_of(keys.begin(), keys.end(), [this](const TileKey& key) {
        return tile_state(key) == TileState::Installed;
    });
}

bool TerrainCollision::require_coverage(const physics::Aabb& bounds) {
    if (covers(bounds)) {
        return true;
    }
    ++counters_.coverage_refusals;
    return false;
}

AdmissionResult TerrainCollision::request_admission(std::uint64_t entity,
                                                    const physics::Aabb& bounds) {
    // A repeat request replaces the earlier one: the entity is being sent somewhere else, and the
    // tiles it was waiting for are no longer its business.
    const auto it = std::lower_bound(
        deferred_.begin(), deferred_.end(), entity, [](const Deferred& d, std::uint64_t e) {
            return d.entity < e;
        });
    const bool had = it != deferred_.end() && it->entity == entity;
    if (had && it->pin.is_valid()) {
        (void)drop_pin(it->pin.id);
        it->pin = {};
    }

    const bool usable = enumerable(bounds, 0.0f, config_);
    if (!usable) {
        // Not a place an entity can be put. It is held out — permanently, until the caller asks
        // again with real bounds or cancels — and the reason is counted.
        ++counters_.refused_budget_overflow;
    }
    if (usable && covers(bounds)) {
        if (had) {
            deferred_.erase(it);
        }
        ++counters_.admissions_immediate;
        return AdmissionResult::Admitted;
    }

    Deferred entry;
    entry.entity = entity;
    entry.bounds = bounds;
    if (usable) {
        std::vector<TileKey> keys;
        (void)collect_keys(bounds, 0.0f, keys);
        // The pin is what makes the wait productive: it gets the tiles requested at the next
        // barrier and keeps them from being evicted before the entity that wants them arrives.
        entry.pin = take_pin(std::move(keys), PinReason::Admission);
    }
    if (had) {
        *it = entry;
    } else {
        deferred_.insert(it, entry);
    }
    ++counters_.admissions_deferred;
    return AdmissionResult::Deferred;
}

bool TerrainCollision::cancel_admission(std::uint64_t entity) {
    const auto it = std::lower_bound(
        deferred_.begin(), deferred_.end(), entity, [](const Deferred& d, std::uint64_t e) {
            return d.entity < e;
        });
    if (it == deferred_.end() || it->entity != entity) {
        return false;
    }
    if (it->pin.is_valid()) {
        (void)drop_pin(it->pin.id);
    }
    deferred_.erase(it);
    ++counters_.admissions_cancelled;
    return true;
}

// ── Barrier step 2: the plan ──────────────────────────────────────────────────────────────────

Plan TerrainCollision::plan(std::uint64_t tick, std::span<const Demand> demands) {
    // The entities admitted at the previous barrier are simulated bodies now, and the caller has
    // put them in `demands`; their admission pins have done their job. Dropping them HERE, before
    // the eviction list is computed, is what hands the tile from "pinned for an arrival" to "under
    // a body" with no barrier in between at which it is neither.
    for (const PinToken token : admitted_pins_) {
        (void)drop_pin(token.id);
    }
    admitted_pins_.clear();
    // …and the list that named them is spent: `admitted()` is meaningful only between a Ready
    // commit and the next plan. Cleared here rather than at the next Ready so that a caller who
    // reads it after a `Waiting` or `Failed` answer sees nothing, not last tick's arrivals again.
    admitted_.clear();

    Plan out;
    out.tick = tick;
    out.serial = ++plan_serial_;
    plan_open_ = true;

    // Required: (key, kind) pairs, merged so one tile under a body AND a sweep carries both bits.
    std::vector<std::pair<TileKey, std::uint8_t>> required;
    std::vector<TileKey> scratch;
    std::vector<TileKey> retain;
    for (const Demand& demand : demands) {
        // The retain envelope is the widest box this demand is ever enumerated at; if that is not
        // enumerable none of them is trusted.
        if (!enumerable(demand.bounds, config_.retain_margin_tiles, config_)) {
            out.overflow = true;
            continue;
        }
        scratch.clear();
        (void)collect_keys(demand.bounds, 0.0f, scratch);
        for (const TileKey& key : scratch) {
            required.emplace_back(key, kind_bit(demand.kind));
        }
        (void)collect_keys(demand.bounds, config_.activate_margin_tiles, out.activate);
        (void)collect_keys(demand.bounds, config_.retain_margin_tiles, retain);
    }
    std::sort(required.begin(), required.end());
    for (const auto& [key, bit] : required) {
        if (!out.required.empty() && out.required.back() == key) {
            out.required_kinds.back() |= bit;
        } else {
            out.required.push_back(key);
            out.required_kinds.push_back(bit);
        }
    }

    // Pinned tiles are wanted and kept, whatever the demands say.
    out.required_or_pinned = out.required.size();
    for (const Record& record : records_) {
        if (record.pin_total() > 0) {
            out.activate.push_back(record.key);
            retain.push_back(record.key);
            if (!contains(out.required, record.key)) {
                ++out.required_or_pinned;
            }
        }
    }
    sort_unique(out.activate);
    sort_unique(retain);

    // Everything held that the retain envelope no longer reaches. `records_` is sorted, so this
    // list is too.
    for (const Record& record : records_) {
        if (!contains(retain, record.key)) {
            out.deactivate.push_back(record.key);
        }
    }

    if (out.required_or_pinned > config_.max_required_tiles) {
        out.overflow = true;
    }
    return out;
}

// ── Barrier steps 3–8 ─────────────────────────────────────────────────────────────────────────

void TerrainCollision::fail(Record& record) {
    // Give the ownership back now rather than at deactivation: a Failed record keeps its place (so
    // the tile is not re-requested every barrier — no retry storm) but holds nothing.
    if (record.handle.is_valid()) {
        (void)source_.release(record.handle);
        record.handle = {};
    }
    record.state = TileState::Failed;
}

void TerrainCollision::try_install(Record& record, std::uint64_t tick, bool required) {
    // THE ONLY PLACE READINESS IS READ. Everything a load completion did before this line was to
    // change what `state` answers; nothing was installed, selected or scheduled by it.
    const assets::AssetState state = source_.state(record.handle);
    const std::uint64_t scheduled = record.requested_tick + config_.activation_lead_ticks;
    if (state != assets::AssetState::Ready && tick >= scheduled && !record.late) {
        // The tick this tile was scheduled to appear on has come and the load has not. From here
        // its install tick is set by the disk, not by the request — so it is said, once per
        // request, rather than left to look like an on-time install. Whether anything WAITS for
        // it is not decided here: a body that requires the tile stalls the tick (step 5), an
        // arriving entity stays deferred (step 6), and a tile nobody is on simply appears late.
        record.late = true;
        ++counters_.install_late;
    }
    if (state == assets::AssetState::Loading) {
        return;
    }
    if (state == assets::AssetState::Failed) {
        ++counters_.load_failures;
        fail(record);
        return;
    }
    if (state == assets::AssetState::Stale) {
        // Our ownership is gone — something released this slot out from under us. The handle may
        // by now name another tile entirely, so it is neither read nor released; ask again.
        ++counters_.refused_stale_handle;
        record.handle = source_.request(record.key);
        ++counters_.requests;
        if (!record.handle.is_valid()) {
            ++counters_.load_failures;
            fail(record);
        }
        return;
    }

    // Ready. A prefetched tile waits out its lead so that the tick it appears in the physics world
    // is set by when it was asked for, not by how fast the disk was. A REQUIRED tile never waits:
    // the lead is a determinism aid for the envelope, not a reason to stall a body. That early
    // install is still a function of the tick history alone — a required tile is installed on the
    // tick it first became required whether the load was fast (here) or slow (the stall holds the
    // tick until it is) — and it is counted so that it is not a silent exception to the schedule.
    if (tick < scheduled) {
        if (!required) {
            ++counters_.installs_held_for_lead;
            return;
        }
        ++counters_.installs_required_early;
    }

    const TilePayload payload = source_.resolve(record.handle);
    if (payload.asset == nullptr) {
        // Ready a moment ago and unresolvable now: the same lost-ownership case as Stale.
        ++counters_.refused_stale_handle;
        record.handle = {};
        fail(record);
        return;
    }
    if (payload.revision != record.key.revision) {
        ++counters_.refused_revision_mismatch;
        fail(record);
        return;
    }
    if (!fits_grid(*payload.asset, record.key, config_)) {
        ++counters_.refused_invalid_content;
        fail(record);
        return;
    }

    const assets::HeightfieldAsset& asset = *payload.asset;
    physics::HeightfieldDesc desc;
    desc.samples = asset.samples;
    desc.columns = asset.columns;
    desc.rows = asset.rows;
    desc.cell_size_x = asset.cell_size_x;
    desc.cell_size_z = asset.cell_size_z;
    desc.height_scale = asset.height_scale;
    desc.height_offset = asset.height_offset;
    desc.thickness = config_.thickness;
    const physics::HeightfieldId shape = world_.register_heightfield(desc);
    if (!shape.is_valid()) {
        ++counters_.refused_invalid_content;
        fail(record);
        return;
    }

    physics::BodyDesc body;
    body.motion = physics::MotionType::Static;
    body.shape.type = physics::ShapeType::Heightfield;
    body.shape.heightfield = shape;
    body.position = asset.origin;
    body.friction = config_.friction;
    const physics::BodyId id = world_.create_body(body);
    if (!id.is_valid()) {
        (void)world_.unregister_heightfield(shape);
        ++counters_.refused_invalid_content;
        fail(record);
        return;
    }

    record.shape = shape;
    record.body = id;
    record.bytes = asset.samples.size() * sizeof(std::uint16_t);
    record.state = TileState::Installed;
    // The physics world copied the samples at registration, so this module has no further use for
    // the asset: give the ownership back NOW rather than at deactivation. On a server that halves
    // what an installed tile costs; on a client the render residency holds its own ownership of the
    // same slot (ADR-0067: one request, one release), so nothing is reloaded there. `asset` dangles
    // after this line — nothing below may touch it.
    if (!source_.release(record.handle)) {
        ++counters_.refused_stale_handle;
    }
    record.handle = {};
    ++counters_.installs;
    ++counters_.tiles_installed;
    counters_.bytes_resident += record.bytes;
    counters_.peak_bytes_resident =
        std::max(counters_.peak_bytes_resident, counters_.bytes_resident);
    journal_.push_back({tick, record.key, JournalOp::Activate});
}

void TerrainCollision::deactivate(Record& record, std::uint64_t tick) {
    if (record.state == TileState::Installed) {
        // Body first: unregister_heightfield is reject-if-referenced, and this body is the only
        // reference this module ever made.
        world_.destroy_body(record.body);
        (void)world_.unregister_heightfield(record.shape);
        counters_.bytes_resident -= record.bytes;
        --counters_.tiles_installed;
        ++counters_.evictions;
        journal_.push_back({tick, record.key, JournalOp::Deactivate});
    } else if (record.state == TileState::Requested) {
        ++counters_.requests_cancelled;
    }
    // Exactly once. Only a record still LOADING holds a handle by now: an installed tile gave its
    // ownership back at install, and `fail` clears the handle when it releases.
    if (record.handle.is_valid()) {
        if (!source_.release(record.handle)) {
            ++counters_.refused_stale_handle;
        }
        record.handle = {};
    }
}

CommitStatus TerrainCollision::try_commit(const Plan& plan) {
    if (!plan_open_ || plan.serial != plan_serial_) {
        // Either a newer plan superseded this one, or this one was already committed. Applying its
        // eviction list now would act on a world it was not computed against.
        ++counters_.refused_stale_plan;
        return CommitStatus::Failed;
    }
    if (plan.overflow) {
        if (overflow_serial_ != plan.serial) {
            overflow_serial_ = plan.serial;
            ++counters_.refused_budget_overflow;
        }
        return CommitStatus::Failed; // before anything is requested or installed
    }

    // 3. Request. Idempotent across the retries of a stalled tick: a record that already has a
    //    handle is left alone, so `requested_tick` is the tick that FIRST wanted the tile.
    for (const TileKey& key : plan.activate) {
        Record& record = find_or_create(key);
        if (record.state != TileState::Pending) {
            continue;
        }
        record.handle = source_.request(key);
        record.requested_tick = plan.tick;
        ++counters_.requests;
        if (record.handle.is_valid()) {
            record.state = TileState::Requested;
        } else {
            ++counters_.load_failures;
            fail(record);
        }
    }

    // 4. Install whatever is ready, in key order. The order is the point: body and shape ids are
    //    handed out by the physics world in call order, so walking the READY SET sorted makes them
    //    independent of the order the loads finished in.
    for (Record& record : records_) {
        if (record.state == TileState::Requested) {
            try_install(record, plan.tick, contains(plan.required, record.key));
        }
    }

    // 5. The stall check — the safety net. Nothing below this line runs for a stalled tick, which
    //    is what lets the caller retry with the same plan and find the barrier exactly as it left
    //    it apart from the installs above.
    std::uint8_t missing = 0;
    bool required_failed = false;
    for (std::size_t i = 0; i < plan.required.size(); ++i) {
        const TileState state = tile_state(plan.required[i]);
        if (state == TileState::Failed) {
            required_failed = true;
        } else if (state != TileState::Installed) {
            missing |= plan.required_kinds[i];
        }
    }
    if (required_failed) {
        // No amount of waiting installs a tile whose load failed or whose content was refused.
        // Terminal, and said so, rather than a stall that never ends.
        ++counters_.refused_required_failed;
        return CommitStatus::Failed;
    }
    if (missing != 0) {
        if (stalled_serial_ != plan.serial) {
            stalled_serial_ = plan.serial;
            if ((missing & kBodyBit) != 0) {
                ++counters_.stalls_admitted_body;
            }
            if ((missing & kSweepBit) != 0) {
                ++counters_.stalls_sweep;
            }
        }
        ++counters_.stall_retries;
        return CommitStatus::Waiting;
    }

    // 6. Deferred admissions, in entity order. ALL tiles, not some: an entity straddling a seam
    //    with one side missing would be admitted onto half a floor.
    for (auto it = deferred_.begin(); it != deferred_.end();) {
        if (it->pin.is_valid() && covers(it->bounds)) {
            admitted_.push_back(it->entity);
            // The pin outlives the admission by one barrier — see `plan`.
            admitted_pins_.push_back(it->pin);
            ++counters_.admitted_after_deferral;
            it = deferred_.erase(it);
        } else {
            ++counters_.admission_retries;
            if (!it->failure_reported && it->pin.is_valid()) {
                // Is it waiting for something that can still arrive? A tile that failed to load or
                // was refused will not: this entity is stuck until the caller cancels or re-aims
                // it. There is no timeout policy here (ADR-0068), so the least this can do is make
                // the stuck admission visible — one count and one warning per deferral, not one
                // per barrier.
                std::vector<TileKey> keys;
                (void)collect_keys(it->bounds, 0.0f, keys);
                for (const TileKey& key : keys) {
                    if (tile_state(key) == TileState::Failed) {
                        it->failure_reported = true;
                        ++counters_.admissions_blocked_by_failure;
                        RIME_WARN("terrain_collision: admission of entity {} is blocked — tile "
                                  "({}, {}) rev {} failed to load or was refused; it stays "
                                  "deferred until cancelled",
                                  it->entity,
                                  key.x,
                                  key.z,
                                  key.revision);
                        break;
                    }
                }
            }
            ++it;
        }
    }

    // 7. Deactivate, in key order. A tile pinned since the plan was made is left alone and
    //    counted; the next plan will not list it.
    for (const TileKey& key : plan.deactivate) {
        Record* record = find(key);
        if (record == nullptr) {
            continue;
        }
        if (record->pin_total() > 0) {
            ++counters_.evictions_skipped_pinned;
            continue;
        }
        deactivate(*record, plan.tick);
        records_.erase(records_.begin() + (record - records_.data()));
    }

    plan_open_ = false;
    return CommitStatus::Ready;
}

} // namespace rime::terrain_collision
