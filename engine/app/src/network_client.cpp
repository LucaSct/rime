// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Connect, exchange InputHello, and wait for ownership plus a complete initial world. At each
// paced tick pump the driver, apply the authoritative world to a mirror, reconcile checkpoints,
// sample input, predict, and send the redundant input window and a replication ack. The mirror
// and prediction world stay separate (ADR-0104 §4): replication owns truth for every entity,
// while checkpoint restore and replay may write only the local predicted set.

#include "rime/app/network_client.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/app/network_input.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/interaction.hpp"
#include "rime/replication/interpolation.hpp"
#include "rime/rhi/device.hpp"

namespace rime::app {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

class ClientLink final : public net::Link {
public:
    explicit ClientLink(platform::UdpSocket socket) : socket_(std::move(socket)) {}

    bool send(const platform::Endpoint& to, std::span<const std::byte> bytes) override {
        return socket_.send_to(to, bytes).has_value();
    }

    std::size_t receive(std::vector<net::Datagram>& out) override {
        std::array<std::byte, 65536> buffer{};
        std::size_t seen = 0;
        while (seen < 64) {
            platform::Endpoint from{};
            const auto size = socket_.recv_from(from, buffer);
            if (!size)
                break;
            ++seen;
            // Read the whole datagram: truncating an oversized state packet into a plausible
            // transport packet would hide the refusal this counter is meant to prove.
            if (*size > 1200) {
                ++oversized_;
                continue;
            }
            if (*size < 8) {
                ++malformed_;
                continue;
            }
            out.push_back({from, {buffer.begin(), buffer.begin() + *size}});
        }
        return seen;
    }

    [[nodiscard]] std::uint64_t oversized() const { return oversized_; }

    [[nodiscard]] std::uint64_t malformed() const { return malformed_; }

private:
    platform::UdpSocket socket_;
    std::uint64_t oversized_ = 0, malformed_ = 0;
};

class ImpairedLink final : public net::Link {
public:
    ImpairedLink(net::Link& inner, const LaunchOptions& options)
        : inner_(inner), delay_(options.net_delay_ms), jitter_(options.net_jitter_ms),
          loss_(options.net_loss), blackout_ms_(options.net_blackout_ms),
          blackout_tick_(options.net_blackout_tick), rng_(options.net_seed ? options.net_seed : 1) {
    }

    void client_tick(std::uint64_t tick) {
        if (blackout_ms_ && tick == blackout_tick_) {
            blackout_until_ = now_ms() + blackout_ms_;
            outbound_.clear();
            inbound_.clear();
        }
    }

    bool send(const platform::Endpoint& to, std::span<const std::byte> bytes) override {
        if (blacked_out() || drop())
            return true;
        outbound_.push_back(
            {now_ms() + delay_ + random_jitter(), to, {bytes.begin(), bytes.end()}});
        return true;
    }

    std::size_t receive(std::vector<net::Datagram>& out) override {
        const auto now = now_ms();
        while (!blacked_out() && deliverable(outbound_, now)) {
            auto packet = take_due(outbound_);
            (void)inner_.send(packet.peer, packet.bytes);
        }
        std::vector<net::Datagram> received;
        inner_.receive(received);
        for (auto& packet : received)
            if (!blacked_out() && !drop())
                inbound_.push_back(
                    {now + delay_ + random_jitter(), packet.from, std::move(packet.bytes)});
        const auto before = out.size();
        while (!blacked_out() && deliverable(inbound_, now)) {
            auto packet = take_due(inbound_);
            out.push_back({packet.peer, std::move(packet.bytes)});
        }
        return out.size() - before;
    }

private:
    struct Pending {
        std::uint64_t due;
        platform::Endpoint peer;
        std::vector<std::byte> bytes;
    };

    [[nodiscard]] bool blacked_out() const { return now_ms() < blackout_until_; }

    std::uint64_t sample() {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return rng_ * 0x2545F4914F6CDD1Dull;
    }

    bool drop() { return static_cast<double>(sample() >> 11) * (1.0 / 9007199254740992.0) < loss_; }

    std::uint32_t random_jitter() {
        return jitter_ ? static_cast<std::uint32_t>(sample() % (jitter_ + 1ull)) : 0;
    }

    static bool deliverable(const std::vector<Pending>& queue, std::uint64_t now) {
        return std::any_of(
            queue.begin(), queue.end(), [now](const Pending& packet) { return packet.due <= now; });
    }

    static Pending take_due(std::vector<Pending>& queue) {
        const auto first =
            std::min_element(queue.begin(), queue.end(), [](const Pending& a, const Pending& b) {
                return a.due < b.due;
            });
        Pending packet = std::move(*first);
        queue.erase(first);
        return packet;
    }

    net::Link& inner_;
    std::uint32_t delay_;
    std::uint32_t jitter_;
    double loss_;
    std::uint32_t blackout_ms_;
    std::uint64_t blackout_tick_;
    std::uint64_t blackout_until_ = 0;
    std::uint64_t rng_;
    std::vector<Pending> outbound_, inbound_;
};

RunReport fail(RunReport report, std::string_view reason) {
    report.status = RunStatus::Failed;
    report.message = std::string(reason);
    fmt::print("{{\"event\":\"connect_failed\",\"reason\":\"{}\"}}\n", reason);
    std::fflush(stdout);
    return report;
}

} // namespace

RunReport run_network_client(const LaunchOptions& options,
                             const GameDefinition& definition,
                             const RunHooks& hooks) {
    RunReport report{};
    report.mode = LaunchMode::Play;
    const auto connect_deadline = Clock::now() + std::chrono::seconds(5);
    if (!definition.network_input || !definition.remote_play || !definition.create ||
        !definition.remote_play->register_replicated || !definition.remote_play->prediction_hooks ||
        !definition.remote_play->to_command || !definition.remote_play->seed_prediction ||
        !definition.remote_play->digest || !definition.remote_play->player_pose ||
        definition.remote_play->fixed_dt <= 0 ||
        definition.remote_play->fixed_dt != 1.0 / definition.tick_hz)
        return fail(report, "invalid remote play profile or fixed timestep");

    const auto endpoint =
        platform::Endpoint::from_string(options.connect_host, options.connect_port);
    const auto bind_host = options.connect_host == "127.0.0.1" ? "127.0.0.1" : "0.0.0.0";
    auto udp = platform::UdpSocket::bind(options.client_port, bind_host, false);
    if (!endpoint || !udp)
        return fail(report, "UDP endpoint unavailable");
    ClientLink socket_link{std::move(*udp)};
    std::unique_ptr<ImpairedLink> impaired;
    net::Link* link = &socket_link;
    if (options.net_impair) {
        impaired = std::make_unique<ImpairedLink>(*link, options);
        link = impaired.get();
    }
    net::NetDriver driver{*link, net::NetDriver::Config{}};
    const auto session_id = driver.connect(*endpoint, now_ms());
    if (!session_id)
        return fail(report, "transport session unavailable");

    AppConfig config{};
    config.tick_hz = definition.tick_hz;
    config.worker_threads = options.workers;
    config.gpu = !options.no_gpu;
    config.windowed = !options.no_gpu && !options.headless;
    config.render_extent = kPlayRenderExtent;
    config.window_size = kPlayRenderExtent;
    config.window_title = definition.name;
    config.device_factory = [&] {
        ++report.device_factory_calls;
        return hooks.device_factory ? hooks.device_factory() : rhi::create_device({});
    };
    Application app(config);
    report.had_device = app.device() != nullptr;
    report.windowed = app.windowed();
    auto& mirror_world = app.world();
    definition.remote_play->register_replicated(mirror_world);
    replication::ClientReplicator mirror{mirror_world};
    ecs::World prediction_world;
    definition.remote_play->register_replicated(prediction_world);
    auto game = definition.create();
    if (!game || (options.autopilot && !game->has_autopilot()) ||
        (options.input_tape && !definition.remote_play->diagnostic_input_tape) ||
        (options.input_tape_throw &&
         (!definition.remote_play->spawn_prediction ||
          !definition.remote_play->spawn_prediction->diagnostic_throw_input)) ||
        (options.input_tape_push &&
         (!definition.remote_play->active_interaction() ||
          !definition.remote_play->active_interaction()->diagnostic_push_input)))
        return fail(report, "game or autopilot unavailable");

    replication::InputWindow window;
    std::unique_ptr<replication::Reconciler> reconciler;
    // Presentation is deliberately outside the rewindable prediction world: replay must not
    // forget that a Launch has already been shown under its provisional key.
    replication::PresentationLedger ledger;
    std::unique_ptr<replication::SpawnReconciler> spawn_reconciler;
    std::optional<GameDefinition::RemotePlayProfile::SpawnPredictionProfile::ClientHooks>
        spawn_hooks;
    std::vector<replication::SpawnBinding> spawn_bindings;
    std::vector<replication::ProvisionalEntity> detached_guesses;
    std::unordered_set<replication::SpawnKey, replication::SpawnKeyHash> live_presentations;
    std::unordered_set<replication::SpawnKey, replication::SpawnKeyHash> launched_presentations;
    std::unordered_set<replication::SpawnKey, replication::SpawnKeyHash> ended_keys;
    std::unordered_set<replication::SpawnKey, replication::SpawnKeyHash> suspended_keys;
    std::optional<replication::SpawnKey> acknowledged_outcomes;
    std::uint64_t presentation_launch = 0, presentation_end = 0;
    std::uint64_t presentation_cancelled = 0, presentation_suspended = 0;
    // Keys whose end was the entity's own life limit rather than the server's: the miss/hit
    // evidence (a hit ends the server's flight early, so its end arrives as an authoritative one).
    std::uint64_t lifetime_ends = 0, presentation_resumed = 0;
    std::uint64_t live_presentations_max = 0, spawn_response_ticks = 0;
    std::optional<std::uint64_t> spawn_press_tick;
    bool spawn_responded = false;
    std::uint64_t spawn_acks_sent = 0, spawn_ack_send_refused = 0;
    // Checkpoints the spawn path could not use: an m20.4-shaped one (no extension, so no verdicts
    // to apply) or one whose roster lacks the mirror's player. Counted, because a client that
    // silently dropped every checkpoint would still "converge" on its own prediction (guardrail 5).
    std::uint64_t spawn_checkpoints_without_extension = 0, spawn_checkpoints_without_player = 0;
    // m20.6b, ADR-0106 §3-§4: the interaction object. The game's decorated hooks own its
    // prediction physics world; the engine owns what is OUTSIDE rewindable state -- the smoother
    // and every counter -- so no restore or replay can rewrite what the player was shown.
    //
    // CORRECTED SIMULATION, SMOOTHED PRESENTATION: a checkpoint always moves the simulated object
    // to the authoritative answer and replays (the hooks never blend), and the smoother records
    // the jump as a visual offset that decays, so the object on screen glides over it instead.
    const auto* interaction = definition.remote_play->active_interaction();
    std::optional<GameDefinition::RemotePlayProfile::InteractionProfile::ClientHooks>
        interaction_hooks;
    replication::PredictedEntityId interaction_id{};
    ecs::Entity mirror_object = ecs::kNullEntity;
    replication::CorrectionSmoother smoother;
    // Membership as the CLIENT saw it (applied rosters), beside the server's own counters.
    std::uint64_t crate_joins_seen = 0, crate_leaves_seen = 0, crate_member_ticks = 0;
    // The simulation's own correction: the predicted object before a checkpoint against the same
    // tick replayed from it, for checkpoints where it was a member on both sides (a join is a
    // seed, not a correction). With no server-only force this is the UNDISTURBED RESIDUAL that
    // ADR-0106 §3 expects to be non-zero: the rebuilt world has no warm-start cache.
    double crate_max_sim_correction = 0.0;
    std::uint64_t crate_member_checkpoints = 0, crate_corrections_over_1cm = 0;
    // `--interaction-trace`: one (server tick, simulated crate correction, the predicted PLAYER's
    // correction at the same checkpoint, both in metres) triple per member->member checkpoint. The
    // player's is what lets a proof tell a crate that disagrees because its pusher did (a lost or
    // late command: the server's player stood still while the prediction walked) from a crate that
    // disagrees while the player agrees -- hidden physics state. So a proof can tell the
    // undisturbed residual (before a kick) from the disturbance and its convergence (after) instead
    // of reading one maximum over the run.
    std::vector<std::array<double, 3>> crate_trace;
    // Push response: the first predicted step in which the player touched the object, and the
    // presented object's position just before that step.
    std::optional<std::uint64_t> crate_push_tick;
    core::Vec3 crate_push_pose{};
    std::uint64_t crate_response_ticks = 0;
    bool crate_responded = false;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;
    std::vector<replication::SpawnCheckpoint> pending;
    std::uint32_t epoch = 0;
    bool has_epoch = false, accepted = false, disconnected = false, graceful = false;
    std::string failure_reason;
    std::uint64_t checkpoints = 0, replayed = 0, max_replay = 0;
    std::uint64_t response_ticks = 0, snaps_over_bound = 0;
    double max_snap = 0.0;
    std::optional<std::uint64_t> first_input_tick;
    std::optional<core::Vec3> first_input_position;
    bool responded = false;
    std::uint32_t sequence = 0;
    ecs::Entity mirror_player = ecs::kNullEntity;
    ecs::Entity predicted_player = ecs::kNullEntity;
    const auto presented_position = [&] {
        const bool predicted = !options.no_prediction;
        return definition.remote_play
            ->player_pose(predicted ? prediction_world : mirror_world,
                          predicted ? predicted_player : mirror_player)
            .translation;
    };
    const auto record_snap = [&](core::Vec3 before) {
        const auto distance = static_cast<double>(core::length(presented_position() - before));
        max_snap = std::max(max_snap, distance);
        if (options.has_snap_bound && distance > options.snap_bound)
            ++snaps_over_bound;
    };

    const auto object_member = [&] {
        if (!spawn_reconciler || !interaction_hooks)
            return false;
        const auto roster = spawn_reconciler->roster();
        return std::find(roster.begin(), roster.end(), interaction_id) != roster.end();
    };
    // What the player sees of the object: the predicted simulation plus the smoother's offset
    // while it is a predicted member, otherwise (or with --no-prediction) the mirror's authority.
    const auto presented_object = [&] {
        // `present-mirror` control: keep predicting, but SHOW the round-trip-old mirror.
        if (!options.no_prediction && interaction_hooks &&
            options.interaction_debug_break != InteractionDebugBreak::PresentMirror)
            if (const auto simulated = interaction_hooks->object_pose()) {
                core::Transform pose = *simulated;
                pose.translation = smoother.presented(interaction_id, simulated->translation);
                pose.rotation = smoother.presented(interaction_id, simulated->rotation);
                return pose;
            }
        const auto* wt = mirror_world.get<ecs::WorldTransform>(mirror_object);
        return wt ? wt->value : core::Transform{};
    };
    // After an applied checkpoint: count what the roster did, and hand the smoother the jump from
    // what was on screen to what the corrected simulation now says.
    const auto note_object_checkpoint = [&](bool was_member,
                                            const std::optional<core::Transform>& simulated_before,
                                            const core::Transform& shown_before,
                                            std::uint64_t checkpoint_server_tick,
                                            double player_jump) {
        const bool member = object_member();
        if (member && !was_member)
            ++crate_joins_seen;
        if (!member && was_member) {
            ++crate_leaves_seen;
            smoother.forget(interaction_id); // the mirror presents it again
        }
        if (!member)
            return;
        const auto simulated_after = interaction_hooks->object_pose();
        if (!simulated_after)
            return;
        smoother.on_correction(
            interaction_id, shown_before.translation, simulated_after->translation);
        smoother.on_correction(interaction_id, shown_before.rotation, simulated_after->rotation);
        if (was_member && simulated_before) {
            const double jump = static_cast<double>(
                core::length(simulated_before->translation - simulated_after->translation));
            crate_max_sim_correction = std::max(crate_max_sim_correction, jump);
            ++crate_member_checkpoints;
            if (jump > 0.01)
                ++crate_corrections_over_1cm;
            if (options.interaction_trace)
                crate_trace.push_back(
                    {static_cast<double>(checkpoint_server_tick), jump, player_jump});
        }
    };

    const auto update_presentation = [&] {
        if (!spawn_reconciler || !spawn_hooks)
            return;
        std::unordered_set<replication::SpawnKey, replication::SpawnKeyHash> visible;
        std::unordered_map<replication::SpawnKey, std::uint64_t, replication::SpawnKeyHash>
            live_per_key;
        for (const auto& binding : spawn_bindings)
            if (spawn_hooks->is_live(binding.id)) {
                visible.insert(binding.key);
                ++live_per_key[binding.key];
            }
        if (!options.no_prediction &&
            options.spawn_debug_break != SpawnDebugBreak::AuthorityPresentation) {
            for (const auto& provisional : spawn_reconciler->provisional())
                if (spawn_hooks->is_live(provisional.id)) {
                    visible.insert(provisional.key);
                    ++live_per_key[provisional.key];
                }
            for (const auto& detached : detached_guesses)
                if (spawn_hooks->is_live(detached.id)) {
                    visible.insert(detached.key);
                    ++live_per_key[detached.key];
                }
        }
        // Count identities per key before the set coalesces them for ledger events. A surviving
        // provisional ghost beside its adopted roster entity must make this diagnostic exceed 1.
        for (const auto& [key, count] : live_per_key) {
            (void)key;
            live_presentations_max = std::max(live_presentations_max, count);
        }
        for (const auto& key : visible) {
            // A suspended key that becomes visible again is the replica of a late verdict -- or
            // a guess resurrected by replay, which must never happen. Counted either way; the
            // proof says which legs may show it.
            if (live_presentations.insert(key).second && suspended_keys.erase(key) != 0)
                ++presentation_resumed;
            if ((options.spawn_debug_break == SpawnDebugBreak::NoLedger ||
                 ledger.should_play(key, replication::PresentationEventKind::Launch, 0))) {
                ++presentation_launch;
                launched_presentations.insert(key);
            }
            if (spawn_press_tick && !spawn_responded) {
                spawn_response_ticks = report.ticks + 1 - *spawn_press_tick;
                spawn_responded = true;
            }
        }
        // An entity that still exists but whose own age reached the game's life limit really
        // ended -- a still-tracked guess, or the adopted replica, which (predicted ahead of the
        // server by the round trip) normally reaches its life before the server's binding goes.
        // Losing the guess during reconciliation or at a diagnostic budget is NOT an end: the
        // entity is gone, not expired, and its key waits for the verdict.
        const auto ended_in_place = [&](replication::PredictedEntityId id) {
            return spawn_hooks->entity_for(id) != ecs::kNullEntity && !spawn_hooks->is_live(id);
        };
        for (const auto& provisional : spawn_reconciler->provisional())
            if (ended_in_place(provisional.id) && ended_keys.insert(provisional.key).second)
                ++lifetime_ends;
        for (const auto& binding : spawn_bindings)
            if (ended_in_place(binding.id) && ended_keys.insert(binding.key).second)
                ++lifetime_ends;
        for (auto it = live_presentations.begin(); it != live_presentations.end();) {
            if (visible.contains(*it)) {
                ++it;
                continue;
            }
            if (!ledger.cancelled(*it) && ended_keys.contains(*it) &&
                launched_presentations.contains(*it)) {
                if (ledger.should_play(*it, replication::PresentationEventKind::End, 0))
                    ++presentation_end;
                ledger.release(*it);
            } else if (!ledger.cancelled(*it)) {
                ++presentation_suspended;
                suspended_keys.insert(*it);
            }
            it = live_presentations.erase(it);
        }
        // AcceptedEnded can arrive after a local removal, when there is no visible transition.
        for (const auto& key : ended_keys)
            if (launched_presentations.contains(key) && !visible.contains(key) &&
                !ledger.cancelled(key) &&
                ledger.should_play(key, replication::PresentationEventKind::End, 0))
                ++presentation_end;
    };

    const auto apply_pending = [&] {
        for (const auto& checkpoint : pending) {
            const auto& base = checkpoint.base;
            if (spawn_reconciler) {
                if (!checkpoint.spawns) {
                    ++spawn_checkpoints_without_extension;
                    continue;
                }
                const bool has_player = std::any_of(
                    base.entities.begin(), base.entities.end(), [&](const auto& member) {
                        return mirror.map().resolve({member.id.index, member.id.generation}) ==
                               mirror_player;
                    });
                if (!has_player) {
                    ++spawn_checkpoints_without_player;
                    continue;
                }
                const auto before = presented_position();
                const bool object_was_member = object_member();
                const auto object_simulated_before =
                    interaction_hooks ? interaction_hooks->object_pose() : std::nullopt;
                const auto object_shown_before =
                    interaction_hooks ? presented_object() : core::Transform{};
                const auto result = spawn_reconciler->apply_checkpoint(base, *checkpoint.spawns);
                if (!result.base.applied)
                    continue;
                if (interaction_hooks)
                    note_object_checkpoint(
                        object_was_member,
                        object_simulated_before,
                        object_shown_before,
                        base.server_tick,
                        static_cast<double>(core::length(presented_position() - before)));
                for (const auto& old : spawn_bindings)
                    if (std::none_of(checkpoint.spawns->bindings.begin(),
                                     checkpoint.spawns->bindings.end(),
                                     [&](const auto& binding) { return binding.key == old.key; }))
                        ended_keys.insert(old.key);
                for (const auto& outcome : checkpoint.spawns->outcomes)
                    if (outcome.kind == replication::SpawnOutcomeKind::AcceptedEnded)
                        ended_keys.insert(outcome.key);
                // Bindings are authoritative for this applied roster only; a stale or refused
                // checkpoint cannot replace the keys used by presentation.
                spawn_bindings = checkpoint.spawns->bindings;
                for (const auto& cancelled : result.cancelled) {
                    live_presentations.erase(cancelled.key);
                    ++presentation_cancelled;
                }
                record_snap(before);
                ++checkpoints;
                replayed += result.base.replayed;
                max_replay = std::max<std::uint64_t>(max_replay, result.base.replayed);
                window.apply_result(base.result);
                // Acknowledge only applied verdicts. The held frontier cannot advance from a
                // malformed, refused, or stale checkpoint, even if its wire bytes decoded.
                const auto held = spawn_reconciler->outcomes_held_through();
                if (held && (!acknowledged_outcomes || *held > *acknowledged_outcomes)) {
                    std::array<std::byte, replication::kSpawnOutcomeAckBytes> bytes{};
                    const auto size = replication::encode_spawn_outcome_ack({*held}, bytes);
                    auto* session = driver.session(*session_id);
                    if (session && session->send_reliable(std::span(bytes).first(size), now_ms())) {
                        acknowledged_outcomes = held;
                        ++spawn_acks_sent;
                    } else {
                        ++spawn_ack_send_refused;
                    }
                }
                update_presentation();
                continue;
            }
            if (!reconciler || base.entities.size() != 1)
                continue;
            const auto id = base.entities.front().id;
            if (mirror.map().resolve({id.index, id.generation}) != mirror_player)
                continue;
            if (!has_epoch || base.membership_epoch != epoch) {
                epoch = base.membership_epoch;
                reconciler->set_membership(epoch, {id});
                has_epoch = true;
            }
            const auto before = presented_position();
            const auto result = reconciler->apply_checkpoint(base);
            if (result.applied) {
                record_snap(before);
                ++checkpoints;
                replayed += result.replayed;
                max_replay = std::max<std::uint64_t>(max_replay, result.replayed);
                window.apply_result(base.result);
            }
        }
        pending.clear();
    };

    const auto pump = [&] {
        const auto before_mirror =
            (reconciler || spawn_reconciler) ? std::optional{presented_position()} : std::nullopt;
        events.clear();
        messages.clear();
        driver.update(now_ms(), events);
        for (auto& event : events) {
            if (event.kind == net::SessionEvent::Kind::Connected) {
                // The overrides exist to provoke a Schema refusal on purpose; everything else uses
                // the ONE identity function the server uses, so a game that opts into more (m20.5's
                // spawn prediction folds itself in) cannot drift between the two ends again.
                auto input_profile = *definition.network_input;
                auto remote_profile = *definition.remote_play;
                if (options.has_profile_version_override)
                    input_profile.profile_version = options.profile_version_override;
                if (options.has_fixed_dt_override)
                    remote_profile.fixed_dt = options.fixed_dt_override;
                replication::InputHello hello{
                    2, 2, input_schema_identity(input_profile, &remote_profile)};
                std::array<std::byte, replication::kInputHelloBytes> bytes{};
                const auto size = replication::encode_input_hello(hello, bytes);
                if (auto* session = driver.session(*session_id))
                    (void)session->send_reliable(std::span(bytes).first(size), now_ms());
            } else if (event.kind == net::SessionEvent::Kind::Disconnected ||
                       event.kind == net::SessionEvent::Kind::ConnectFailed) {
                disconnected = true;
                graceful = event.kind == net::SessionEvent::Kind::Disconnected &&
                           event.reason == net::DisconnectReason::Graceful;
                failure_reason = event.kind == net::SessionEvent::Kind::ConnectFailed
                                     ? "unreachable"
                                     : "server disconnected before initial world";
                // A refusal is the server's reliable reply followed by its disconnect. Read in one
                // update, the reply rides the Disconnected event; decoding it below replaces this
                // generic reason with the real one (full, schema, version).
                for (auto& message : event.undrained)
                    messages.push_back(std::move(message));
            }
        }
        if (auto* session = driver.session(*session_id))
            session->drain_received(messages);
        mirror.apply_messages(messages);
        for (const auto& message : messages) {
            replication::InputHelloReply reply{};
            if (replication::decode_input_hello_reply(message.bytes, reply)) {
                accepted = reply.refusal == replication::InputHelloRefusal::None;
                if (!accepted) {
                    disconnected = true;
                    failure_reason =
                        reply.refusal == replication::InputHelloRefusal::Schema    ? "schema"
                        : reply.refusal == replication::InputHelloRefusal::Version ? "version"
                                                                                   : "full";
                }
                continue;
            }
            replication::InputResult result{};
            if (replication::decode_input_result(message.bytes, result)) {
                window.apply_result(result);
                continue;
            }
            // Decode the whole extension before any state changes: dropping verdicts but applying
            // their roster would leave a ghost provisional entity alive.
            replication::SpawnCheckpoint checkpoint{};
            if (replication::decode_spawn_checkpoint(message.bytes, checkpoint))
                pending.push_back(std::move(checkpoint));
        }
        if (reconciler || spawn_reconciler)
            apply_pending();
        if (before_mirror && options.no_prediction && checkpoints > 0)
            record_snap(*before_mirror);
        mirror.send_ack(driver, now_ms());
    };

    while (Clock::now() < connect_deadline && !disconnected && !reconciler && !spawn_reconciler) {
        pump();
        if (accepted && mirror.deltas_applied() > 0 && !pending.empty()) {
            const auto& checkpoint = pending.back().base;
            if (!definition.remote_play->spawn_prediction && checkpoint.entities.size() != 1)
                continue;
            for (const auto& member : checkpoint.entities) {
                const auto candidate =
                    mirror.map().resolve({member.id.index, member.id.generation});
                if (candidate != ecs::kNullEntity &&
                    candidate == definition.remote_play->owned_player(mirror_world)) {
                    mirror_player = candidate;
                    break;
                }
            }
            if (mirror_player != ecs::kNullEntity && game->bind_remote_world(mirror_world)) {
                predicted_player = definition.remote_play->seed_prediction(
                    mirror_world, prediction_world, mirror_player);
                if (const auto& profile = definition.remote_play->spawn_prediction) {
                    auto* session = driver.session(*session_id);
                    if (!session || !profile->client_hooks)
                        return fail(report, "spawn client hooks unavailable");
                    spawn_hooks = profile->client_hooks(
                        prediction_world,
                        predicted_player,
                        session->incarnation(),
                        [&](const replication::SpawnKey& key, std::span<const std::byte> state) {
                            return spawn_reconciler->spawn_provisional(key, state);
                        });
                    if (interaction) {
                        // The object's roster id is its world-stream NetId, which the mirror
                        // already holds: the server names it, the client only looks it up.
                        mirror_object = interaction->object(mirror_world);
                        const auto net_id = mirror_object != ecs::kNullEntity
                                                ? mirror.map().net_id_of(mirror_object)
                                                : std::nullopt;
                        if (!net_id || !interaction->client_hooks)
                            return fail(report, "interaction object unavailable");
                        interaction_id = {net_id->index, net_id->generation};
                        interaction_hooks = interaction->client_hooks(
                            prediction_world,
                            predicted_player,
                            interaction_id,
                            *spawn_hooks,
                            {options.interaction_debug_break != InteractionDebugBreak::NoRebuild,
                             options.interaction_debug_break ==
                                 InteractionDebugBreak::StaleRestore});
                        spawn_hooks = interaction_hooks->hooks;
                    }
                    if (options.spawn_debug_break == SpawnDebugBreak::NoAdopt) {
                        spawn_hooks->spawn.adopt = [&](auto provisional, auto authoritative) {
                            // Keep the old entity beside a fresh replica so the live-key gate
                            // proves that it can see the ghost named by ADR-0105 §5.
                            for (const auto& guess : spawn_reconciler->provisional())
                                if (guess.id == provisional)
                                    detached_guesses.push_back(guess);
                            spawn_hooks->spawn.create(authoritative);
                        };
                    }
                    replication::SpawnBudget spawn_budget{};
                    if (options.spawn_unresolved_bound)
                        spawn_budget.max_unresolved_sequences = options.spawn_unresolved_bound;
                    spawn_reconciler = std::make_unique<replication::SpawnReconciler>(
                        spawn_hooks->prediction,
                        spawn_hooks->spawn,
                        definition.remote_play->fixed_dt,
                        session->incarnation(),
                        &ledger,
                        replication::ReconcilerBudget{},
                        spawn_budget);
                    std::vector<replication::PredictedEntityId> members;
                    for (const auto& member : checkpoint.entities)
                        members.push_back(member.id);
                    spawn_reconciler->set_membership(checkpoint.membership_epoch,
                                                     std::move(members));
                } else {
                    reconciler = std::make_unique<replication::Reconciler>(
                        definition.remote_play->prediction_hooks(prediction_world,
                                                                 predicted_player),
                        definition.remote_play->fixed_dt);
                }
                // The initial checkpoint follows the same applied-verdict and ack path.
                apply_pending();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!reconciler && !spawn_reconciler)
        return fail(report,
                    disconnected ? failure_reason
                                 : "timed out waiting for ownership and initial world");

    std::unique_ptr<render::MeshRegistry> meshes;
    std::unique_ptr<render::MaterialRegistry> materials;
    std::unique_ptr<render::SceneRenderer> renderer;
    if (!options.no_gpu && app.device()) {
        meshes = std::make_unique<render::MeshRegistry>(*app.device());
        materials = std::make_unique<render::MaterialRegistry>();
        renderer = std::make_unique<render::SceneRenderer>(*app.device(), *meshes, *materials);
        renderer->set_frames_in_flight(app.frames_in_flight());
        PresentSetupContext setup{mirror_world, *meshes, *materials, *renderer};
        game->present_setup(setup);
        app.on_render([&](FrameContext& ctx) {
            // The frame's interpolation fraction is a double; the render transforms are float. The
            // narrowing is intended (a [0,1) blend factor loses nothing that matters) -- and MSVC's
            // C4244 is an error under /WX, which is how CI on #350 caught it being implicit.
            (void)replication::update_render_transforms(mirror_world,
                                                        static_cast<float>(ctx.alpha));
            if (!options.no_prediction) {
                if (const auto* source =
                        prediction_world.get<ecs::WorldTransform>(predicted_player))
                    mirror_world.add_component(mirror_player, ecs::RenderTransform{source->value});
                if (interaction_hooks && interaction_hooks->object_pose())
                    mirror_world.add_component(mirror_object,
                                               ecs::RenderTransform{presented_object()});
            }
            PresentFrameContext frame{mirror_world, ctx.frame_dt, ctx.alpha};
            game->present_frame(frame);
            if (spawn_reconciler && !options.no_prediction)
                game->present_prediction(frame, prediction_world);
            ctx.present = renderer->render(*ctx.graph, mirror_world, ctx.extent, true).ldr;
        });
    }

    InputMapper mapper(definition.input);
    auto deadline = Clock::now();
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(definition.remote_play->fixed_dt));
    while (!disconnected &&
           (options.frames == 0 || report.frames < static_cast<std::uint64_t>(options.frames))) {
        deadline += period;
        std::this_thread::sleep_until(deadline);
        if (impaired)
            impaired->client_tick(report.ticks + 1);
        pump();
        if (disconnected)
            break;
        ActionState actions{};
        if (options.input_tape_push)
            actions = interaction->diagnostic_push_input(report.ticks + 1);
        else if (options.input_tape_throw)
            actions =
                definition.remote_play->spawn_prediction->diagnostic_throw_input(report.ticks + 1);
        else if (options.input_tape)
            actions = definition.remote_play->diagnostic_input_tape(report.ticks + 1);
        else if (options.autopilot)
            actions = game->autopilot();
        else if (app.windowed()) {
            mapper.update(app.frame_input());
            actions = mapper.take();
        }
        auto command = definition.remote_play->to_command(actions);
        const bool non_neutral =
            command.move_x != 0 || command.move_y != 0 || command.held != 0 || command.pressed != 0;
        if (non_neutral && !first_input_tick) {
            first_input_tick = report.ticks + 1;
            first_input_position = presented_position();
        }
        command.sequence = ++sequence;
        if (options.input_tape_throw && command.pressed != 0 && !spawn_press_tick)
            spawn_press_tick = report.ticks + 1;
        window.push(command);
        if (spawn_reconciler) {
            const auto object_before =
                interaction_hooks ? presented_object().translation : core::Vec3{};
            spawn_reconciler->predict(command);
            update_presentation();
            if (interaction_hooks) {
                smoother.advance(definition.remote_play->fixed_dt);
                if (object_member())
                    ++crate_member_ticks;
                if (!crate_push_tick && interaction_hooks->touching()) {
                    crate_push_tick = report.ticks + 1;
                    crate_push_pose = object_before;
                }
                // 1 mm: above the smoother's decaying residual offsets, far below a push step
                // (4 m/s is 67 mm per tick).
                if (crate_push_tick && !crate_responded &&
                    core::length(presented_object().translation - crate_push_pose) > 1e-3f) {
                    crate_response_ticks = report.ticks + 1 - *crate_push_tick;
                    crate_responded = true;
                }
            }
        } else {
            reconciler->predict(command);
        }
        if (first_input_tick && !responded &&
            core::length(presented_position() - *first_input_position) > 1e-5f) {
            response_ticks = report.ticks + 1 - *first_input_tick;
            responded = true;
        }
        std::array<std::byte,
                   replication::kInputV2HeaderBytes +
                       replication::kInputV2MaxCommands * replication::kInputV2CommandBytes>
            bytes{};
        const auto size = replication::encode_input_v2(window.packet(), bytes);
        if (auto* session = driver.session(*session_id))
            (void)session->send_unreliable(
                std::span(bytes).first(size), now_ms(), kRemoteInputCommandsStream);
        mirror.send_ack(driver, now_ms());
        mirror.settle_transform_history();
        (void)app.step(definition.remote_play->fixed_dt);
        ++report.frames;
        ++report.ticks;
    }
    if (options.frames == 0 && disconnected)
        pump(); // the closing packet may share a poll with the final state
    if (options.frames == 0 && !graceful)
        return fail(report, "server did not disconnect gracefully");
    app.finish_gpu();
    report.state_digest = definition.remote_play->digest(mirror_world);
    report.finished = disconnected;
    const auto& base = spawn_reconciler ? spawn_reconciler->base() : *reconciler;
    // SpawnReconciler rejects stale extended checkpoints before its inner Reconciler sees them.
    // Keep the established JSON field tied to the active checkpoint path's refusal counter.
    const auto stale_checkpoints =
        spawn_reconciler ? spawn_reconciler->stale_checkpoints() : base.stale_checkpoints();
    std::string spawn_metrics;
    if (spawn_reconciler) {
        const auto& s = *spawn_reconciler;
        spawn_metrics = fmt::format(
            ",\"prediction_path\":\"spawn\",\"presentation_launch\":{},"
            "\"presentation_end\":{},\"presentation_cancelled\":{},"
            "\"presentation_suspended\":{},\"presentation_resumed\":{},"
            "\"presentation_lifetime_ends\":{},"
            "\"spawn_response_ticks\":{},"
            "\"live_presentations_max\":{},\"spawn_acks_sent\":{},"
            "\"spawn_ack_send_refused\":{},\"ledger_played\":{},"
            "\"ledger_duplicates_suppressed\":{},\"ledger_suppressed_cancelled\":{},"
            "\"ledger_adoptions\":{},\"ledger_duplicate_adoptions\":{},"
            "\"ledger_adopt_after_cancel\":{},\"ledger_cancels\":{},"
            "\"ledger_duplicate_cancels\":{},\"ledger_key_overflow\":{},"
            "\"ledger_event_overflow\":{},\"ledger_tombstones_evicted\":{},"
            "\"spawn_rejected_launches\":{},\"spawn_rejected_unpredicted\":{},"
            "\"spawn_local_removals\":{},\"spawn_local_releases\":{},"
            "\"spawn_authoritative_cancels\":{},\"spawn_unreplayed_guesses\":{},"
            "\"spawn_frontier_ahead_of_client\":{},\"spawn_epoch_roster_mismatch\":{},"
            "\"spawn_retired_without_launch\":{},\"spawn_duplicate_outcomes_ignored\":{},"
            "\"spawn_confirmed_after_expiry_alive\":{},"
            "\"spawn_confirmed_after_expiry_ended\":{},\"spawn_adopted\":{},"
            "\"spawn_accepted_ended\":{},\"spawn_unpredicted_outcomes\":{},"
            "\"spawn_contradictory_outcomes\":{},\"spawn_provisional_expired\":{},"
            "\"spawn_pending_without_snapshot\":{},\"spawn_not_recreated\":{},"
            "\"spawn_recreated\":{},\"spawn_respawn_suppressed\":{},"
            "\"spawn_outside_step\":{},\"spawn_bad_key\":{},\"spawn_duplicate\":{},"
            "\"spawn_provisional_overflow\":{},\"spawn_snapshots_evicted\":{},"
            "\"spawn_tombstones_evicted\":{},\"spawn_stale_checkpoints\":{},"
            "\"spawn_invalid_base\":{}",
            presentation_launch,
            presentation_end,
            presentation_cancelled,
            presentation_suspended,
            presentation_resumed,
            lifetime_ends,
            spawn_responded ? spawn_response_ticks
                            : (spawn_press_tick ? report.ticks - *spawn_press_tick : 0),
            live_presentations_max,
            spawn_acks_sent,
            spawn_ack_send_refused,
            ledger.played(),
            ledger.duplicates_suppressed(),
            ledger.suppressed_cancelled(),
            ledger.adoptions(),
            ledger.duplicate_adoptions(),
            ledger.adopt_after_cancel(),
            ledger.cancels(),
            ledger.duplicate_cancels(),
            ledger.key_overflow(),
            ledger.event_overflow(),
            ledger.tombstones_evicted(),
            s.rejected_launches(),
            s.rejected_unpredicted(),
            s.local_removals(),
            s.local_releases(),
            s.authoritative_cancels(),
            s.unreplayed_guesses(),
            s.frontier_ahead_of_client(),
            s.epoch_roster_mismatch(),
            s.retired_without_launch(),
            s.duplicate_outcomes_ignored(),
            s.confirmed_after_expiry_alive(),
            s.confirmed_after_expiry_ended(),
            s.adopted(),
            s.accepted_ended(),
            s.unpredicted_outcomes(),
            s.contradictory_outcomes(),
            s.provisional_expired(),
            s.pending_without_snapshot(),
            s.not_recreated(),
            s.recreated(),
            s.respawn_suppressed(),
            s.spawn_outside_step(),
            s.spawn_bad_key(),
            s.spawn_duplicate(),
            s.provisional_overflow(),
            s.snapshots_evicted(),
            s.tombstones_evicted(),
            s.stale_checkpoints(),
            s.invalid_base());
        for (std::size_t i = 0; i < replication::kSpawnExtensionRefusalCount; ++i)
            spawn_metrics += fmt::format(
                ",\"spawn_invalid_extension_{}\":{}",
                i,
                s.invalid_extension(static_cast<replication::SpawnExtensionRefusal>(i)));
        spawn_metrics += fmt::format(
            ",\"spawn_checkpoints_without_extension\":{},\"spawn_checkpoints_without_player\":{}",
            spawn_checkpoints_without_extension,
            spawn_checkpoints_without_player);
        if (interaction_hooks) {
            const auto& c = smoother.counters();
            // crate_response_ticks reads the ticks run since the push when the crate never
            // visibly moved, and the whole run when no push happened (a refusal, not a pass).
            spawn_metrics +=
                fmt::format(",\"interaction\":1,\"crate_member\":{},\"crate_joins_seen\":{},"
                            "\"crate_leaves_seen\":{},\"crate_member_ticks\":{},"
                            "\"crate_corrections\":{},\"crate_snaps\":{},"
                            "\"crate_max_raw_jump\":{:.6f},\"crate_max_presented_jump\":{:.6f},"
                            "\"crate_rotation_snaps\":{},\"crate_max_raw_angle\":{:.6f},"
                            "\"crate_member_checkpoints\":{},\"crate_max_sim_correction\":{:.6f},"
                            "\"crate_corrections_over_1cm\":{},\"crate_push_tick\":{},"
                            "\"crate_response_ticks\":{}",
                            object_member() ? 1 : 0,
                            crate_joins_seen,
                            crate_leaves_seen,
                            crate_member_ticks,
                            c.corrections,
                            c.snaps,
                            static_cast<double>(c.max_raw_jump),
                            static_cast<double>(c.max_presented_jump),
                            c.rotation_snaps,
                            static_cast<double>(c.max_raw_angle),
                            crate_member_checkpoints,
                            crate_max_sim_correction,
                            crate_corrections_over_1cm,
                            crate_push_tick.value_or(0),
                            crate_responded ? crate_response_ticks
                                            : (crate_push_tick ? report.ticks - *crate_push_tick
                                                               : report.ticks));
            if (options.interaction_trace) {
                spawn_metrics += ",\"crate_trace\":[";
                for (std::size_t i = 0; i < crate_trace.size(); ++i)
                    spawn_metrics += fmt::format("{}[{},{:.6f},{:.6f}]",
                                                 i == 0 ? "" : ",",
                                                 static_cast<std::uint64_t>(crate_trace[i][0]),
                                                 crate_trace[i][1],
                                                 crate_trace[i][2]);
                spawn_metrics += "]";
            }
        }
    } else {
        spawn_metrics = ",\"prediction_path\":\"legacy\"";
    }
    fmt::print("{{\"event\":\"finished\",\"ticks\":{},\"digest\":\"0x{:016x}\","
               "\"response_ticks\":{},\"max_snap\":{:.6f},\"snaps_over_bound\":{},"
               "\"corrections\":{},\"reseeds\":{},\"replayed\":{},\"max_replay\":{},"
               "\"stale_checkpoints\":{},\"checkpoints\":{},"
               "\"malformed_datagrams\":{},\"oversized_datagrams\":{},"
               "\"epoch_mismatch\":{},\"incomplete_checkpoints\":{},"
               "\"history_overflow\":{},\"replay_budget_exceeded\":{}{} }}\n",
               report.ticks,
               report.state_digest,
               responded ? response_ticks : report.ticks,
               max_snap,
               snaps_over_bound,
               base.corrections(),
               base.reseeds(),
               replayed,
               max_replay,
               stale_checkpoints,
               checkpoints,
               socket_link.malformed(),
               socket_link.oversized(),
               base.epoch_mismatch(),
               base.incomplete_checkpoints(),
               base.history_overflow(),
               base.replay_budget_exceeded(),
               spawn_metrics);
    std::fflush(stdout);
    return report;
}

} // namespace rime::app
