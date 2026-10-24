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

#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/app/network_input.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/replication/client_replicator.hpp"
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

class ImpairedLink final : public net::Link {
public:
    ImpairedLink(net::Link& inner, std::uint32_t delay, double loss, std::uint64_t seed)
        : inner_(inner), delay_(delay), loss_(loss), rng_(seed ? seed : 1) {}

    bool send(const platform::Endpoint& to, std::span<const std::byte> bytes) override {
        if (drop())
            return true;
        outbound_.push_back({now_ms() + delay_, to, {bytes.begin(), bytes.end()}});
        return true;
    }

    std::size_t receive(std::vector<net::Datagram>& out) override {
        const auto now = now_ms();
        while (!outbound_.empty() && outbound_.front().due <= now) {
            auto packet = std::move(outbound_.front());
            outbound_.pop_front();
            (void)inner_.send(packet.peer, packet.bytes);
        }
        std::vector<net::Datagram> received;
        inner_.receive(received);
        for (auto& packet : received)
            if (!drop())
                inbound_.push_back({now + delay_, packet.from, std::move(packet.bytes)});
        const auto before = out.size();
        while (!inbound_.empty() && inbound_.front().due <= now) {
            auto packet = std::move(inbound_.front());
            inbound_.pop_front();
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

    bool drop() {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const auto sample = rng_ * 0x2545F4914F6CDD1Dull;
        return static_cast<double>(sample >> 11) * (1.0 / 9007199254740992.0) < loss_;
    }

    net::Link& inner_;
    std::uint32_t delay_;
    double loss_;
    std::uint64_t rng_;
    std::deque<Pending> outbound_, inbound_;
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
        !definition.remote_play->digest || definition.remote_play->fixed_dt <= 0 ||
        definition.remote_play->fixed_dt != 1.0 / definition.tick_hz)
        return fail(report, "invalid remote play profile or fixed timestep");

    const auto endpoint =
        platform::Endpoint::from_string(options.connect_host, options.connect_port);
    auto udp = net::UdpLink::bind(0);
    if (!endpoint || !udp)
        return fail(report, "UDP endpoint unavailable");
    std::unique_ptr<ImpairedLink> impaired;
    net::Link* link = &*udp;
    if (options.net_impair) {
        impaired = std::make_unique<ImpairedLink>(
            *link, options.net_delay_ms, options.net_loss, options.net_seed);
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
    if (!game || (options.autopilot && !game->has_autopilot()))
        return fail(report, "game or autopilot unavailable");

    replication::InputWindow window;
    std::unique_ptr<replication::Reconciler> reconciler;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> messages;
    std::vector<replication::OwnerCheckpoint> pending;
    std::uint32_t epoch = 0;
    bool has_epoch = false, accepted = false, disconnected = false, graceful = false;
    std::string failure_reason;
    std::uint64_t checkpoints = 0, replayed = 0;
    std::uint32_t sequence = 0;
    ecs::Entity mirror_player = ecs::kNullEntity;
    ecs::Entity predicted_player = ecs::kNullEntity;

    const auto pump = [&] {
        events.clear();
        driver.update(now_ms(), events);
        for (const auto& event : events) {
            if (event.kind == net::SessionEvent::Kind::Connected) {
                replication::InputHello hello{
                    2,
                    2,
                    replication::input_schema_hash(definition.network_input->profile_id,
                                                   definition.network_input->profile_version)};
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
                                     ? "transport connection failed"
                                     : "server disconnected before initial world";
            }
        }
        messages.clear();
        if (auto* session = driver.session(*session_id))
            session->drain_received(messages);
        mirror.apply_messages(messages);
        for (const auto& message : messages) {
            replication::InputHelloReply reply{};
            if (replication::decode_input_hello_reply(message.bytes, reply)) {
                accepted = reply.refusal == replication::InputHelloRefusal::None;
                if (!accepted) {
                    disconnected = true;
                    failure_reason = reply.refusal == replication::InputHelloRefusal::Schema
                                         ? "input schema mismatch"
                                     : reply.refusal == replication::InputHelloRefusal::Version
                                         ? "input codec mismatch"
                                         : "server full";
                }
                continue;
            }
            replication::InputResult result{};
            if (replication::decode_input_result(message.bytes, result)) {
                window.apply_result(result);
                continue;
            }
            replication::OwnerCheckpoint checkpoint{};
            if (replication::decode_owner_checkpoint(message.bytes, checkpoint) ==
                replication::CheckpointRefusal::None)
                pending.push_back(std::move(checkpoint));
        }
        if (reconciler) {
            for (const auto& checkpoint : pending) {
                if (checkpoint.entities.size() != 1)
                    continue;
                const auto id = checkpoint.entities.front().id;
                if (mirror.map().resolve({id.index, id.generation}) != mirror_player)
                    continue;
                if (!has_epoch || checkpoint.membership_epoch != epoch) {
                    epoch = checkpoint.membership_epoch;
                    reconciler->set_membership(epoch, {id});
                    has_epoch = true;
                }
                const auto result = reconciler->apply_checkpoint(checkpoint);
                if (result.applied) {
                    ++checkpoints;
                    replayed += result.replayed;
                    window.apply_result(checkpoint.result);
                }
            }
            pending.clear();
        }
        mirror.send_ack(driver, now_ms());
    };

    while (Clock::now() < connect_deadline && !disconnected && !reconciler) {
        pump();
        if (accepted && mirror.deltas_applied() > 0 && !pending.empty()) {
            const auto& checkpoint = pending.back();
            if (checkpoint.entities.size() == 1) {
                const auto id = checkpoint.entities.front().id;
                mirror_player = mirror.map().resolve({id.index, id.generation});
                if (mirror_player != ecs::kNullEntity && game->bind_remote_world(mirror_world)) {
                    predicted_player = definition.remote_play->seed_prediction(
                        mirror_world, prediction_world, mirror_player);
                    reconciler = std::make_unique<replication::Reconciler>(
                        definition.remote_play->prediction_hooks(prediction_world,
                                                                 predicted_player),
                        definition.remote_play->fixed_dt);
                    // Process the first checkpoint through the same path as all later ones.
                    const auto initial = std::move(pending);
                    pending.clear();
                    for (const auto& first : initial) {
                        if (!has_epoch || first.membership_epoch != epoch) {
                            reconciler->set_membership(first.membership_epoch,
                                                       {first.entities.front().id});
                            epoch = first.membership_epoch;
                            has_epoch = true;
                        }
                        if (reconciler->apply_checkpoint(first).applied) {
                            ++checkpoints;
                            window.apply_result(first.result);
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!reconciler)
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
            (void)replication::update_render_transforms(mirror_world, ctx.alpha);
            if (!options.no_prediction) {
                if (const auto* source =
                        prediction_world.get<ecs::WorldTransform>(predicted_player))
                    mirror_world.add_component(mirror_player, ecs::RenderTransform{source->value});
            }
            PresentFrameContext frame{mirror_world, ctx.frame_dt, ctx.alpha};
            game->present_frame(frame);
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
        pump();
        if (disconnected)
            break;
        ActionState actions{};
        if (options.autopilot)
            actions = game->autopilot();
        else if (app.windowed()) {
            mapper.update(app.frame_input());
            actions = mapper.take();
        }
        auto command = definition.remote_play->to_command(actions);
        command.sequence = ++sequence;
        window.push(command);
        reconciler->predict(command);
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
    fmt::print("{{\"event\":\"finished\",\"ticks\":{},\"digest\":\"0x{:016x}\","
               "\"corrections\":{},\"reseeds\":{},\"replayed\":{},\"checkpoints\":{}}}\n",
               report.ticks,
               report.state_digest,
               reconciler->corrections(),
               reconciler->reseeds(),
               replayed,
               checkpoints);
    std::fflush(stdout);
    return report;
}

} // namespace rime::app
