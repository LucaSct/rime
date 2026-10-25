// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "rime/replication/spawn_prediction.hpp"

using namespace rime::replication;

// A toy 1-D game with a ball that is "thrown" (ADR-0105 §4 in miniature): three charges, a short
// cooldown, at most one live ball, a fixed velocity and a fixed lifetime. The same rules run in a
// toy SERVER (authority: acceptance, identity, the end of the flight) and in the client's predicted
// step, so every test below is the real reconciliation loop over a deterministic delayed link --
// not a hand-built sequence of checkpoints that could encode the answer it is meant to test.
namespace {

constexpr PredictedEntityId kPlayer{1, 1};
constexpr std::uint32_t kInc = 7;
constexpr std::uint8_t kLife = 8;
constexpr std::uint8_t kCooldown = 2;
constexpr std::uint32_t kThrow = 1;
constexpr double kDt = 1.0;

std::vector<std::byte> f32(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return {std::byte(bits & 0xFF),
            std::byte((bits >> 8) & 0xFF),
            std::byte((bits >> 16) & 0xFF),
            std::byte((bits >> 24) & 0xFF)};
}

float f32_of(std::span<const std::byte> bytes) {
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        bits |= std::uint32_t{std::to_integer<std::uint8_t>(bytes[i])} << (8 * i);
    return std::bit_cast<float>(bits);
}

struct Ball {
    float x = 0.0f;
    std::uint8_t age = 0;
};

std::vector<std::byte> ball_bytes(Ball ball) {
    auto bytes = f32(ball.x);
    bytes.push_back(std::byte{ball.age});
    return bytes;
}

Ball ball_of(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() == 5);
    return {f32_of(bytes), std::to_integer<std::uint8_t>(bytes[4])};
}

std::uint64_t pack(PredictedEntityId id) {
    return (std::uint64_t{id.index} << 32) | id.generation;
}

InputCommand neutral() {
    return {};
}

InputCommand throw_command() {
    InputCommand command{};
    command.pressed = kThrow;
    command.held = kThrow;
    return command;
}

struct ClientGame {
    float player_x = 0.0f;
    std::uint8_t charges = 3;
    std::uint8_t cooldown = 0;
    std::map<std::uint64_t, std::vector<std::byte>> balls;
    // Presentation's view: which key each ball is the projectile for.
    std::map<std::uint64_t, SpawnKey> key_of;
    SpawnReconciler* reconciler = nullptr;
    // The deliberately broken policy of the sensitivity control: confirmation creates the replica
    // beside the guess instead of renaming the guess.
    bool ignore_adoption = false;
    std::size_t launches_simulated = 0;
    std::vector<PredictedEntityId> provisional_creates;

    PredictionHooks hooks() {
        return {[this](PredictedEntityId id) {
                    return id == kPlayer ? f32(player_x) : balls.at(pack(id));
                },
                [this](PredictedEntityId id, std::span<const std::byte> bytes) {
                    if (id == kPlayer) {
                        player_x = f32_of(bytes);
                        return;
                    }
                    // SpawnHooks::create always precedes the first restore of a ball.
                    REQUIRE(balls.contains(pack(id)));
                    balls[pack(id)].assign(bytes.begin(), bytes.end());
                },
                [this] { return std::vector<std::byte>{std::byte{charges}, std::byte{cooldown}}; },
                [this](std::span<const std::byte> bytes) {
                    REQUIRE(bytes.size() == 2);
                    charges = std::to_integer<std::uint8_t>(bytes[0]);
                    cooldown = std::to_integer<std::uint8_t>(bytes[1]);
                },
                [this](std::span<const PredictedEntityId> members,
                       const InputCommand& input,
                       const PredictionStep& step) {
                    if (cooldown > 0)
                        --cooldown;
                    player_x += input.move_x * static_cast<float>(step.fixed_dt);
                    bool live_ball = false;
                    for (const auto& id : members) {
                        if (id == kPlayer)
                            continue;
                        auto ball = ball_of(balls.at(pack(id)));
                        if (ball.age < kLife) {
                            ++ball.age;
                            ball.x += 1.0f;
                        }
                        balls[pack(id)] = ball_bytes(ball);
                        live_ball = live_ball || ball.age < kLife;
                    }
                    if ((input.pressed & kThrow) != 0 && charges > 0 && cooldown == 0 &&
                        !live_ball) {
                        --charges;
                        cooldown = kCooldown;
                        ++launches_simulated;
                        const SpawnKey key{kInc, input.sequence, 0};
                        if (const auto id =
                                reconciler->spawn_provisional(key, ball_bytes({player_x, 0})))
                            key_of[pack(*id)] = key;
                    }
                }};
    }

    SpawnHooks spawn_hooks() {
        return {[this](PredictedEntityId id) {
                    CHECK_FALSE(balls.contains(pack(id)));
                    balls[pack(id)] = {};
                    if ((id.index & kProvisionalIndexBit) != 0)
                        provisional_creates.push_back(id);
                },
                [this](PredictedEntityId id) {
                    CHECK(balls.erase(pack(id)) == 1);
                    key_of.erase(pack(id));
                },
                [this](PredictedEntityId provisional, PredictedEntityId authoritative) {
                    if (ignore_adoption) {
                        balls[pack(authoritative)] = {};
                        return;
                    }
                    auto node = balls.extract(pack(provisional));
                    REQUIRE_FALSE(node.empty());
                    node.key() = pack(authoritative);
                    balls.insert(std::move(node));
                    key_of[pack(authoritative)] = key_of.at(pack(provisional));
                    key_of.erase(pack(provisional));
                }};
    }
};

struct Server {
    float player_x = 0.0f;
    std::uint8_t charges = 3;
    std::uint8_t cooldown = 0;
    std::uint32_t epoch = 1;
    std::uint64_t tick = 0;

    struct Live {
        PredictedEntityId id{};
        SpawnKey key{};
        Ball ball{};
    };

    std::optional<Live> ball;
    std::uint32_t next_index = 100;
    std::vector<SpawnOutcome> outcomes;
    std::vector<std::pair<std::uint64_t, SpawnOutcome>> deferred;
    std::optional<SpawnKey> acked;
    std::uint32_t retired = 0;
    std::map<std::uint32_t, bool> applied;
    // Semantic faults, injected after decoding (ADR-0105 §5).
    bool refuse_next_throw = false;  // the server's state disagrees: no charges left
    std::uint8_t end_at_age = kLife; // a server-only hit ends the flight early
    std::uint64_t defer_outcomes = 0;

    void emit(const SpawnOutcome& outcome) {
        if (defer_outcomes > 0)
            deferred.push_back({tick + defer_outcomes, outcome});
        else
            outcomes.push_back(outcome);
    }

    void end_flight() {
        for (auto& outcome : outcomes) {
            if (outcome.key == ball->key)
                outcome = {outcome.key, SpawnOutcomeKind::AcceptedEnded, {}, 0};
        }
        for (auto& [due, outcome] : deferred) {
            if (outcome.key == ball->key)
                outcome = {outcome.key, SpawnOutcomeKind::AcceptedEnded, {}, 0};
        }
        ball.reset();
        ++epoch;
    }

    void step(std::uint32_t sequence, const InputCommand* command) {
        ++tick;
        retired = sequence;
        applied[sequence] = command != nullptr;
        // A synthesized tick repeats nothing here: the toy holds no keys, and `pressed` is cleared.
        const InputCommand input = command != nullptr ? *command : neutral();
        if (cooldown > 0)
            --cooldown;
        player_x += input.move_x * static_cast<float>(kDt);
        if (ball) {
            if (ball->ball.age < kLife) {
                ++ball->ball.age;
                ball->ball.x += 1.0f;
            }
            if (ball->ball.age >= end_at_age)
                end_flight();
        }
        if ((input.pressed & kThrow) != 0) {
            const SpawnKey key{kInc, sequence, 0};
            if (refuse_next_throw) {
                charges = 0;
                refuse_next_throw = false;
            }
            if (charges > 0 && cooldown == 0 && !ball) {
                --charges;
                cooldown = kCooldown;
                ball = Live{{next_index++, 1}, key, {player_x, 0}};
                ++epoch;
                emit({key, SpawnOutcomeKind::Accepted, ball->id, 0});
            } else {
                emit({key, SpawnOutcomeKind::Rejected, {}, std::uint8_t(charges == 0 ? 1 : 2)});
            }
        }
        for (auto it = deferred.begin(); it != deferred.end();) {
            if (it->first <= tick) {
                outcomes.push_back(it->second);
                it = deferred.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] SpawnCheckpoint checkpoint() const {
        SpawnCheckpoint value{};
        value.base.server_tick = tick;
        value.base.membership_epoch = epoch;
        value.base.result.server_tick = tick;
        value.base.result.retired_through = retired;
        value.base.result.any_retired = retired > 0;
        for (const auto& [sequence, was_applied] : applied) {
            const auto offset = retired - sequence;
            if (was_applied && sequence <= retired && offset < 128)
                value.base.result.applied_mask[offset / 64] |= std::uint64_t{1} << (offset % 64);
        }
        value.base.entities.push_back({kPlayer, f32(player_x)});
        SpawnCheckpointExtension ext{};
        if (ball) {
            value.base.entities.push_back({ball->id, ball_bytes(ball->ball)});
            ext.bindings.push_back({ball->key, ball->id});
        }
        value.base.group_state = {std::byte{charges}, std::byte{cooldown}};
        ext.outcomes = outcomes;
        ext.acknowledged_through = acked;
        value.spawns = ext;
        return value;
    }

    void acknowledge(std::optional<SpawnKey> held) {
        if (!held || (acked && *held <= *acked))
            return;
        acked = held;
        std::erase_if(outcomes, [&](const auto& outcome) { return outcome.key <= *held; });
    }
};

struct Harness {
    ClientGame game;
    PresentationLedger ledger;
    SpawnReconciler reconciler;
    Server server;
    SpawnCheckpointDecoder decoder;
    std::uint32_t command_delay;
    std::uint32_t checkpoint_delay;
    bool acknowledge = true;
    std::set<std::uint32_t> lost_commands;    // every copy lost: the server synthesizes
    std::set<std::uint64_t> lost_checkpoints; // by server tick
    std::uint32_t frame = 0;
    std::vector<InputCommand> sent;
    std::deque<std::pair<std::uint32_t, SpawnCheckpoint>> in_flight;
    std::vector<SpawnReconcileOutcome> outcomes;
    // Presentation, checked every frame.
    bool id_keyed_presenter = false; // the deliberately broken de-duplication policy
    std::set<std::uint64_t> played_by_id;
    std::map<SpawnKey, int> launches;
    std::map<SpawnKey, std::size_t> max_live;
    std::map<SpawnKey, int> live_frames;

    Harness(std::uint32_t command_delay_frames,
            std::uint32_t checkpoint_delay_frames,
            SpawnBudget budget = {},
            ReconcilerBudget replay_budget = {})
        : reconciler(game.hooks(), game.spawn_hooks(), kDt, kInc, &ledger, replay_budget, budget),
          command_delay(command_delay_frames), checkpoint_delay(checkpoint_delay_frames) {
        game.reconciler = &reconciler;
        reconciler.set_membership(1, {kPlayer});
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    void step(InputCommand command) {
        ++frame;
        command.sequence = frame;
        while (!in_flight.empty() && in_flight.front().first <= frame) {
            const auto checkpoint = std::move(in_flight.front().second);
            in_flight.pop_front();
            outcomes.push_back(reconciler.apply_checkpoint(checkpoint.base, *checkpoint.spawns));
            for (const auto& binding : checkpoint.spawns->bindings) {
                if (game.balls.contains(pack(binding.id)))
                    game.key_of[pack(binding.id)] = binding.key;
            }
            if (acknowledge)
                server.acknowledge(reconciler.outcomes_held_through());
        }
        sent.push_back(command);
        reconciler.predict(command);
        if (frame > command_delay) {
            const auto sequence = frame - command_delay;
            server.step(sequence, lost_commands.contains(sequence) ? nullptr : &sent[sequence - 1]);
            if (!lost_checkpoints.contains(server.tick))
                in_flight.push_back({frame + checkpoint_delay, through_the_wire()});
        }
        present();
    }

    void run(std::size_t frames) {
        for (std::size_t i = 0; i < frames; ++i)
            step(neutral());
    }

    // Every checkpoint crosses the real codec, so the tests exercise the decoder as well.
    SpawnCheckpoint through_the_wire() {
        const auto value = server.checkpoint();
        std::array<std::byte, kCheckpointMaxMessageBytes> bytes{};
        const auto encoded = encode_spawn_checkpoint(value.base, *value.spawns, bytes);
        REQUIRE(encoded);
        SpawnCheckpoint decoded{};
        REQUIRE(decoder.decode(std::span{bytes}.first(encoded.bytes_written), decoded));
        REQUIRE(decoded.spawns);
        return decoded;
    }

    void present() {
        std::map<SpawnKey, std::size_t> live;
        for (const auto& [id, bytes] : game.balls) {
            const auto key = game.key_of.find(id);
            if (key == game.key_of.end() || bytes.empty() || ball_of(bytes).age >= kLife)
                continue;
            ++live[key->second];
            ++live_frames[key->second];
            const bool play =
                id_keyed_presenter
                    ? played_by_id.insert(id).second
                    : ledger.should_play(key->second, PresentationEventKind::Launch, 0);
            if (play)
                ++launches[key->second];
        }
        for (const auto& [key, count] : live)
            max_live[key] = std::max(max_live[key], count);
    }

    [[nodiscard]] bool cancelled(const PresentationEvent& event) const {
        return std::any_of(outcomes.begin(), outcomes.end(), [&](const auto& outcome) {
            return std::find(outcome.cancelled.begin(), outcome.cancelled.end(), event) !=
                   outcome.cancelled.end();
        });
    }
};

constexpr SpawnKey key_at(std::uint32_t sequence) {
    return {kInc, sequence, 0};
}

OwnerCheckpoint
client_checkpoint(std::uint64_t tick, std::uint32_t retired, std::uint8_t charges = 3) {
    OwnerCheckpoint checkpoint{};
    checkpoint.server_tick = tick;
    checkpoint.membership_epoch = 1;
    checkpoint.result.server_tick = tick;
    checkpoint.result.retired_through = retired;
    checkpoint.result.any_retired = retired != 0;
    checkpoint.entities.push_back({kPlayer, f32(0.0f)});
    checkpoint.group_state = {std::byte{charges}, std::byte{0}};
    return checkpoint;
}

} // namespace

TEST_CASE(
    "a predicted launch, then accepted: one presentation, identity adopted, epoch untouched") {
    Harness h(2, 2);
    h.step(neutral());
    h.step(throw_command());
    const auto key = key_at(2);
    const auto guesses = h.reconciler.provisional();
    REQUIRE(guesses.size() == 1);
    CHECK(guesses[0].key == key);
    CHECK((guesses[0].id.index & kProvisionalIndexBit) != 0);
    // The overlay is not membership: the guess changes neither the epoch nor the roster.
    CHECK(h.reconciler.epoch() == 1);
    CHECK(h.reconciler.roster().size() == 1);
    CHECK(h.launches[key] == 1);

    const auto provisional = guesses[0].id;
    while (h.reconciler.adopted() == 0 && h.frame < 20)
        h.step(neutral());
    REQUIRE(h.reconciler.adopted() == 1);
    const PredictedEntityId replica{100, 1};
    CHECK(h.reconciler.provisional().empty());
    CHECK(h.game.balls.contains(pack(replica)));
    CHECK_FALSE(h.game.balls.contains(pack(provisional)));
    CHECK(h.game.key_of.at(pack(replica)) == key);
    CHECK(h.ledger.bound(key) == replica);
    // The server's membership change (the ball joined its roster) is what moved the epoch.
    CHECK(h.reconciler.epoch() == 2);
    // Perfect prediction: adopting the guess needed no correction at all.
    CHECK(h.reconciler.base().corrections() == 0);

    h.run(20);
    CHECK(h.launches[key] == 1);
    CHECK(h.max_live[key] == 1);
    CHECK(h.game.balls.empty()); // the flight ended authoritatively; nothing lingers
    CHECK(h.reconciler.epoch() == h.server.epoch);
    CHECK(h.reconciler.rejected_launches() == 0);
    CHECK(h.reconciler.unpredicted_outcomes() == 0);
    CHECK(h.reconciler.contradictory_outcomes() == 0);
    CHECK(h.decoder.extended_decoded() > 0);
}

TEST_CASE("a rejected launch is removed, the ledger cancels it, and resources come back") {
    Harness h(2, 2);
    h.server.refuse_next_throw = true;
    h.step(neutral());
    h.step(throw_command());
    const auto key = key_at(2);
    CHECK(h.game.charges == 2); // predicted: one charge spent
    h.run(10);
    CHECK(h.reconciler.rejected_launches() == 1);
    CHECK(h.reconciler.provisional().empty());
    CHECK(h.game.balls.empty());
    CHECK(h.cancelled({key, PresentationEventKind::Launch, 0}));
    CHECK(h.ledger.cancelled(key));
    CHECK_FALSE(h.ledger.should_play(key, PresentationEventKind::Impact, 0));
    CHECK(h.launches[key] == 1);
    CHECK(h.max_live[key] == 1);
    // The server's state (no charges) is restored, not the client's guess.
    CHECK(h.game.charges == 0);
}

TEST_CASE("a duplicated confirmation is ignored and counted: one projectile, one effect") {
    Harness h(2, 2);
    h.acknowledge =
        false; // the server retains -- and so repeats -- the verdict in every checkpoint
    h.step(neutral());
    h.step(throw_command());
    h.run(10);
    const auto key = key_at(2);
    CHECK(h.reconciler.adopted() == 1);
    CHECK(h.reconciler.duplicate_outcomes_ignored() >= 3);
    CHECK(h.launches[key] == 1);
    CHECK(h.max_live[key] == 1);
    CHECK(h.ledger.duplicate_adoptions() == 0);

    // The same checkpoint delivered twice is stale on the second arrival and changes nothing.
    const auto copy = h.server.checkpoint();
    (void)h.reconciler.apply_checkpoint(copy.base, *copy.spawns);
    const auto again = h.reconciler.apply_checkpoint(copy.base, *copy.spawns);
    CHECK_FALSE(again.base.applied);
    CHECK(h.reconciler.stale_checkpoints() == 1);
}

TEST_CASE("confirmed after local expiry: the live replica binds, an ended one stays absent") {
    SpawnBudget budget{};
    budget.max_unresolved_sequences = 3;
    const auto key = key_at(2);

    SUBCASE("authoritative projectile still alive") {
        Harness h(3, 3, budget);
        h.step(neutral());
        h.step(throw_command());
        h.run(3);
        CHECK(h.reconciler.provisional_expired() == 1);
        CHECK(h.reconciler.local_releases() == 1);
        CHECK(h.game.balls.empty());
        h.run(10);
        CHECK(h.reconciler.confirmed_after_expiry_alive() == 1);
        // Replays of the launch command between expiry and retirement asked again; refused.
        CHECK(h.reconciler.respawn_suppressed() >= 1);
        CHECK(h.ledger.bound(key) == PredictedEntityId{100, 1});
        CHECK(h.launches[key] == 1); // the replica appeared; the launch did not restart
        CHECK(h.max_live[key] == 1);
    }
    SUBCASE("authoritative projectile already ended") {
        Harness h(3, 3, budget);
        h.server.end_at_age = 1;
        h.lost_checkpoints = {2}; // the only checkpoint showing the projectile alive is lost
        h.step(neutral());
        h.step(throw_command());
        h.run(15);
        CHECK(h.reconciler.provisional_expired() == 1);
        CHECK(h.reconciler.confirmed_after_expiry_ended() == 1);
        CHECK(h.reconciler.confirmed_after_expiry_alive() == 0);
        CHECK(h.game.balls.empty());
        CHECK(h.launches[key] == 1);
        CHECK(h.max_live[key] == 1);
    }
}

TEST_CASE("a launch command retired as synthesized is never recreated by replay") {
    Harness h(3, 3);
    h.lost_commands = {2};
    h.step(neutral());
    h.step(throw_command());
    const auto key = key_at(2);
    std::size_t frames_until_resolved = 0;
    while (h.reconciler.retired_without_launch() == 0 && frames_until_resolved++ < 20)
        h.step(neutral());
    REQUIRE(h.reconciler.retired_without_launch() == 1);
    // Before retirement, the replays did recreate the guess -- under the same id.
    CHECK(h.reconciler.recreated() >= 1);
    // Synthesized retirement removes an edge-triggered guess, but a late server verdict owns the
    // effect; the launch must remain playable until that verdict arrives.
    CHECK_FALSE(h.cancelled({key, PresentationEventKind::Launch, 0}));
    CHECK(h.reconciler.local_removals() == 1);
    CHECK(h.reconciler.authoritative_cancels() == 0);
    const auto simulated = h.game.launches_simulated;
    for (int i = 0; i < 12; ++i) {
        h.step(neutral());
        CHECK(h.reconciler.provisional().empty());
        CHECK(h.game.balls.empty());
    }
    CHECK(h.game.launches_simulated == simulated); // no replay ran the launch again
    CHECK(h.server.ball == std::nullopt);
    CHECK(h.game.charges == 3);
    CHECK(h.launches[key] == 1);
    CHECK(h.reconciler.local_releases() == 0); // the default bound has not elapsed
}

TEST_CASE("replay across ten checkpoints still plays the launch exactly once") {
    // Six frames each way: the launch command stays unretired through the eleven checkpoints
    // that left the server before it ran -- each one restores and replays the launch again.
    Harness h(6, 6);
    h.run(12);
    h.step(throw_command());
    const auto key = key_at(13);
    const auto first_id = h.reconciler.provisional_id(key);
    REQUIRE(first_id);
    for (int i = 0; i < 30; ++i) {
        h.step(neutral());
        CHECK(h.launches[key] == 1);
        CHECK(h.max_live[key] <= 1);
        // Reusing the id is part of the replay promise, even when presentation is keyed by key.
        if (h.reconciler.provisional_id(key))
            CHECK(h.reconciler.provisional_id(key) == first_id);
    }
    CHECK(h.reconciler.recreated() >= 10);
    CHECK(h.reconciler.adopted() == 1);
    // Presentation asked on every frame the ball was on screen; all but the first were refused.
    CHECK(h.live_frames[key] > 1);
    CHECK(h.ledger.duplicates_suppressed() == static_cast<std::uint64_t>(h.live_frames[key] - 1));
}

TEST_CASE("a retired launch whose verdict is late stays provisional, then resolves") {
    SUBCASE("verdict arrives within the bound") {
        Harness h(1, 1);
        h.server.refuse_next_throw = true;
        h.server.defer_outcomes = 4;
        h.step(neutral());
        h.step(throw_command());
        h.run(2);
        CHECK(std::any_of(h.outcomes.begin(), h.outcomes.end(), [](const auto& outcome) {
            return outcome.pending == 1;
        }));
        CHECK(h.reconciler.provisional().size() == 1);
        h.run(6);
        CHECK(h.reconciler.rejected_launches() == 1);
        CHECK(h.reconciler.provisional().empty());
        CHECK(h.game.balls.empty());
        CHECK(h.cancelled({key_at(2), PresentationEventKind::Launch, 0}));
    }
    SUBCASE("the unresolved bound expires first, the verdict still cancels") {
        SpawnBudget budget{};
        budget.max_unresolved_sequences = 3;
        Harness h(1, 1, budget);
        h.server.refuse_next_throw = true;
        h.server.defer_outcomes = 8;
        h.step(neutral());
        h.step(throw_command());
        h.run(14);
        CHECK(h.reconciler.provisional_expired() == 1);
        CHECK(h.reconciler.rejected_launches() == 1);
        CHECK(h.cancelled({key_at(2), PresentationEventKind::Launch, 0}));
        CHECK(h.game.balls.empty());
    }
}

TEST_CASE("a skipped replay keeps a live guess until the server accepts it") {
    // The tiny replay budget makes the inner reconciler restore without running the throw again.
    Harness h(4, 4, {}, ReconcilerBudget{128, 2});
    h.step(neutral());
    h.step(throw_command());
    const auto key = key_at(2);
    while (h.reconciler.base().replay_budget_exceeded() == 0 && h.frame < 20)
        h.step(neutral());
    REQUIRE(h.reconciler.base().replay_budget_exceeded() == 1);
    CHECK(h.reconciler.unreplayed_guesses() >= 1);
    CHECK_FALSE(h.cancelled({key, PresentationEventKind::Launch, 0}));
    CHECK_FALSE(h.ledger.cancelled(key));
    while (!h.ledger.bound(key) && h.frame < 25)
        h.step(neutral());
    CHECK(h.ledger.bound(key) == PredictedEntityId{100, 1});
    CHECK(h.reconciler.authoritative_cancels() == 0);
    CHECK(h.ledger.should_play(key, PresentationEventKind::Impact, 0));
    CHECK_FALSE(h.ledger.should_play(key, PresentationEventKind::Impact, 0));
}

TEST_CASE("a dropped guess waits for an authoritative verdict before cancelling presentation") {
    for (const bool accepted : {true, false}) {
        Harness h(30, 30);
        h.step(neutral());
        h.step(throw_command());
        const auto key = key_at(2);
        auto corrected = client_checkpoint(1, 1, 0);
        corrected.result.applied_mask[0] = 1;
        REQUIRE(h.reconciler.apply_checkpoint(corrected, {}).base.applied);
        CHECK(h.reconciler.not_recreated() == 1);
        CHECK(h.reconciler.local_removals() == 1);
        CHECK_FALSE(h.ledger.cancelled(key));

        auto verdict = client_checkpoint(2, 2, 2);
        verdict.result.applied_mask[0] = 3;
        SpawnCheckpointExtension ext{};
        if (accepted) {
            verdict.membership_epoch = 2;
            verdict.entities.push_back({{100, 1}, ball_bytes({0.0f, 1})});
            ext.bindings.push_back({key, {100, 1}});
            ext.outcomes.push_back({key, SpawnOutcomeKind::Accepted, {100, 1}, 0});
        } else {
            ext.outcomes.push_back({key, SpawnOutcomeKind::Rejected, {}, 1});
        }
        const auto answer = h.reconciler.apply_checkpoint(verdict, ext);
        REQUIRE(answer.base.applied);
        if (accepted) {
            CHECK(answer.cancelled.empty());
            CHECK(h.ledger.bound(key) == PredictedEntityId{100, 1});
            CHECK(h.ledger.should_play(key, PresentationEventKind::Impact, 0));
            CHECK_FALSE(h.ledger.should_play(key, PresentationEventKind::Impact, 0));
        } else {
            CHECK(answer.cancelled ==
                  std::vector<PresentationEvent>{{key, PresentationEventKind::Launch, 0}});
            CHECK(h.reconciler.authoritative_cancels() == 1);
        }
    }
}

TEST_CASE("a locally dropped guess releases presentation at the bound without a verdict") {
    SpawnBudget budget{};
    budget.max_unresolved_sequences = 3;
    Harness h(30, 30, budget);
    h.step(neutral());
    h.step(throw_command());
    auto corrected = client_checkpoint(1, 1, 0);
    corrected.result.applied_mask[0] = 1;
    REQUIRE(h.reconciler.apply_checkpoint(corrected, {}).base.applied);
    CHECK(h.reconciler.local_releases() == 0);
    h.run(3);
    CHECK(h.reconciler.local_releases() == 1);
    CHECK_FALSE(h.ledger.cancelled(key_at(2)));
    CHECK(h.ledger.should_play(key_at(2), PresentationEventKind::Impact, 0));
    CHECK_FALSE(h.ledger.should_play(key_at(2), PresentationEventKind::Impact, 0));
}

TEST_CASE("evicting a tombstone cannot recreate an older provisional id") {
    SpawnBudget budget{};
    budget.max_tombstones = 1;
    budget.max_unresolved_sequences = 3;
    Harness h(6, 6, budget);
    h.step(neutral());
    h.step(throw_command());
    h.run(4);
    h.step(throw_command()); // sequence 7; the first guess has expired locally
    h.run(24);
    CHECK(h.reconciler.tombstones_evicted() >= 1);
    CHECK(h.reconciler.respawn_suppressed() >= 1);
    CHECK(h.game.provisional_creates.size() == 2);
    CHECK(h.reconciler.provisional_expired() == 2);

    // A verdict can arrive after eviction; it still answers a launch this client predicted.
    Harness late(30, 30, budget);
    late.step(neutral());
    late.step(throw_command());
    late.run(4);
    late.step(throw_command());
    late.run(4);
    REQUIRE_FALSE(late.reconciler.has_tombstone(key_at(2)));
    auto cp = client_checkpoint(1, 7);
    SpawnCheckpointExtension ext{};
    ext.outcomes.push_back({key_at(2), SpawnOutcomeKind::Rejected, {}, 1});
    REQUIRE(late.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(late.reconciler.rejected_launches() == 1);
    CHECK(late.reconciler.rejected_unpredicted() == 0);
}

TEST_CASE("a server frontier is clamped to held outcomes and preserves newer tombstones") {
    SpawnBudget budget{};
    budget.max_unresolved_sequences = 3;
    Harness h(30, 30, budget);
    h.step(neutral());
    h.step(throw_command());
    h.run(4);
    h.step(throw_command()); // sequence 7
    h.run(4);
    REQUIRE(h.reconciler.has_tombstone(key_at(2)));
    REQUIRE(h.reconciler.has_tombstone(key_at(7)));
    const auto created = h.game.provisional_creates.size();

    // A held outcome means command 2 has retired; command 7 is still replayable.
    auto cp = client_checkpoint(1, 2);
    cp.result.applied_mask[0] = 3;
    SpawnCheckpointExtension ext{};
    ext.acknowledged_through = key_at(7);
    REQUIRE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(h.reconciler.frontier_ahead_of_client() == 1);
    CHECK(h.reconciler.has_tombstone(key_at(2)));
    CHECK(h.reconciler.has_tombstone(key_at(7)));

    cp.server_tick = cp.result.server_tick = 2;
    ext.acknowledged_through.reset();
    ext.outcomes.push_back({key_at(2), SpawnOutcomeKind::Rejected, {}, 1});
    REQUIRE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(h.reconciler.outcomes_held_through() == key_at(2));
    cp.server_tick = cp.result.server_tick = 3;
    ext.outcomes.clear();
    ext.acknowledged_through = key_at(2);
    REQUIRE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK_FALSE(h.reconciler.has_tombstone(key_at(2)));
    CHECK(h.reconciler.has_tombstone(key_at(7)));
    cp.server_tick = cp.result.server_tick = 4;
    REQUIRE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(h.game.provisional_creates.size() == created);
}

TEST_CASE("a retired applied guess without a snapshot expires without cancelling") {
    SpawnBudget budget{};
    budget.max_snapshots = 0;
    Harness h(1, 1, budget);
    h.server.refuse_next_throw = true;
    h.server.defer_outcomes = 8;
    h.step(neutral());
    h.step(throw_command());
    while (h.reconciler.pending_without_snapshot() == 0 && h.frame < 12)
        h.step(neutral());
    CHECK(h.reconciler.pending_without_snapshot() == 1);
    CHECK(h.reconciler.provisional().empty());
    CHECK(h.reconciler.has_tombstone(key_at(2)));
    CHECK_FALSE(h.ledger.cancelled(key_at(2)));
    h.run(12);
    CHECK(h.reconciler.rejected_launches() == 1);
    CHECK(h.cancelled({key_at(2), PresentationEventKind::Launch, 0}));
}

TEST_CASE("same-epoch roster changes refuse whole; an unpredicted rejection is separate") {
    Harness h(30, 30);
    h.step(neutral());
    auto cp = client_checkpoint(1, 1);
    cp.result.applied_mask[0] = 1;
    SpawnCheckpointExtension ext{};
    ext.outcomes.push_back({key_at(2), SpawnOutcomeKind::Rejected, {}, 1});
    REQUIRE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(h.reconciler.rejected_launches() == 0);
    CHECK(h.reconciler.rejected_unpredicted() == 1);
    const auto held = h.reconciler.outcomes_held_through();
    const auto cancelled = h.ledger.cancelled(key_at(2));
    const auto tombstone = h.reconciler.has_tombstone(key_at(2));
    cp.server_tick = cp.result.server_tick = 2;
    cp.entities.push_back({{100, 1}, ball_bytes({0.0f, 0})});
    ext.outcomes.clear();
    CHECK_FALSE(h.reconciler.apply_checkpoint(cp, ext).base.applied);
    CHECK(h.reconciler.epoch_roster_mismatch() == 1);
    CHECK(h.reconciler.roster().size() == 1);
    CHECK(h.reconciler.outcomes_held_through() == held);
    CHECK(h.ledger.cancelled(key_at(2)) == cancelled);
    CHECK(h.reconciler.has_tombstone(key_at(2)) == tombstone);
}

TEST_CASE("sensitivity control: the broken policies fail the gates the real ones pass") {
    const auto key = key_at(2);
    SUBCASE("adoption disabled -> two live presentations for one key") {
        Harness h(2, 2);
        h.game.ignore_adoption = true;
        h.step(neutral());
        h.step(throw_command());
        h.run(6);
        CHECK(h.reconciler.adopted() == 1);
        CHECK(h.max_live[key] == 2);
        CHECK_FALSE(h.max_live[key] <= 1);
    }
    SUBCASE("de-duplication keyed by entity id instead of the ledger -> the launch plays twice") {
        Harness h(2, 2);
        h.id_keyed_presenter = true;
        h.step(neutral());
        h.step(throw_command());
        h.run(6);
        CHECK(h.reconciler.adopted() == 1);
        CHECK(h.launches[key] == 2);
        CHECK_FALSE(h.launches[key] == 1);
    }
}

// ---------------------------------------------------------------------------------------------
// The codec
// ---------------------------------------------------------------------------------------------

namespace {

OwnerCheckpoint base_checkpoint() {
    OwnerCheckpoint value{};
    value.server_tick = 40;
    value.membership_epoch = 3;
    value.result = {40, 12, {0b11, 0}, true};
    value.entities.push_back({kPlayer, f32(2.0f)});
    value.entities.push_back({{100, 1}, ball_bytes({3.0f, 4})});
    value.group_state = {std::byte{2}, std::byte{0}};
    return value;
}

SpawnCheckpointExtension extension() {
    SpawnCheckpointExtension ext{};
    ext.bindings.push_back({key_at(10), {100, 1}});
    ext.outcomes.push_back({key_at(9), SpawnOutcomeKind::Rejected, {}, 1});
    ext.outcomes.push_back({key_at(10), SpawnOutcomeKind::Accepted, {100, 1}, 0});
    ext.outcomes.push_back({key_at(11), SpawnOutcomeKind::AcceptedEnded, {}, 0});
    ext.acknowledged_through = key_at(8);
    return ext;
}

std::vector<std::byte> base_wire(const OwnerCheckpoint& base) {
    std::vector<std::byte> bytes(kCheckpointMaxMessageBytes);
    const auto encoded = encode_owner_checkpoint(base, bytes);
    REQUIRE(encoded);
    bytes.resize(encoded.bytes_written);
    return bytes;
}

std::vector<std::byte> spawn_wire(const OwnerCheckpoint& base,
                                  const SpawnCheckpointExtension& ext) {
    std::vector<std::byte> bytes(kCheckpointMaxMessageBytes);
    const auto encoded = encode_spawn_checkpoint(base, ext, bytes);
    REQUIRE(encoded);
    bytes.resize(encoded.bytes_written);
    return bytes;
}

// Written from the layout comment in spawn_prediction.hpp, NOT from the encoder, and with no
// validation -- so it can put semantically malformed extensions on the wire for the decoder.
std::vector<std::byte> raw_wire(const OwnerCheckpoint& base, const SpawnCheckpointExtension& ext) {
    auto bytes = base_wire(base);
    const auto u8 = [&](std::uint8_t v) { bytes.push_back(std::byte{v}); };
    const auto u32 = [&](std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto key = [&](SpawnKey k) {
        u32(k.incarnation);
        u32(k.sequence);
        u8(k.slot);
    };
    const std::size_t body = 12 + 17 * ext.bindings.size() + 19 * ext.outcomes.size();
    u8(0xE5);
    u8(1);
    u8(static_cast<std::uint8_t>(body & 0xFF));
    u8(static_cast<std::uint8_t>(body >> 8));
    u8(static_cast<std::uint8_t>(ext.bindings.size()));
    u8(static_cast<std::uint8_t>(ext.outcomes.size()));
    u8(ext.acknowledged_through ? 1 : 0);
    key(ext.acknowledged_through.value_or(SpawnKey{}));
    for (const auto& binding : ext.bindings) {
        key(binding.key);
        u32(binding.id.index);
        u32(binding.id.generation);
    }
    for (const auto& outcome : ext.outcomes) {
        key(outcome.key);
        u8(static_cast<std::uint8_t>(outcome.kind));
        u32(outcome.id.index);
        u32(outcome.id.generation);
        u8(outcome.reason);
    }
    return bytes;
}

SpawnCheckpoint sentinel() {
    SpawnCheckpoint value{};
    value.base.server_tick = 999;
    value.spawns = SpawnCheckpointExtension{};
    value.spawns->bindings.push_back({key_at(77), {5, 5}});
    return value;
}

void untouched(const SpawnCheckpoint& value) {
    CHECK(value.base.server_tick == 999);
    REQUIRE(value.spawns);
    REQUIRE(value.spawns->bindings.size() == 1);
    CHECK(value.spawns->bindings[0].key == key_at(77));
}

void refused(SpawnCheckpointDecoder& decoder,
             std::span<const std::byte> bytes,
             SpawnExtensionRefusal reason) {
    auto out = sentinel();
    const auto before = decoder.refusals(reason);
    const auto result = decoder.decode(bytes, out);
    CHECK(result.base == CheckpointRefusal::None);
    CHECK(result.extension == reason);
    CHECK(decoder.refusals(reason) == before + 1);
    untouched(out);
}

void semantic(SpawnCheckpointDecoder& decoder,
              const OwnerCheckpoint& base,
              const SpawnCheckpointExtension& ext,
              SpawnExtensionRefusal reason) {
    CHECK(validate_spawn_extension(base, ext) == reason);
    std::array<std::byte, kCheckpointMaxMessageBytes> out{};
    out.fill(std::byte{0x5A});
    const auto encoded = encode_spawn_checkpoint(base, ext, out);
    CHECK(encoded.extension == reason);
    CHECK(encoded.bytes_written == 0);
    CHECK(std::all_of(out.begin(), out.end(), [](std::byte b) { return b == std::byte{0x5A}; }));
    refused(decoder, raw_wire(base, ext), reason);
}

} // namespace

TEST_CASE("spawn checkpoint extension round-trips, and pins its documented layout") {
    const auto base = base_checkpoint();
    const auto ext = extension();
    const auto bytes = spawn_wire(base, ext);
    const auto base_bytes = base_wire(base);
    CHECK(bytes.size() == base_bytes.size() + 16 + 17 * 1 + 19 * 3);
    CHECK(std::equal(base_bytes.begin(), base_bytes.end(), bytes.begin()));
    // The independently written raw layout is byte-identical to the encoder's.
    CHECK(raw_wire(base, ext) == bytes);
    SpawnCheckpoint out{};
    SpawnCheckpointDecoder decoder;
    REQUIRE(decoder.decode(bytes, out));
    CHECK(out.base.server_tick == 40);
    CHECK(out.base.entities.size() == 2);
    REQUIRE(out.spawns);
    CHECK(out.spawns->bindings == ext.bindings);
    CHECK(out.spawns->outcomes == ext.outcomes);
    CHECK(out.spawns->acknowledged_through == key_at(8));
    CHECK(decoder.extended_decoded() == 1);
    CHECK(kSpawnExtensionMaxBytes == 592);
}

TEST_CASE("an m20.4-only checkpoint still decodes; an m20.4 decoder refuses the extension") {
    const auto base = base_checkpoint();
    SpawnCheckpoint out = sentinel();
    SpawnCheckpointDecoder decoder;
    REQUIRE(decoder.decode(base_wire(base), out));
    CHECK(out.base.server_tick == 40);
    CHECK_FALSE(out.spawns);
    CHECK(decoder.legacy_decoded() == 1);
    // Why the extension is NEGOTIATED: m20.4's decoder treats it as trailing garbage, whole.
    OwnerCheckpoint legacy{};
    CHECK(decode_owner_checkpoint(spawn_wire(base, extension()), legacy) ==
          CheckpointRefusal::BadLength);
}

TEST_CASE("every spawn extension refusal refuses the whole message and is counted") {
    const auto base = base_checkpoint();
    const auto good = spawn_wire(base, extension());
    const auto at = base_wire(base).size(); // where the extension starts
    SpawnCheckpointDecoder decoder;

    auto bad = good;
    bad.pop_back();
    refused(decoder, bad, SpawnExtensionRefusal::Truncated);
    refused(decoder, std::span{good}.first(at + 2), SpawnExtensionRefusal::Truncated);
    bad = good;
    bad.push_back(std::byte{0});
    refused(decoder, bad, SpawnExtensionRefusal::BadLength);
    bad = good;
    bad[at] = std::byte{0xE4};
    refused(decoder, bad, SpawnExtensionRefusal::BadTag);
    bad = good;
    bad[at + 1] = std::byte{2};
    refused(decoder, bad, SpawnExtensionRefusal::UnknownVersion);
    bad = good;
    bad[at + 4] = std::byte{17};
    refused(decoder, bad, SpawnExtensionRefusal::TooManyBindings);
    bad = good;
    bad[at + 5] = std::byte{17};
    refused(decoder, bad, SpawnExtensionRefusal::TooManyOutcomes);
    bad = good;
    bad[at + 6] = std::byte{2};
    refused(decoder, bad, SpawnExtensionRefusal::BadFrontierFlag);
    bad = good;
    bad[at + 4] = std::byte{0}; // counts disagree with body_bytes
    refused(decoder, bad, SpawnExtensionRefusal::BadLength);
    bad = good;
    bad[at + 6] = std::byte{0}; // "no frontier", but frontier bytes are set
    refused(decoder, bad, SpawnExtensionRefusal::ReservedNonZero);
    const auto first_outcome = at + 16 + 17;
    bad = good;
    bad[first_outcome + 9] = std::byte{4};
    refused(decoder, bad, SpawnExtensionRefusal::BadOutcomeKind);
    bad = good;
    bad[first_outcome + 9] = std::byte{0};
    refused(decoder, bad, SpawnExtensionRefusal::BadOutcomeKind);

    // Semantic refusals: validate, encode and decode all agree.
    auto ext = extension();
    ext.outcomes[0].id = {3, 1}; // a Rejected verdict naming an entity
    semantic(decoder, base, ext, SpawnExtensionRefusal::ReservedNonZero);
    ext = extension();
    ext.outcomes[1].reason = 4; // an Accepted verdict with a reason
    semantic(decoder, base, ext, SpawnExtensionRefusal::ReservedNonZero);
    ext = extension();
    ext.outcomes[2].reason = 1; // an AcceptedEnded verdict with a reason
    semantic(decoder, base, ext, SpawnExtensionRefusal::ReservedNonZero);
    ext = extension();
    ext.bindings.push_back({key_at(10), {1, 1}}); // the same key bound twice
    semantic(decoder, base, ext, SpawnExtensionRefusal::DuplicateBinding);
    ext = extension();
    ext.bindings.push_back({key_at(13), {100, 1}}); // one entity bound to two keys
    semantic(decoder, base, ext, SpawnExtensionRefusal::DuplicateBinding);
    ext = extension();
    ext.bindings[0].id = {101, 1};
    ext.outcomes[1].id = {101, 1};
    semantic(decoder, base, ext, SpawnExtensionRefusal::BindingNotInRoster);
    auto provisional_base = base;
    provisional_base.entities[1].id = {kProvisionalIndexBit | 4, 1};
    semantic(decoder, provisional_base, {}, SpawnExtensionRefusal::ProvisionalIdInRoster);
    ext = extension();
    std::swap(ext.outcomes[0], ext.outcomes[2]);
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomesNotAscending);
    ext = extension();
    ext.outcomes.insert(ext.outcomes.begin(), ext.outcomes[0]); // a duplicate within one message
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomesNotAscending);
    ext = extension();
    ext.acknowledged_through = key_at(9);
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomeAtOrBelowFrontier);
    ext = extension();
    ext.outcomes[1].id = {1, 1}; // Accepted, but the binding names another entity
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomeBindingMismatch);
    ext = extension();
    ext.bindings.clear(); // Accepted with no binding at all
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomeBindingMismatch);
    ext = extension();
    ext.bindings.push_back({key_at(9), {1, 1}}); // a REJECTED key bound to a roster entity
    semantic(decoder, base, ext, SpawnExtensionRefusal::OutcomeBindingMismatch);
    ext = extension();
    ext.outcomes.resize(17, ext.outcomes.back());
    CHECK(validate_spawn_extension(base, ext) == SpawnExtensionRefusal::TooManyOutcomes);
    ext = extension();
    ext.bindings.resize(17, ext.bindings.back());
    CHECK(validate_spawn_extension(base, ext) == SpawnExtensionRefusal::TooManyBindings);

    // The 1200-byte bound covers base AND extension.
    auto big = base;
    big.group_state.resize(kCheckpointMaxGroupBytes);
    big.entities[0].state.resize(kCheckpointMaxStateBytes);
    ext = {};
    for (std::uint32_t i = 0; i < 16; ++i)
        ext.outcomes.push_back({key_at(20 + i), SpawnOutcomeKind::Rejected, {}, 1});
    std::array<std::byte, 2 * kCheckpointMaxMessageBytes> wide{};
    CHECK(encode_spawn_checkpoint(big, ext, wide).extension ==
          SpawnExtensionRefusal::MessageTooLarge);
    auto oversized = raw_wire(big, ext);
    REQUIRE(oversized.size() > kCheckpointMaxMessageBytes);
    auto out = sentinel();
    CHECK(decoder.decode(oversized, out).base == CheckpointRefusal::MessageTooLarge);
    CHECK(decoder.base_refusals() == 1);
    untouched(out);
    // Too small an output buffer.
    std::array<std::byte, 64> small{};
    CHECK(encode_spawn_checkpoint(base, extension(), small).extension ==
          SpawnExtensionRefusal::Truncated);
    // A base refusal is reported as one, not as an extension refusal.
    auto mismatched = base;
    mismatched.result.server_tick = 41;
    CHECK(encode_spawn_checkpoint(mismatched, extension(), wide).base ==
          CheckpointRefusal::TickMismatch);
}

// ---------------------------------------------------------------------------------------------
// The ledger and the reconciler's own refusals
// ---------------------------------------------------------------------------------------------

TEST_CASE("presentation ledger: exactly once, cancel, release, and every bound counted") {
    PresentationLedger ledger({2, 2, 1});
    const auto a = key_at(1);
    const auto b = key_at(2);
    const auto c = key_at(3);
    CHECK(ledger.should_play(a, PresentationEventKind::Launch, 0));
    CHECK_FALSE(ledger.should_play(a, PresentationEventKind::Launch, 0));
    CHECK(ledger.should_play(a, PresentationEventKind::Trail, 0));
    CHECK_FALSE(ledger.should_play(a, PresentationEventKind::Impact, 0)); // 2 events per key
    CHECK(ledger.event_overflow() == 1);
    CHECK(ledger.duplicates_suppressed() == 1);
    CHECK(ledger.adopt(a, {100, 1}));
    CHECK_FALSE(ledger.adopt(a, {100, 1}));
    CHECK(ledger.duplicate_adoptions() == 1);

    CHECK(ledger.should_play(b, PresentationEventKind::Launch, 0));
    CHECK_FALSE(ledger.should_play(c, PresentationEventKind::Launch, 0)); // 2 live keys
    CHECK(ledger.key_overflow() == 1);

    const auto cancelled = ledger.cancel(b);
    CHECK(cancelled == std::vector<PresentationEvent>{{b, PresentationEventKind::Launch, 0}});
    CHECK(ledger.cancel(b).empty());
    CHECK(ledger.duplicate_cancels() == 1);
    // Cancellation has already returned the launch for removal; adoption cannot revive that key.
    CHECK_FALSE(ledger.adopt(b, {101, 1}));
    CHECK(ledger.adopt_after_cancel() == 1);
    CHECK_FALSE(ledger.bound(b));
    CHECK_FALSE(ledger.should_play(b, PresentationEventKind::Impact, 0));
    CHECK(ledger.suppressed_cancelled() == 1);

    // Released: played events stay played; a new triple still plays once.
    ledger.release(a);
    CHECK_FALSE(ledger.should_play(a, PresentationEventKind::Launch, 0));
    CHECK(ledger.bound(a) == PredictedEntityId{100, 1});
    // One tombstone only: releasing `a` evicted `b`'s, which is counted.
    CHECK(ledger.tombstones_evicted() == 1);
    CHECK_FALSE(ledger.cancelled(b));
    CHECK(ledger.should_play(c, PresentationEventKind::Launch, 0)); // capacity came back
}

TEST_CASE("spawn_provisional refuses calls it could not honour, each counted") {
    struct Tiny {
        std::map<std::uint64_t, std::vector<std::byte>> entities;
        SpawnReconciler* reconciler = nullptr;
        std::vector<std::optional<PredictedEntityId>> results;
    } tiny;

    PredictionHooks hooks{
        [&](PredictedEntityId id) {
            return id == kPlayer ? std::vector<std::byte>{} : tiny.entities.at(pack(id));
        },
        [&](PredictedEntityId id, std::span<const std::byte> bytes) {
            if (!(id == kPlayer))
                tiny.entities[pack(id)].assign(bytes.begin(), bytes.end());
        },
        [] { return std::vector<std::byte>{}; },
        [](std::span<const std::byte>) {},
        [&](std::span<const PredictedEntityId>, const InputCommand& input, const PredictionStep&) {
            const std::byte one{1};
            auto& r = *tiny.reconciler;
            tiny.results.push_back(r.spawn_provisional({kInc, input.sequence, 0}, {&one, 1}));
            tiny.results.push_back(r.spawn_provisional({kInc, input.sequence, 0}, {&one, 1}));
            tiny.results.push_back(r.spawn_provisional({kInc, input.sequence, 1}, {&one, 1}));
            tiny.results.push_back(r.spawn_provisional({kInc, input.sequence + 1, 2}, {&one, 1}));
            tiny.results.push_back(r.spawn_provisional({kInc + 1, input.sequence, 3}, {&one, 1}));
        }};
    SpawnHooks spawn_hooks{[&](PredictedEntityId id) { tiny.entities[pack(id)] = {}; },
                           [&](PredictedEntityId id) { tiny.entities.erase(pack(id)); },
                           [](PredictedEntityId, PredictedEntityId) {}};
    SpawnBudget budget{};
    budget.max_provisional = 1;
    SpawnReconciler reconciler(hooks, spawn_hooks, kDt, kInc, nullptr, {}, budget);
    tiny.reconciler = &reconciler;
    reconciler.set_membership(1, {kPlayer});

    const std::byte one{1};
    CHECK_FALSE(reconciler.spawn_provisional(key_at(1), {&one, 1}));
    CHECK(reconciler.spawn_outside_step() == 1);

    InputCommand command{};
    command.sequence = 1;
    reconciler.predict(command);
    REQUIRE(tiny.results.size() == 5);
    CHECK(tiny.results[0].has_value());
    CHECK_FALSE(tiny.results[1]); // the same key twice in one step
    CHECK_FALSE(tiny.results[2]); // over max_provisional
    CHECK_FALSE(tiny.results[3]); // a key naming another command
    CHECK_FALSE(tiny.results[4]); // a key from another incarnation
    CHECK(reconciler.spawn_duplicate() == 1);
    CHECK(reconciler.provisional_overflow() == 1);
    CHECK(reconciler.spawn_bad_key() == 2);
    CHECK(reconciler.provisional().size() == 1);
}

TEST_CASE("a refused spawn checkpoint changes nothing") {
    Harness h(2, 2);
    h.step(neutral());
    h.step(throw_command());
    const auto balls = h.game.balls;
    const auto epoch = h.reconciler.epoch();

    auto good = h.server.checkpoint();
    good.base.server_tick = 50;
    good.base.result.server_tick = 50;
    auto bad = good;
    bad.base.result.server_tick = 51;
    bad.spawns->outcomes.push_back({key_at(20), SpawnOutcomeKind::Rejected, {}, 1});
    CHECK_FALSE(h.reconciler.apply_checkpoint(bad.base, *bad.spawns).base.applied);
    CHECK(h.reconciler.invalid_base() == 1);
    CHECK_FALSE(h.reconciler.outcomes_held_through());
    bad = good;
    bad.spawns->bindings.push_back({key_at(1), {555, 1}});
    bad.spawns->outcomes.push_back({key_at(20), SpawnOutcomeKind::Rejected, {}, 1});
    CHECK_FALSE(h.reconciler.apply_checkpoint(bad.base, *bad.spawns).base.applied);
    CHECK(h.reconciler.invalid_extension(SpawnExtensionRefusal::BindingNotInRoster) == 1);
    CHECK(h.game.balls == balls);
    CHECK(h.reconciler.epoch() == epoch);
    CHECK(h.reconciler.provisional().size() == 1);
    CHECK_FALSE(h.reconciler.outcomes_held_through());
    CHECK_FALSE(h.ledger.cancelled(key_at(2)));
    CHECK_FALSE(h.reconciler.has_tombstone(key_at(2)));

    // A stale message carrying a newer verdict must not acknowledge or cancel it either.
    good.spawns->outcomes.push_back({key_at(20), SpawnOutcomeKind::Rejected, {}, 1});
    REQUIRE(h.reconciler.apply_checkpoint(good.base, *good.spawns).base.applied);
    const auto held = h.reconciler.outcomes_held_through();
    const auto tombstone = h.reconciler.has_tombstone(key_at(20));
    good.spawns->outcomes.clear();
    good.spawns->outcomes.push_back({key_at(21), SpawnOutcomeKind::Rejected, {}, 1});
    CHECK_FALSE(h.reconciler.apply_checkpoint(good.base, *good.spawns).base.applied);
    CHECK(h.reconciler.outcomes_held_through() == held);
    CHECK(h.reconciler.has_tombstone(key_at(20)) == tombstone);
    CHECK_FALSE(h.ledger.cancelled(key_at(21)));
}

TEST_CASE("the outcome acknowledgement round-trips exactly and refuses every malformed length") {
    const SpawnOutcomeAck ack{SpawnKey{0xA1B2C3D4u, 0x01020304u, 5}};
    std::array<std::byte, kSpawnOutcomeAckBytes + 4> bytes{};
    REQUIRE(encode_spawn_outcome_ack(ack, bytes) == kSpawnOutcomeAckBytes);
    CHECK(kSpawnOutcomeAckBytes == 10);
    CHECK(bytes[0] == std::byte{kSpawnOutcomeAckTag});
    // Little-endian, the layout the header documents: incarnation, sequence, slot.
    CHECK(bytes[1] == std::byte{0xD4});
    CHECK(bytes[4] == std::byte{0xA1});
    CHECK(bytes[5] == std::byte{0x04});
    CHECK(bytes[9] == std::byte{5});

    SpawnOutcomeAck decoded{};
    REQUIRE(decode_spawn_outcome_ack(std::span(bytes).first(kSpawnOutcomeAckBytes), decoded));
    CHECK(decoded == ack);

    // Too small an output buffer writes nothing.
    std::array<std::byte, kSpawnOutcomeAckBytes - 1> small{};
    CHECK(encode_spawn_outcome_ack(ack, small) == 0);
    CHECK(std::all_of(small.begin(), small.end(), [](std::byte b) { return b == std::byte{0}; }));

    // Every refusal leaves `out` as it was.
    const SpawnOutcomeAck sentinel{SpawnKey{9, 9, 9}};
    for (std::size_t length :
         {std::size_t{0}, std::size_t{1}, kSpawnOutcomeAckBytes - 1, kSpawnOutcomeAckBytes + 1}) {
        SpawnOutcomeAck out = sentinel;
        CHECK_FALSE(decode_spawn_outcome_ack(std::span(bytes).first(length), out));
        CHECK(out == sentinel);
    }
    auto wrong_tag = bytes;
    wrong_tag[0] = std::byte{kSpawnExtensionTag};
    SpawnOutcomeAck out = sentinel;
    CHECK_FALSE(decode_spawn_outcome_ack(std::span(wrong_tag).first(kSpawnOutcomeAckBytes), out));
    CHECK(out == sentinel);
}
