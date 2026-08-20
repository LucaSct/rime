// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 12-networked-destruction — Milestone 11's "done when" (see README.md for the design).
//
// A dedicated headless server runs the canonical destruction simulation and publishes the committed
// damage-op list. Two clients connect from DIFFERENT viewpoints, observe, and must end up agreeing
// bit for bit about what broke — while provably receiving different bytes, because m11.5 culls a
// different set of debris transforms for each of them.
//
// THE TRANSPORT DECISION. The brick was specified as "two clients over loopback" and, in the same
// breath, "deterministic, hash-verified in CI, scripted loss, never environment luck". Those pull
// apart: real UDP brings a scheduler that differs on every CI runner, while a proof that never
// touches a socket is not a proof of networked destruction. `net::Link` already resolved this one
// layer down — UdpLink and ScriptedNetwork are the same seam — so the peers here are written once,
// against Link, and the transport is a flag. CI gates on the scripted run (exact counters, exact
// packet economy); the UDP run is a convergence smoke over real sockets that asserts agreement and
// nothing about counts. Each covers the other's blind spot.
//
// WHAT IS HASHED. destruction_net::shared_state_hash — per-part alive bits and health plus debris
// COMPOSITION, in NetId order. Not DestructionWorld::state_hash(), which folds physics body ids and
// so mismatches across a wire even when the destruction is identical. And compared PEER TO PEER,
// never against a checked-in constant: the server's op list is fed by contact impulses, whose float
// results may legitimately differ between compilers, and this project has already recorded that
// same-binary determinism is not cross-platform. What may not differ is what the peers in ONE run
// agree on.
//
// Run it:  build/dev/bin/networked_destruction --headless [--cooked <dir>] [--transport=udp]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/math.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_net/components.hpp"
#include "rime/destruction_net/composition.hpp"
#include "rime/destruction_net/destruction_client.hpp"
#include "rime/destruction_net/destruction_server.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/relevancy.hpp"
#include "rime/replication/server_replicator.hpp"

#ifndef RIME_WALL_COOKED_DIR
#define RIME_WALL_COOKED_DIR "cooked"
#endif

using namespace rime;

namespace {

// ── Scale ────────────────────────────────────────────────────────────────────────────────────────
// Every number here is chosen so a piece of M11 machinery is EXERCISED rather than trivially
// satisfied, and each one is asserted on at the end. Too small is the quiet failure mode: a run
// where the budget never binds and relevancy never culls is green for reasons that have nothing to
// do with whether networked destruction works.

constexpr std::uint64_t kTickMs = 16;
constexpr float kDt = 1.0f / 60.0f;

// Six walls spread along X. Enough that fracture yields debris in the hundreds (so the byte budget
// binds), and enough spread that the two viewpoints below genuinely disagree about what is near.
constexpr int kWalls = 6;
constexpr float kWallSpacing = 40.0f;
constexpr std::uint64_t kAssetId =
    0x5741'4C4Cull; // 'WALL' — the id both peers resolve independently

// The two viewpoints, deliberately at opposite ends of the row. If both clients saw everything,
// relevancy would be untested and "different bytes, same hash" would be a vacuous claim.
constexpr float kRelevancyRadius = 70.0f;

// Long enough that debris settles AND that ordinary delta ticks follow entry ticks.
constexpr std::uint64_t kMatchTicks = 260;

// A tick budget for driving the network to quiescence at a barrier. A bound, not a hope: exceeding
// it is a failure with a deadline rather than a hang.
constexpr std::uint64_t kQuiesceTickBudget = 400;

// How many consecutive drained ticks with an unchanged per-client hash count as settled. Watching
// each client's OWN hash go quiet is deliberately not the same as comparing it to the server's:
// the latter would beg the very question the barrier exists to ask.
constexpr std::uint64_t kQuiesceStableTicks = 10;

// The match tick at which the negative control drops one client's damage ops. Inside the first
// burst, so there is certainly something to lose.
constexpr std::uint64_t kSabotageTick = 25;

// ── The scripted match (ADR-0033 A6) ─────────────────────────────────────────────────────────────
// The damage source is DATA — timed shots at named walls — never anything physics-derived. That is
// what makes the run reproducible: a shooter that aimed by querying the solver would re-introduce
// exactly the float dependence the op-list design removes. There is no player controller and no
// weapon anywhere in engine/, and inventing one here would be guessing at M12's design.
// `burst` is how many consecutive match ticks the shot keeps firing. A single tap does not kill a
// part — the first version of this match fired one op per wall and ended with every wall intact,
// which made every downstream assertion vacuous. Damage accumulates, so a burst is what a "shot"
// has to mean here.
struct Shot {
    std::uint64_t tick;
    std::uint64_t burst;
    int wall;
    float amount;
    float radius;
    core::Vec3 impulse;
};

constexpr Shot kMatch[] = {
    {20, 10, 0, 0.60f, 0.45f, {0.0f, 0.0f, 4.0f}},
    {45, 10, 3, 0.60f, 0.45f, {0.0f, 0.0f, 4.0f}},
    {75, 12, 1, 0.70f, 0.40f, {0.0f, 0.0f, 5.0f}},
    {105, 10, 5, 0.60f, 0.45f, {0.0f, 0.0f, 4.0f}},
    {135, 14, 2, 0.75f, 0.50f, {0.0f, 0.0f, 6.0f}},
    {175, 10, 4, 0.60f, 0.45f, {0.0f, 0.0f, 4.0f}},
};

// Compare the peers here. Barriers localize a divergence to the shot that caused it, instead of
// only reporting at the end that the worlds differ.
constexpr std::uint64_t kBarriers[] = {60, 120, 200, kMatchTicks};

// ── Transport ────────────────────────────────────────────────────────────────────────────────────
enum class TransportKind { Scripted, Udp };

// Owns whichever Link implementation the run chose and hands out a stable Link& per node. The rest
// of the sample never learns which it got.
//
// HANDED OUT BY unique_ptr, AND THAT IS NOT STYLE. A ScriptedLink holds a back-pointer to the
// ScriptedNetwork that vended it (link.hpp: "the network must outlive its links"). So the network
// may never change address once a single node has been added — and returning this class BY VALUE
// does exactly that, moving the network out from under every link already handed out. It compiles,
// it looks like ordinary RAII, and it segfaults on the first receive() with a backtrace pointing at
// endpoint comparison rather than at the move. Pinning the whole Transport on the heap makes the
// address stable by construction instead of by hoping a container's move preserves it.
class Transport {
public:
    static std::unique_ptr<Transport>
    make(TransportKind kind, std::size_t nodes, std::uint64_t seed, float loss_rate) {
        auto t = std::make_unique<Transport>();
        t->kind_ = kind;
        if (kind == TransportKind::Scripted) {
            t->scripted_.emplace(seed,
                                 net::ScriptedNetwork::Config{/*loss_rate=*/loss_rate,
                                                              /*duplicate_rate=*/0.0f,
                                                              /*min_latency_ms=*/5,
                                                              /*max_latency_ms=*/40});
            for (std::size_t i = 0; i < nodes; ++i) {
                const net::Endpoint e{0x7F000001u, static_cast<std::uint16_t>(7800 + i)};
                t->endpoints_.push_back(e);
                t->links_.push_back(&t->scripted_->add_node(e));
            }
            return t;
        }
        // Real sockets on loopback, ephemeral ports read back after bind.
        for (std::size_t i = 0; i < nodes; ++i) {
            auto link = net::UdpLink::bind(0, "127.0.0.1");
            if (!link.has_value()) {
                std::fprintf(stderr,
                             "12-networked-destruction: could not bind a loopback socket\n");
                return nullptr;
            }
            t->udp_.push_back(std::move(*link));
            t->endpoints_.push_back(t->udp_.back().local_endpoint());
        }
        for (net::UdpLink& l : t->udp_) {
            t->links_.push_back(&l);
        }
        return t;
    }

    // The virtual clock pump. A no-op on UDP, where the OS owns the clock — which is precisely why
    // the UDP run cannot make any exact claim about the packet economy.
    void advance(std::uint64_t now_ms) {
        if (scripted_.has_value()) {
            scripted_->advance_time(now_ms);
        }
    }

    [[nodiscard]] net::Link& link(std::size_t i) const { return *links_[i]; }

    [[nodiscard]] net::Endpoint endpoint(std::size_t i) const { return endpoints_[i]; }

    [[nodiscard]] bool is_scripted() const noexcept { return scripted_.has_value(); }

    [[nodiscard]] std::uint64_t packets_dropped() const noexcept {
        return scripted_.has_value() ? scripted_->packets_dropped() : 0;
    }

    [[nodiscard]] std::uint64_t packets_delivered() const noexcept {
        return scripted_.has_value() ? scripted_->packets_delivered() : 0;
    }

private:
    TransportKind kind_ = TransportKind::Scripted;
    std::optional<net::ScriptedNetwork> scripted_;
    std::deque<net::UdpLink> udp_; // deque: stable addresses, and Link& must not dangle
    std::vector<net::Endpoint> endpoints_;
    std::vector<net::Link*> links_;
};

// ── One peer ─────────────────────────────────────────────────────────────────────────────────────
// The same shape the m11.4 proofs use: an ECS world, a physics world, a destruction world, and the
// pattern registration both sides do INDEPENDENTLY from the same cooked asset. Nothing is shared
// between peers but bytes on a Link.
struct Peer {
    ecs::World world;
    physics::PhysicsWorld physics;
    destruction::DestructionWorld destruction;
    destruction::PatternId pattern{};
    std::uint64_t asset_id = 0;

    void register_components() {
        ecs::register_transform_components(world);
        destruction::register_destruction_components(world);
        destruction_net::register_destruction_net_components(world);
    }

    bool register_pattern(const assets::DestructibleAsset& asset, std::uint64_t id) {
        pattern = destruction.register_pattern(asset, physics);
        asset_id = id;
        return pattern.is_valid();
    }

    [[nodiscard]] destruction::PatternResolver resolver() {
        return [this](std::uint64_t id) {
            return id == asset_id ? pattern : destruction::PatternId{};
        };
    }

    [[nodiscard]] std::uint64_t shared_hash(const replication::NetIdMap& map) const {
        return destruction_net::shared_state_hash(world, map, destruction);
    }
};

// What one run of the match observed. Everything a caller asserts on comes back here rather than
// being checked inside the loop, so the sabotaged run can assert the OPPOSITE of the honest one
// without a second copy of the match.
struct RunResult {
    bool ran = false;
    std::uint64_t server_hash = 0;
    std::uint64_t client_hash[2] = {0, 0};

    // Liveness — the proof that the run was not vacuous.
    std::uint64_t dead_parts = 0;
    std::uint64_t debris_entities = 0;
    std::uint64_t packets_dropped = 0;
    std::uint64_t culled_irrelevant = 0;
    std::uint64_t dropped_over_budget = 0;
    std::uint64_t multipart_ticks = 0;
    std::uint64_t client_bytes[2] = {0, 0};
    std::uint64_t deltas_applied[2] = {0, 0};
    std::uint64_t malformed[2] = {0, 0};
    std::uint64_t input_consumed[2] = {0, 0};

    // Per-barrier agreement, so a divergence is reported at the shot that caused it.
    std::vector<std::uint64_t> barrier_ticks;
    std::vector<bool> barrier_agreed;
};

// Count the parts the destruction world has killed, across every bound instance. The floor this
// feeds is what stops two INTACT walls from hashing equal and calling it a proof.
std::uint64_t count_dead_parts(const destruction::DestructionWorld& d, std::uint32_t instances) {
    std::uint64_t dead = 0;
    for (std::uint32_t i = 0; i < instances; ++i) {
        const destruction::InstanceId id{i, 0};
        const std::uint32_t parts = d.instance_part_count(id);
        for (std::uint32_t p = 0; p < parts; ++p) {
            if (d.part_health(id, p) <= 0.0f) {
                ++dead;
            }
        }
    }
    return dead;
}

// `sabotage_client` is the negative control: -1 for the honest run, otherwise the index of the
// client whose damage-op stream gets one batch dropped on the floor. Without it, "the hashes agree"
// is a claim that two numbers are equal, which two empty worlds also satisfy.
RunResult run_match(const assets::DestructibleAsset& asset,
                    TransportKind kind,
                    std::uint64_t seed,
                    float loss_rate,
                    int sabotage_client) {
    RunResult r;

    auto transport = Transport::make(kind, /*nodes=*/3, seed, loss_rate);
    if (transport == nullptr) {
        return r;
    }

    Peer server;
    Peer clients[2];
    server.register_components();
    if (!server.register_pattern(asset, kAssetId)) {
        return r;
    }
    for (Peer& c : clients) {
        c.register_components();
        if (!c.register_pattern(asset, kAssetId)) {
            return r;
        }
    }

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u; // 'RIME'
    server_config.schema_hash = ecs::component_schema_hash(server.world);
    server_config.salt_seed = 0x1111ull;

    net::NetDriver server_driver{transport->link(0), server_config};
    server_driver.listen();

    std::optional<net::NetDriver> client_drivers[2];
    for (int i = 0; i < 2; ++i) {
        net::NetDriver::Config cfg = server_config;
        cfg.schema_hash = ecs::component_schema_hash(clients[i].world);
        cfg.salt_seed = 0x2222ull + static_cast<std::uint64_t>(i);
        client_drivers[i].emplace(transport->link(static_cast<std::size_t>(i) + 1), cfg);
    }

    replication::ServerReplicator state_server{server.world};
    replication::ClientReplicator state_clients[2] = {
        replication::ClientReplicator{clients[0].world},
        replication::ClientReplicator{clients[1].world}};
    destruction_net::DestructionServer destruction_server;
    destruction_net::DestructionClient destruction_clients[2];
    replication::ServerInputReceiver input_receiver;
    replication::ClientInputSender input_senders[2];

    // ── Per-client viewpoints ──
    // Sessions are assigned a viewpoint in connection order: the first at the near end of the wall
    // row, the second at the far end. They are deliberately more than a radius apart, so the two
    // clients' relevant sets genuinely differ and the "different bytes, same hash" claim has
    // something behind it.
    const core::Vec3 viewpoints[2] = {
        core::Vec3{0.0f, 0.0f, 0.0f},
        core::Vec3{static_cast<float>(kWalls - 1) * kWallSpacing, 0.0f, 0.0f}};
    std::vector<net::SessionId> session_order;

    replication::DistanceRelevancy relevancy_config;
    relevancy_config.radius = kRelevancyRadius;
    relevancy_config.hysteresis = 0.1f;
    relevancy_config.viewpoint = [&session_order, &viewpoints](net::SessionId id,
                                                               core::Vec3& out) -> bool {
        for (std::size_t i = 0; i < session_order.size() && i < 2; ++i) {
            if (session_order[i] == id) {
                out = viewpoints[i];
                return true;
            }
        }
        return false; // not yet seated — "no viewpoint" is relevant-to-everything, by design
    };
    // THE ROADMAP'S RULE, MADE EXECUTABLE: "destruction events are never culled, debris transforms
    // are distance-budgeted per client." Getting this wrong is subtle and this sample got it wrong
    // first: a plain distance policy culled whole WALLS from the far client, and since
    // shared_state_hash folds each peer's own NetIdMap, a client that was never told wall 5 exists
    // hashes five walls against the server's six. The mismatch is then CORRECT behaviour, and the
    // proof is quietly measuring relevancy instead of destruction.
    //
    // So the destructibles — the subject of the proof — are always relevant, and debris, the
    // population m11.5 was actually built to cull, stays distance-scored. That is also what makes
    // the headline claim real rather than rhetorical: the two clients demonstrably receive
    // different bytes (different debris), and must still agree bit for bit on what broke.
    auto distance = replication::distance_relevancy(server.world, relevancy_config);
    state_server.set_relevancy([&server, distance](net::SessionId id,
                                                   std::span<const ecs::Entity> candidates,
                                                   std::span<float> priorities) {
        distance(id, candidates, priorities);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (server.world.has<destruction::Destructible>(candidates[i])) {
                priorities[i] = replication::kUnpositionedPriority;
            }
        }
    });

    // A budget tight enough to actually bind. An unbinding budget makes m11.5 dead code in this
    // run, which is why entities_dropped_over_budget() is asserted non-zero at the end.
    replication::Budget budget;
    budget.max_bytes_per_tick = 900;
    state_server.set_budget(budget);

    // ── The world ──
    for (int i = 0; i < kWalls; ++i) {
        core::Transform placement;
        placement.translation.x = static_cast<float>(i) * kWallSpacing;
        const ecs::Entity e = server.world.spawn_with(ecs::LocalTransform{placement},
                                                      destruction::Destructible{kAssetId});
        (void)state_server.replicate(e);
    }

    for (int i = 0; i < 2; ++i) {
        if (!client_drivers[i]->connect(transport->endpoint(0), kTickMs).has_value()) {
            return r;
        }
    }

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0; // every tick ever driven — what the wire's tick tag wants
    std::uint64_t match_tick = 0; // only MATCH ticks, so quiescing does not skip the schedule
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;
    std::vector<replication::InputCommand> drained_input;
    int sabotage_budget = sabotage_client >= 0 ? 1 : 0;

    // One tick of the whole system, in the order Application's sim stage prescribes (ADR-0033 A5):
    // PreSim = poll, apply remote ops, bind; sim; Publish = send.
    const auto tick = [&](bool run_shots) {
        now_ms += kTickMs;
        ++tick_index;
        if (run_shots) {
            ++match_tick;
        }
        transport->advance(now_ms);

        // ── PreSim: server ──
        events.clear();
        server_driver.update(now_ms, events);
        state_server.on_session_events(events);
        input_receiver.on_session_events(events);
        for (const net::SessionEvent& e : events) {
            // Seat each client the first time we see it, so the viewpoint callback has an answer.
            bool known = false;
            for (const net::SessionId id : session_order) {
                known = known || id == e.id;
            }
            if (!known && session_order.size() < 2) {
                session_order.push_back(e.id);
            }
        }
        // The server shares one inbox between replication, destruction and input: drain_received
        // MOVES messages out, so each subsystem drawing its own would eat the others' mail.
        for (const net::SessionId id : server_driver.session_ids()) {
            net::Session* session = server_driver.session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            (void)state_server.apply_messages(id, inbox);
            (void)input_receiver.apply_messages(id, inbox);
        }

        // ── PreSim: clients ──
        for (int i = 0; i < 2; ++i) {
            events.clear();
            client_drivers[i]->update(now_ms, events);
            for (const net::SessionId id : client_drivers[i]->session_ids()) {
                net::Session* session = client_drivers[i]->session(id);
                if (session == nullptr) {
                    continue;
                }
                inbox.clear();
                r.client_bytes[i] += session->drain_received(inbox);
                state_clients[i].apply_messages(inbox);
                const bool sabotage_now =
                    sabotage_client == i && sabotage_budget > 0 && match_tick == kSabotageTick;
                if (sabotage_now) {
                    // Drop this client's damage ops on the floor, exactly once. The channel has
                    // already delivered them, so nothing retransmits — which is precisely the
                    // "a client missed an op batch" divergence the hash exists to catch.
                    --sabotage_budget;
                } else {
                    destruction_clients[i].apply_messages(
                        inbox, state_clients[i].map(), clients[i].world);
                }
            }
        }

        // Bind on every side: an entity that says it is a destructible becomes one. The server owns
        // its instances; the clients' are mirrors, which is what suppresses their contact
        // conversion — a mirror that converted its own impulses would diverge on the first pile.
        (void)destruction::bind_destructibles(server.world,
                                              server.destruction,
                                              server.physics,
                                              server.resolver(),
                                              destruction::Authority::Local);
        for (int i = 0; i < 2; ++i) {
            (void)destruction::bind_destructibles(clients[i].world,
                                                  clients[i].destruction,
                                                  clients[i].physics,
                                                  clients[i].resolver(),
                                                  destruction::Authority::Remote);
        }

        // ── The simulation ──
        server.world.advance_version();
        if (run_shots) {
            for (const Shot& shot : kMatch) {
                if (match_tick < shot.tick || match_tick >= shot.tick + shot.burst) {
                    continue;
                }
                core::Vec3 at{static_cast<float>(shot.wall) * kWallSpacing, 0.4f, 0.0f};
                server.destruction.apply_damage(
                    destruction::InstanceId{static_cast<std::uint32_t>(shot.wall), 0},
                    at,
                    shot.amount,
                    shot.radius,
                    shot.impulse);
            }
        }
        server.physics.step(kDt);
        server.destruction.update(server.physics);

        // The clients' input rides the m11.6c path for PROOF OF FLOW only (A6): it is stamped,
        // sent with a redundancy window, deduplicated, drained and acked — and nothing consumes it,
        // because there is no character controller to consume it until M12.
        for (int i = 0; i < 2; ++i) {
            // `sequence` is stamped by record() — the sender owns that counter, so setting it here
            // would be writing a field the API is about to overwrite.
            replication::InputCommand cmd;
            cmd.move_x = 0.0f;
            cmd.move_y = 1.0f;
            (void)input_senders[i].record(cmd);
        }
        for (const net::SessionId id : server_driver.session_ids()) {
            drained_input.clear();
            const std::size_t n = input_receiver.drain(id, drained_input);
            for (std::size_t i = 0; i < session_order.size() && i < 2; ++i) {
                if (session_order[i] == id) {
                    r.input_consumed[i] += n;
                }
            }
        }

        // ONE queued batch per destruction update — never two merged, which would skip the fracture
        // boundary between them and leave two waves of debris looking like one island.
        for (int i = 0; i < 2; ++i) {
            do {
                (void)destruction_clients[i].apply_next_batch(
                    clients[i].world, state_clients[i].map(), clients[i].destruction);
                clients[i].physics.step(kDt);
                clients[i].destruction.update(clients[i].physics);
            } while (destruction_clients[i].pending_batches() > 0);
        }

        // ── PostSim: the debris bridge ──
        // Every new chunk becomes a replicated entity carrying DebrisOrigin, live chunks get this
        // tick's transform, and reclaimed ones are retracted. It runs AFTER the destruction update
        // that created the chunks and BEFORE publish, so the entities carry this tick's transform
        // rather than last tick's. Omitting it is silent: the walls still break and the peers still
        // agree, because composition is DERIVED — but no rubble is ever replicated, so relevancy
        // has nothing to cull and the byte budget nothing to bind on, and the m11.5 half of this
        // proof quietly measures an empty world.
        destruction_server.sync_debris(
            server.world,
            server.destruction,
            server.physics,
            state_server.map(),
            [&state_server](ecs::Entity e) { (void)state_server.replicate(e); },
            [&state_server](ecs::Entity e) { state_server.despawn(e); });

        // ── Publish ──
        destruction_server.publish(server_driver,
                                   state_server.map(),
                                   server.world,
                                   server.destruction,
                                   tick_index,
                                   now_ms);
        state_server.publish(server_driver, now_ms);
        input_receiver.send_acks(server_driver, now_ms);
        for (int i = 0; i < 2; ++i) {
            state_clients[i].send_ack(*client_drivers[i], now_ms);
            input_senders[i].send(*client_drivers[i], now_ms);
        }
    };

    // Drive with no new damage until every channel is idle and both clients have caught up. The
    // peers are NOT in lockstep — two server ticks can land in one client tick — so comparing them
    // mid-flight is meaningless and would fail against a correct engine. Quiescence is what makes
    // the comparison well defined.
    const auto quiesce = [&]() -> bool {
        std::uint64_t last[2] = {0, 0};
        std::uint64_t stable = 0;
        for (std::uint64_t i = 0; i < kQuiesceTickBudget; ++i) {
            tick(/*run_shots=*/false);
            const std::uint64_t h0 = clients[0].shared_hash(state_clients[0].map());
            const std::uint64_t h1 = clients[1].shared_hash(state_clients[1].map());
            const bool drained = destruction_clients[0].pending_batches() == 0 &&
                                 destruction_clients[1].pending_batches() == 0;
            stable = (drained && h0 == last[0] && h1 == last[1]) ? stable + 1 : 0;
            last[0] = h0;
            last[1] = h1;
            if (stable >= kQuiesceStableTicks) {
                return true;
            }
        }
        return false;
    };

    std::size_t next_barrier = 0;
    for (std::uint64_t t = 0; t < kMatchTicks; ++t) {
        tick(/*run_shots=*/true);
        if (next_barrier < std::size(kBarriers) && match_tick >= kBarriers[next_barrier]) {
            const bool settled = quiesce();
            const std::uint64_t sh = server.shared_hash(state_server.map());
            const std::uint64_t c0 = clients[0].shared_hash(state_clients[0].map());
            const std::uint64_t c1 = clients[1].shared_hash(state_clients[1].map());
            r.barrier_ticks.push_back(match_tick);
            r.barrier_agreed.push_back(settled && sh == c0 && sh == c1);
            ++next_barrier;
        }
    }

    (void)quiesce();

    r.ran = true;
    r.server_hash = server.shared_hash(state_server.map());
    r.client_hash[0] = clients[0].shared_hash(state_clients[0].map());
    r.client_hash[1] = clients[1].shared_hash(state_clients[1].map());
    r.dead_parts = count_dead_parts(server.destruction, kWalls);
    r.packets_dropped = transport->packets_dropped();
    r.culled_irrelevant = state_server.entities_culled_irrelevant();
    r.dropped_over_budget = state_server.entities_dropped_over_budget();
    r.multipart_ticks = state_server.multipart_ticks();
    for (int i = 0; i < 2; ++i) {
        r.deltas_applied[i] = state_clients[i].deltas_applied();
        r.malformed[i] = state_clients[i].malformed_messages();
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path cooked_dir = RIME_WALL_COOKED_DIR;
    TransportKind kind = TransportKind::Scripted;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cooked" && i + 1 < argc) {
            cooked_dir = argv[++i];
        } else if (a == "--transport=udp") {
            kind = TransportKind::Udp;
        } else if (a == "--transport=scripted" || a == "--headless") {
            // --headless is the CI entry point and the default shape; accepted for symmetry with
            // the other samples.
        } else {
            std::fprintf(stderr, "12-networked-destruction: unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }

    const auto bytes = platform::read_file(cooked_dir / "wall.rdest");
    if (!bytes.has_value()) {
        std::fprintf(stderr,
                     "12-networked-destruction: no wall.rdest at %s — run `rime fracture "
                     "--size 4 3 0.3 --parts 60 --seed 11 --out <dir> --name wall`\n",
                     cooked_dir.string().c_str());
        return 1;
    }
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(*bytes, err);
    if (!asset.has_value()) {
        std::fprintf(stderr, "12-networked-destruction: wall.rdest is malformed\n");
        return 1;
    }

    const bool scripted = kind == TransportKind::Scripted;
    const RunResult run = run_match(*asset,
                                    kind,
                                    /*seed=*/0xC0FFEEull,
                                    /*loss_rate=*/scripted ? 0.10f : 0.0f,
                                    /*sabotage_client=*/-1);
    if (!run.ran) {
        std::fprintf(stderr, "12-networked-destruction: the match could not be set up\n");
        return 1;
    }

    int failures = 0;
    const auto check = [&failures](bool ok, const char* what) {
        std::fprintf(stderr, "  %s  %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };

    std::fprintf(stderr,
                 "\n12-networked-destruction — %s transport\n",
                 scripted ? "scripted (deterministic)" : "loopback UDP");

    // ── The claim ──
    std::fprintf(stderr, "\nagreement\n");
    check(run.server_hash == run.client_hash[0] && run.server_hash == run.client_hash[1],
          "both clients agree with the server on shared_state_hash");
    for (std::size_t i = 0; i < run.barrier_ticks.size(); ++i) {
        std::fprintf(stderr,
                     "  %s  agreed at barrier tick %llu\n",
                     run.barrier_agreed[i] ? "ok  " : "FAIL",
                     static_cast<unsigned long long>(run.barrier_ticks[i]));
        failures += run.barrier_agreed[i] ? 0 : 1;
    }

    // ── The claim is only worth anything if the run was not vacuous ──
    // Each of these is a way this proof could be green while proving nothing. They are assertions
    // for the same reason the counters exist at all: a skip that nothing can see reads as a pass.
    std::fprintf(stderr, "\nnon-vacuousness\n");
    check(run.dead_parts > 0, "the wall actually broke (dead parts > 0)");
    check(run.deltas_applied[0] > 0 && run.deltas_applied[1] > 0,
          "both clients actually applied replication deltas");
    check(run.malformed[0] == 0 && run.malformed[1] == 0,
          "no client silently discarded a message as malformed");
    check(run.input_consumed[0] > 0 && run.input_consumed[1] > 0,
          "the m11.6c input path carried commands end to end (proof of flow)");

    if (scripted) {
        // Only the virtual clock can make these exact. On UDP the OS owns the schedule, so
        // asserting a packet economy there would be asserting the runner's mood.
        check(run.packets_dropped > 0, "the scripted link actually dropped packets");
        check(run.culled_irrelevant > 0, "relevancy actually culled entities for a client");
        check(run.client_bytes[0] != run.client_bytes[1],
              "the two clients received DIFFERENT byte counts (relevancy really differed)");
        check(run.dropped_over_budget > 0 || run.multipart_ticks > 0,
              "the byte budget actually bound (deferrals or multipart ticks)");

        // ── The negative control ──
        // Same scenario, same seed, same losses — one client's op stream loses a single batch. If
        // the hashes still agree, then "the hashes agree" was never evidence of anything.
        const RunResult sabotaged = run_match(*asset,
                                              kind,
                                              /*seed=*/0xC0FFEEull,
                                              /*loss_rate=*/0.10f,
                                              /*sabotage_client=*/1);
        std::fprintf(stderr, "\nnegative control (client 1's op stream loses one batch)\n");
        check(sabotaged.ran, "the sabotaged run completed");
        check(sabotaged.ran && sabotaged.client_hash[1] != sabotaged.server_hash,
              "the sabotaged client DISAGREES — the hash can detect a broken op stream");
        check(sabotaged.ran && sabotaged.client_hash[0] == sabotaged.server_hash,
              "the untouched client still agrees — the sabotage was targeted, not global");
    }

    std::fprintf(stderr,
                 "\nserver %016llx | client0 %016llx | client1 %016llx\n"
                 "dead parts %llu | dropped %llu | culled %llu | over-budget %llu | "
                 "multipart %llu\nclient bytes %llu / %llu | input consumed %llu / %llu\n",
                 static_cast<unsigned long long>(run.server_hash),
                 static_cast<unsigned long long>(run.client_hash[0]),
                 static_cast<unsigned long long>(run.client_hash[1]),
                 static_cast<unsigned long long>(run.dead_parts),
                 static_cast<unsigned long long>(run.packets_dropped),
                 static_cast<unsigned long long>(run.culled_irrelevant),
                 static_cast<unsigned long long>(run.dropped_over_budget),
                 static_cast<unsigned long long>(run.multipart_ticks),
                 static_cast<unsigned long long>(run.client_bytes[0]),
                 static_cast<unsigned long long>(run.client_bytes[1]),
                 static_cast<unsigned long long>(run.input_consumed[0]),
                 static_cast<unsigned long long>(run.input_consumed[1]));

    std::fprintf(stderr, "\n%s\n", failures == 0 ? "M11 green." : "M11 NOT green.");
    return failures == 0 ? 0 : 1;
}
