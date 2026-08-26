// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "match_fixture.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"

// m12.3's weapon → destruction glue, proven WHERE IT ACTUALLY LIVES: in the consumer.
//
// THIS FILE IS THE ONLY ONE IN THE SUITE THAT LINKS rime::destruction, and that is the point.
// `engine/gameplay_net` refuses that dependency (see its CMakeLists) for the same reason the M8.4
// fan-out keeps `rime::vfx` from linking destruction: a shot reports WHAT it hit, and deciding that
// "what" means "erode this part" is a game rule. So `GameplayServer` emits `ShotEvent`s carrying
// the resolved damage arguments, and roughly twenty lines below — `apply_shot`, the glue a real
// game writes once — turn them into `destruction::apply_damage` calls.
//
// The translation chain the glue walks, each link already built and none of it new here:
//
//     ShotEvent.body   →  the destructible INSTANCE standing on that body   (body_of)
//     ShotEvent.child  →  the PART index inside it                          (part_from_child)
//     ShotEvent.point / damage / damage_radius / impulse  →  apply_damage
//
// `RayHit::child == compound child index` and `part_from_child` is the one true mapping for it
// (ADR-0029 §4). That equivalence is why hitscan can name a part without the weapon knowing what a
// part is.
using namespace rime;
using namespace rime_test;

namespace {

// The cooked wall: 2 × 1.5 × 0.3 m fractured into 16 parts (the committed m8.1 fixture, shared
// with the destruction suite and the assets oracle).
constexpr std::uint64_t kWallAsset = 0xABCDull;
constexpr float kWallHalfY = 0.75f;

std::vector<std::byte> read_fixture(const std::string& name) {
    const std::string path = std::string(RIME_DESTRUCTION_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open fixture: " << path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

assets::DestructibleAsset load_wall() {
    const std::vector<std::byte> file = read_fixture("wall.rdest");
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    REQUIRE_MESSAGE(asset.has_value(), "wall.rdest failed to decode: " << assets::to_string(err));
    return std::move(*asset);
}

// What one glue pass did, so the tests can assert on the mechanism rather than only on its effect.
struct GlueStats {
    int events = 0;
    int hits = 0;
    int damaged = 0;      // hits that reached a destructible part
    int not_a_target = 0; // hits on the floor, on debris, on anything with no instance behind it
    destruction::InstanceId last_instance{};
    std::uint32_t last_part = destruction::kInvalidPartIndex;
};

// THE GLUE. A real game writes this once and never thinks about it again; it is reproduced here in
// full rather than hidden in a helper library because the whole architectural claim of §3 is that
// it is SHORT and that it belongs to the consumer.
//
// The body → instance direction is a linear scan, which is honest for a handful of destructibles
// and wrong for a city block: a game at m12.7's scale keeps a body → instance map beside its
// destruction world (the same shape `build_instance_entity_table` builds for the other direction).
// Saying so here is cheaper than discovering it in a profile.
GlueStats apply_shots(std::span<const gameplay_net::ShotEvent> shots,
                      destruction::DestructionWorld& destruction) {
    GlueStats stats;
    for (const gameplay_net::ShotEvent& shot : shots) {
        ++stats.events;
        if (!shot.did_hit) {
            continue; // a miss is still an event — a tracer and a report, but no damage
        }
        ++stats.hits;

        destruction::InstanceId instance{};
        for (std::size_t i = 0; i < destruction.instance_count(); ++i) {
            const destruction::InstanceId candidate{static_cast<std::uint32_t>(i), 0};
            if (destruction.body_of(candidate) == shot.body) {
                instance = candidate;
                break;
            }
        }
        if (!instance.is_valid()) {
            ++stats.not_a_target; // the floor, a crate, a debris chunk — not our business
            continue;
        }

        const std::uint32_t part = destruction.part_from_child(instance, shot.child);
        if (part == destruction::kInvalidPartIndex) {
            ++stats.not_a_target;
            continue;
        }

        destruction.apply_damage(
            instance, shot.point, shot.damage_radius, shot.damage, shot.impulse);
        ++stats.damaged;
        stats.last_instance = instance;
        stats.last_part = part;
    }
    return stats;
}

// A server-side destructible world standing one wall in front of the spawn point.
struct Range {
    destruction::DestructionWorld destruction;
    destruction::PatternId pattern{};
    ecs::Entity wall = ecs::kNullEntity;

    void stand(Match& match, float z) {
        const assets::DestructibleAsset asset = load_wall();
        pattern = destruction.register_pattern(asset, match.physics);
        REQUIRE(pattern.is_valid());

        core::Transform placement;
        placement.translation = {0.0f, kWallHalfY, z};
        wall = match.world.spawn_with(ecs::LocalTransform{placement},
                                      ecs::WorldTransform{placement},
                                      destruction::Destructible{kWallAsset});
        (void)match.replicator->replicate(wall);

        const destruction::BindStats bound = destruction::bind_destructibles(
            match.world,
            destruction,
            match.physics,
            [this](std::uint64_t id) {
                return id == kWallAsset ? pattern : destruction::PatternId{};
            },
            destruction::Authority::Local);
        REQUIRE(bound.bound == 1);
        REQUIRE(destruction.instance_count() == 1);
    }

    [[nodiscard]] destruction::InstanceId instance() const { return destruction::InstanceId{0, 0}; }

    // One tick of the whole system, with the glue in the sequential tail where ADR-0029 §8 puts
    // the destruction update: after the physics step, never during it.
    GlueStats tick(Match& match) {
        match.tick();
        const GlueStats stats = apply_shots(match.gameplay.shots(), destruction);
        destruction.update(match.physics);
        return stats;
    }
};

[[nodiscard]] replication::InputCommand fire(float move_x = 0.0f, float move_y = 0.0f) {
    replication::InputCommand c = walk(move_x, move_y);
    c.pressed = gameplay::kActionFire;
    c.held = gameplay::kActionFire;
    return c;
}

} // namespace

TEST_CASE("a shot fired across the wire damages the part it named") {
    Match match(Match::Options{/*with_destruction=*/true});
    add_ground(match.physics);
    Range range;
    range.stand(match, -4.0f);

    match.weapon_config.cooldown_ticks = 0;
    ClientPeer& client = match.add_client();
    match.settle();
    REQUIRE(match.spawned.size() == 1);

    const destruction::InstanceId instance = range.instance();
    const std::uint32_t parts = range.destruction.instance_part_count(instance);
    REQUIRE(parts >= 8);
    std::vector<float> before(parts);
    for (std::uint32_t p = 0; p < parts; ++p) {
        before[p] = range.destruction.part_health(instance, p);
        REQUIRE(before[p] > 0.0f);
    }

    // One command, one trigger pull, aimed straight ahead at the wall's middle.
    (void)client.send_input(fire(), match.now_ms);
    const GlueStats glue = range.tick(match);

    // The whole chain fired: a command was consumed, a shot left the barrel, it hit, the glue
    // resolved it to a part, and the damage was queued.
    CHECK(match.gameplay.commands_consumed() == 1);
    CHECK(match.gameplay.shots_fired() == 1);
    CHECK(match.gameplay.shots_hit() == 1);
    CHECK(match.gameplay.fire_stats().hits == 1);
    CHECK(glue.events == 1);
    CHECK(glue.hits == 1);
    CHECK(glue.damaged == 1);
    CHECK(glue.not_a_target == 0);
    REQUIRE(glue.last_part != destruction::kInvalidPartIndex);

    // The part the RAY named is a part that lost health.
    //
    // NOT "the part that lost the MOST", which is what this assertion said on its first draft and
    // is a different claim entirely: `apply_damage`'s falloff is measured to each part's cooked
    // AABB, those AABBs overlap heavily on a Voronoi fracture, and health is proportional to
    // volume — so a large neighbour at distance 0 can absorb more absolute damage than the small
    // part the ray actually pierced, whose loss is capped by the health it had. Measured on this
    // fixture: the ray named part 4 and part 1 lost more. That is the FALLOFF's business, and
    // asserting it here would pin the addressing test to a damage model it does not test.
    float total_loss = 0.0f;
    for (std::uint32_t p = 0; p < parts; ++p) {
        total_loss += before[p] - range.destruction.part_health(instance, p);
    }
    CHECK(total_loss > 0.0f); // non-vacuity: something really was eroded
    CHECK(before[glue.last_part] - range.destruction.part_health(instance, glue.last_part) > 0.0f);

    // THE ADDRESSING CLAIM, stated so it can fail: aiming somewhere else names a DIFFERENT part.
    // A `child` that were stuck at a constant — or a remap that ignored its argument — would pass
    // every assertion above and fail this one.
    replication::InputCommand high = fire();
    // Aimed DOWN, not up: the eye sits at ~1.43 m and the wall's top edge at 1.5 m, ~3.85 m away,
    // so a positive pitch of any size sails over it and the "different part" claim would be tested
    // by a miss. Pitching down lands on the lower row.
    high.pitch = -0.25f;
    (void)client.send_input(high, match.now_ms);
    const GlueStats second = range.tick(match);
    REQUIRE(second.damaged == 1);
    CHECK(second.last_part != glue.last_part);
}

TEST_CASE("a miss is delivered as an event and damages nothing") {
    // `did_hit == false` still produces a ShotEvent, because a tracer, a muzzle flash and a report
    // all happen on a miss — the m12.6 FX families read exactly this list. An event stream that
    // carried only hits would make missing silent.
    Match match(Match::Options{true});
    add_ground(match.physics);
    Range range;
    range.stand(match, -4.0f);
    match.weapon_config.cooldown_ticks = 0;
    ClientPeer& client = match.add_client();
    match.settle();

    const destruction::InstanceId instance = range.instance();
    const float before = range.destruction.part_health(instance, 0);

    // Aimed 90 degrees away from the wall and level, so it flies off over the ground plane's edge.
    replication::InputCommand shot = fire();
    shot.yaw = 3.14159265f; // about-face: forward becomes +Z
    (void)client.send_input(shot, match.now_ms);
    const GlueStats glue = range.tick(match);

    CHECK(match.gameplay.shots_fired() == 1);
    CHECK(match.gameplay.shots_hit() == 0);
    CHECK(match.gameplay.fire_stats().misses == 1);
    CHECK(glue.events == 1); // the event exists…
    CHECK(glue.hits == 0);   // …and carries no hit
    CHECK(glue.damaged == 0);
    CHECK(range.destruction.part_health(instance, 0) == doctest::Approx(before));
}

TEST_CASE("the shot is resolved from the pose the move produced, not the one it started from") {
    // The ordering commitment inside `apply_command`: step_character FIRST, then the weapon. A
    // player who steps out of cover and fires in the same command has left cover by the time the
    // ray is cast — which is what the input said and what the player will see. Resolving before
    // the move fires from where they used to be, an off-by-one-tick that presents as "my shots
    // clip the corner I just left".
    //
    // Asserted DIRECTLY on the shot's origin rather than through geometry: the origin must be the
    // eye above the POST-move position, bit for bit. A geometric proof would need the two poses to
    // straddle a corner, which at 0.1 m of travel per tick is a knife-edge that would fail for
    // reasons having nothing to do with the ordering.
    Match match(Match::Options{true});
    add_ground(match.physics);
    Range range;
    range.stand(match, -4.0f);
    match.weapon_config.cooldown_ticks = 0;
    ClientPeer& client = match.add_client();
    match.settle();
    const ecs::Entity avatar = match.spawned.front();

    // Build up real speed first: from a standstill one tick of acceleration barely moves anything,
    // so a single move-and-fire command would compare two poses that are equal to a rounding.
    for (int i = 0; i < 30; ++i) {
        (void)client.send_input(walk(1.0f, 0.0f), match.now_ms);
        (void)range.tick(match);
    }
    const gameplay::CharacterState before = *match.server_state(avatar);

    (void)client.send_input(fire(/*move_x=*/1.0f, /*move_y=*/0.0f), match.now_ms);
    const GlueStats glue = range.tick(match);
    REQUIRE(glue.events == 1);
    const gameplay::CharacterState after = *match.server_state(avatar);

    // The move really happened on the same command that fired.
    CHECK(after.position.x > before.position.x);

    const gameplay_net::ShotEvent& shot = match.gameplay.shots().front();
    const float eye = match.weapon_config.eye_height;
    CHECK(shot.origin.x == after.position.x);
    CHECK(shot.origin.y == after.position.y + eye);
    CHECK(shot.origin.z == after.position.z);
    // …and NOT the pre-move pose. This is the assertion that fails if the two lines are swapped.
    CHECK(shot.origin.x != before.position.x);
}

TEST_CASE("sustained fire brings the wall down, end to end") {
    // The full path, run long enough to cross the fracture boundary: input over the wire → consume
    // → hitscan → glue → damage → support solve → the body swap. It is the m12.3 shape of ADR-0035
    // §1's "the fusion actually runs" clause, one client and one wall instead of a city block.
    Match match(Match::Options{true});
    add_ground(match.physics);
    Range range;
    range.stand(match, -4.0f);

    // A rifle, not a demolition charge. The first draft of this case used damage 120 over a 0.35 m
    // radius and levelled all sixteen parts with ONE shot — which then made every later shot land
    // in the rubble, where there is no instance to damage, and the test measured a burst that had
    // nothing left to shoot. Sustained fire has to be sustained to prove anything.
    match.weapon_config.cooldown_ticks = 0;
    match.weapon_config.automatic = true;
    match.weapon_config.damage = 0.15f;
    match.weapon_config.damage_radius = 0.10f;
    ClientPeer& client = match.add_client();
    match.settle();

    const destruction::InstanceId instance = range.instance();
    const std::uint32_t parts = range.destruction.instance_part_count(instance);
    REQUIRE(parts >= 8);

    int total_damaged = 0;
    int total_hits = 0;
    int into_rubble = 0;
    int first_death_tick = -1;
    int last_death_tick = -1;
    int damaged_before_breach = 0;
    std::uint32_t dead = 0;

    // Sweep the aim so the shots do not all land on one part — a wall that loses one part is not a
    // wall coming down.
    constexpr int kBurst = 400;
    for (int i = 0; i < kBurst; ++i) {
        replication::InputCommand shot = fire();
        // The sweep is BIASED DOWNWARD and kept narrow, because the eye is level with the wall's
        // top edge: an unbiased pitch sweep spends half its shots over the top, and the burst then
        // measures the sky. Centre −0.12 rad puts the aim at ~0.97 m on a wall spanning 0 to 1.5.
        shot.yaw = 0.09f * std::sin(static_cast<float>(i) * 0.11f);
        shot.pitch = -0.27f + 0.05f * std::cos(static_cast<float>(i) * 0.07f);
        (void)client.send_input(shot, match.now_ms);
        const GlueStats glue = range.tick(match);
        total_damaged += glue.damaged;
        total_hits += glue.hits;
        into_rubble += glue.not_a_target;

        std::uint32_t now_dead = 0;
        for (std::uint32_t p = 0; p < parts; ++p) {
            if (!range.destruction.part_alive(instance, p)) {
                ++now_dead;
            }
        }
        if (now_dead > dead) {
            if (first_death_tick < 0) {
                first_death_tick = i;
                damaged_before_breach = total_damaged;
            }
            last_death_tick = i;
            dead = now_dead;
        }
    }

    MESSAGE("m12.3 end-to-end: " << total_hits << " hits (" << total_damaged << " reached a part, "
                                 << into_rubble << " landed in rubble), " << dead << "/" << parts
                                 << " parts left the wall between ticks " << first_death_tick
                                 << " and " << last_death_tick << "; " << damaged_before_breach
                                 << " damaging shots to breach it.");

    CHECK(total_hits > 100); // the aim sweep stayed on the wall for most of the burst

    // IT TOOK SUSTAINED FIRE TO BREACH. This is the assertion that fails if the weapon is retuned
    // back into a demolition charge — the exact regression this file already caught once, when a
    // 45-damage default against 1.0-health parts levelled the wall on the opening shot.
    REQUIRE(first_death_tick >= 0);
    CHECK(damaged_before_breach >= 3);

    // …and once breached it CASCADES rather than crumbling part by part: this fixture hangs 16
    // parts off 5 anchors in its bottom row, so losing a load-bearing one unsupports its
    // neighbours, which unsupport theirs. Deaths spread across several ticks; they do not all land
    // in one. That is the support solve doing its job, and asserting the spread is what would
    // catch it degrading into "everything detaches at once" or "nothing ever detaches".
    CHECK(dead > 0);
    CHECK(last_death_tick > first_death_tick);

    CHECK(range.destruction.debris_count() > 0); // the rubble exists as real bodies

    // A shot into the rubble is NOT a hit on the wall: debris bodies carry no instance, and
    // `body_of` is null once an instance has fully collapsed, so the glue counts them and applies
    // nothing. Non-zero by the end of a burst this long, and correct — the alternative would be
    // damage landing on a wall that is no longer there.
    CHECK(into_rubble > 0);

    // Nothing went wrong on the way: no unbound player, no dropped shot events, no orphaned ids.
    CHECK(match.gameplay.players_unbound() == 0);
    CHECK(match.gameplay.shot_events_dropped() == 0);
    CHECK(match.replicator->net_ids_orphaned() == 0);
}
