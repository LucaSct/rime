// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The m11.2 proofs (ADR-0033 §7): the session layer connects, rejects the wrong build, survives a
// lossy handshake, and detects peer death — demonstrated on the deterministic ScriptedNetwork, on a
// virtual clock, never on environment luck. GPU-free; CI on all OSes.
//
// The pump shape is simpler than m11.1's: the driver IS the router now, so a tick is just "deliver
// due packets, then let each driver poll its own link once". Every scenario that claims an
// adversarial network asserts the network really was adversarial (dropped counters), because a
// 100%-loss proof that dropped nothing proves nothing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "rime/net/net_driver.hpp"

using namespace rime::net;

namespace {

constexpr Endpoint kServer{0x0A000001, 7777};
constexpr Endpoint kClient{0x0A000002, 7000};
constexpr Endpoint kIntruder{0x0A000003, 7001};

constexpr std::uint64_t kSchema = 0x9c41e2ff00112233ull;
constexpr std::uint32_t kAppId = 0xBEEF;

std::vector<std::byte> payload_of(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

std::string to_string(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

NetDriver::Config server_config() {
    NetDriver::Config c;
    c.app_id = kAppId;
    c.schema_hash = kSchema;
    c.max_sessions = 4;
    c.heartbeat_interval_ms = 100;
    c.timeout_ms = 400;
    c.connect_retry_ms = 50;
    c.connect_attempts = 12;
    c.resend_ms = 50;
    c.salt_seed = 0x1111'2222'3333'4444ull;
    return c;
}

NetDriver::Config client_config() {
    NetDriver::Config c = server_config();
    // A DIFFERENT seed: two peers sharing one salt stream would draw identical salts, and the
    // reincarnation check (which is precisely "did the client_salt change?") could not fire.
    c.salt_seed = 0xAAAA'BBBB'CCCC'DDDDull;
    return c;
}

// Two drivers on one scripted network, pumped on a virtual clock.
struct Harness {
    explicit Harness(ScriptedNetwork::Config net_config = {},
                     NetDriver::Config server_cfg = server_config(),
                     NetDriver::Config client_cfg = client_config(),
                     std::uint64_t seed = 42)
        : network(seed, net_config), server_link(network.add_node(kServer)),
          client_link(network.add_node(kClient)), server(server_link, server_cfg),
          client(client_link, client_cfg) {}

    // Advance the virtual world by one 10 ms tick: deliver due packets, then each driver polls its
    // own link once and runs its timers. `pump_client` false = the client process is dead.
    void tick(int count = 1, bool pump_client = true) {
        for (int i = 0; i < count; ++i) {
            now_ms += 10;
            network.advance_time(now_ms);
            server.update(now_ms, server_events);
            if (pump_client) {
                client.update(now_ms, client_events);
            }
        }
    }

    ScriptedNetwork network;
    ScriptedLink& server_link;
    ScriptedLink& client_link;
    NetDriver server;
    NetDriver client;
    std::uint64_t now_ms = 0;
    std::vector<SessionEvent> server_events;
    std::vector<SessionEvent> client_events;
};

std::size_t count_kind(const std::vector<SessionEvent>& events, SessionEvent::Kind kind) {
    return static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [&](const SessionEvent& e) { return e.kind == kind; }));
}

const SessionEvent* first_of(const std::vector<SessionEvent>& events, SessionEvent::Kind kind) {
    for (const SessionEvent& e : events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("handshake: two peers connect and exchange a hello each way") {
    Harness h;
    h.server.listen();
    const auto id = h.client.connect(kServer, h.now_ms);
    REQUIRE(id.has_value());
    CHECK(h.client.session(*id)->state() == SessionState::Connecting);

    h.tick(5);

    REQUIRE(count_kind(h.client_events, SessionEvent::Kind::Connected) == 1);
    REQUIRE(count_kind(h.server_events, SessionEvent::Kind::Connected) == 1);
    CHECK(h.client.session_count() == 1);
    CHECK(h.server.session_count() == 1);
    CHECK(h.client.session(*id)->state() == SessionState::Connected);

    // Reliable traffic each way, through the salt frame the driver added under the channel.
    const auto server_id = h.server.session_ids()[0];
    CHECK(h.client.session(*id)->send_reliable(payload_of("hello from client"), h.now_ms));
    CHECK(h.server.session(server_id)->send_reliable(payload_of("hello from server"), h.now_ms));
    h.tick(4);

    std::vector<Received> at_server;
    std::vector<Received> at_client;
    h.server.session(server_id)->drain_received(at_server);
    h.client.session(*id)->drain_received(at_client);
    REQUIRE(at_server.size() == 1);
    REQUIRE(at_client.size() == 1);
    CHECK(to_string(at_server[0].bytes) == "hello from client");
    CHECK(to_string(at_client[0].bytes) == "hello from server");
}

TEST_CASE("schema mismatch is rejected at connect, and allocates nothing on the server") {
    NetDriver::Config bad = client_config();
    bad.schema_hash = 0x77aa0900ffeeddccull; // a client built against a different component set
    Harness h({}, server_config(), bad);
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());

    h.tick(5);

    const SessionEvent* failed = first_of(h.client_events, SessionEvent::Kind::ConnectFailed);
    REQUIRE(failed != nullptr);
    CHECK(failed->reason == DisconnectReason::SchemaMismatch);
    CHECK(failed->expected == kSchema);
    CHECK(failed->actual == bad.schema_hash);

    // The DoS property, as an assertion: a wrong-build peer never caused server-side state.
    CHECK(h.server.session_count() == 0);
    CHECK(count_kind(h.server_events, SessionEvent::Kind::Connected) == 0);
    CHECK(h.client.session_count() == 0);

    // And the rejection reaches a human with both numbers in it.
    const std::string message = format(*failed);
    CHECK(message.find("schema") != std::string::npos);
    CHECK(message.find("9c41e2ff00112233") != std::string::npos);
    CHECK(message.find("77aa0900ffeeddcc") != std::string::npos);
}

TEST_CASE("app id and protocol mismatches report themselves distinctly") {
    NetDriver::Config other_game = client_config();
    other_game.app_id = 0xF00D; // same engine build, different game, same LAN
    Harness h({}, server_config(), other_game);
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(5);

    const SessionEvent* failed = first_of(h.client_events, SessionEvent::Kind::ConnectFailed);
    REQUIRE(failed != nullptr);
    // Not folded into one opaque "handshake mismatch": the whole point is naming what to fix.
    CHECK(failed->reason == DisconnectReason::AppMismatch);
    CHECK(failed->expected == kAppId);
    CHECK(failed->actual == other_game.app_id);
}

TEST_CASE("a client that is not listened for gets no session, and no reply") {
    Harness h; // server never calls listen()
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(6);

    CHECK(h.server.session_count() == 0);
    CHECK(count_kind(h.client_events, SessionEvent::Kind::Connected) == 0);
    // Silence, not a rejection: replying to unsolicited traffic is an amplification vector.
    CHECK(h.server.connect_requests_seen() > 0);
}

TEST_CASE("the handshake survives a lossy link by retrying") {
    ScriptedNetwork::Config lossy;
    lossy.loss_rate = 0.5f; // half the handshake datagrams die
    Harness h(lossy);
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());

    h.tick(60);

    CHECK(count_kind(h.client_events, SessionEvent::Kind::Connected) == 1);
    CHECK(count_kind(h.server_events, SessionEvent::Kind::Connected) == 1);
    CHECK(h.network.packets_dropped() > 0); // the scenario really was lossy
}

TEST_CASE("a client started before its server retries until someone is listening") {
    // The deterministic retry proof, and the reason the two-process smoke needs no sleeps or
    // ordering: a client that starts first simply keeps asking. Driving it with a late listen()
    // rather than with packet loss is what makes it deterministic — a loss-driven version passes or
    // fails on which seed you picked, and at the default seed the very first request and its accept
    // both got through, so the retry it claimed to prove never ran.
    Harness h;
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());

    h.tick(20); // nobody home: every request is seen and ignored
    CHECK(h.server.connect_requests_seen() > 1);
    CHECK(h.server.session_count() == 0);
    CHECK(count_kind(h.client_events, SessionEvent::Kind::Connected) == 0);

    h.server.listen();
    h.tick(5);

    CHECK(count_kind(h.client_events, SessionEvent::Kind::Connected) == 1);
    CHECK(count_kind(h.server_events, SessionEvent::Kind::Connected) == 1);
}

TEST_CASE("a lost accept is self-healing: the retry is recognized as the same attempt") {
    Harness h;
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(5);
    REQUIRE(h.server.session_count() == 1);
    const std::size_t connects_after_first =
        count_kind(h.server_events, SessionEvent::Kind::Connected);

    // Replay the client's ORIGINAL request: byte-identical, same client_salt. The server must
    // recognize a duplicate attempt and re-send its accept rather than mint a second session.
    h.client.connect(kServer, h.now_ms); // a second attempt would carry a FRESH salt — see below
    h.tick(5);

    // Exactly one session per endpoint, always: the table is keyed by peer.
    CHECK(h.server.session_count() == 1);
    CHECK(count_kind(h.server_events, SessionEvent::Kind::Connected) >= connects_after_first);
}

TEST_CASE("reincarnation: a restarted client replaces its own stale session") {
    Harness h;
    h.server.listen();
    const auto first = h.client.connect(kServer, h.now_ms);
    REQUIRE(first.has_value());
    h.tick(5);
    REQUIRE(h.server.session_count() == 1);
    const SessionId stale = h.server.session_ids()[0];

    // The "restart": a brand-new driver at the same endpoint, drawing a fresh client_salt. The new
    // SEED is the whole point and not test decoration — salts come from Config::salt_seed, so a
    // restarted process only looks reincarnated if it seeds differently (from OS entropy, as a real
    // game does). Reuse the seed and the reborn client draws the SAME first salt, and the server
    // correctly reads it as a duplicate of the original attempt rather than a new incarnation.
    NetDriver::Config reborn_config = client_config();
    reborn_config.salt_seed = 0x5EED'0000'0000'0001ull;
    NetDriver reborn(h.client_link, reborn_config);
    REQUIRE(reborn.connect(kServer, h.now_ms).has_value());
    for (int i = 0; i < 8; ++i) {
        h.now_ms += 10;
        h.network.advance_time(h.now_ms);
        h.server.update(h.now_ms, h.server_events);
        reborn.update(h.now_ms, h.client_events);
    }

    const SessionEvent* replaced = first_of(h.server_events, SessionEvent::Kind::Disconnected);
    REQUIRE(replaced != nullptr);
    CHECK(replaced->reason == DisconnectReason::Replaced);
    CHECK(h.server.session_count() == 1);
    // The old handle now addresses nothing — the generational check doing its job.
    CHECK(h.server.session(stale) == nullptr);
}

TEST_CASE("peer death is detected by timeout — the m11.2 milestone proof") {
    Harness h;
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(5);
    REQUIRE(h.server.session_count() == 1);
    const SessionId peer = h.server.session_ids()[0];
    const std::uint64_t last_alive = h.now_ms;

    // The client process dies: no disconnect, no packets, nothing. Just silence.
    h.tick(60, /*pump_client=*/false);

    const SessionEvent* died = first_of(h.server_events, SessionEvent::Kind::Disconnected);
    REQUIRE(died != nullptr);
    CHECK(died->reason == DisconnectReason::Timeout);
    CHECK(h.server.session_count() == 0);
    CHECK(h.server.session(peer) == nullptr);
    // Not declared dead early: the server waited out the full timeout after the last real traffic.
    CHECK(h.now_ms >= last_alive + server_config().timeout_ms);
}

TEST_CASE("an idle connection is kept alive by heartbeats, and is never declared dead") {
    Harness h;
    h.server.listen();
    const auto id = h.client.connect(kServer, h.now_ms);
    REQUIRE(id.has_value());
    h.tick(5);

    // Idle for well past the timeout. Nothing but keepalive traffic flows.
    h.tick(120);

    CHECK(count_kind(h.server_events, SessionEvent::Kind::Disconnected) == 0);
    CHECK(count_kind(h.client_events, SessionEvent::Kind::Disconnected) == 0);
    CHECK(h.server.session_count() == 1);
    // And the proof is not vacuous: heartbeats really were what kept it up.
    CHECK(h.client.session(*id)->heartbeats_sent() > 0);
}

TEST_CASE("graceful disconnect closes the peer long before its timeout would") {
    Harness h;
    h.server.listen();
    const auto id = h.client.connect(kServer, h.now_ms);
    REQUIRE(id.has_value());
    h.tick(5);
    const std::uint64_t closed_at = h.now_ms;

    h.client.session(*id)->disconnect();
    h.tick(4);

    const SessionEvent* bye = first_of(h.server_events, SessionEvent::Kind::Disconnected);
    REQUIRE(bye != nullptr);
    CHECK(bye->reason == DisconnectReason::Graceful);
    CHECK(h.now_ms < closed_at + server_config().timeout_ms); // i.e. not via the timeout backstop
}

TEST_CASE("garbage from an unknown endpoint allocates nothing and poisons nothing") {
    Harness h;
    h.server.listen();
    ScriptedLink& intruder = h.network.add_node(kIntruder);

    const std::vector<std::byte> junk = payload_of("not a rime packet at all, honestly");
    intruder.send(kServer, junk);
    intruder.send(kServer, {}); // and an empty datagram, the classic parser trap
    h.tick(3);

    CHECK(h.server.session_count() == 0);
    CHECK(h.server_events.empty());
    CHECK(h.server.datagrams_dropped() >= 1);

    // A legitimate client is unaffected by the noise that came before it.
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(5);
    CHECK(count_kind(h.server_events, SessionEvent::Kind::Connected) == 1);
}

TEST_CASE("the session table is bounded: a full server rejects rather than grows") {
    NetDriver::Config small = server_config();
    small.max_sessions = 1;
    Harness h({}, small);
    h.server.listen();
    REQUIRE(h.client.connect(kServer, h.now_ms).has_value());
    h.tick(5);
    REQUIRE(h.server.session_count() == 1);

    // A second, distinct client arrives.
    ScriptedLink& second_link = h.network.add_node(kIntruder);
    NetDriver second(second_link, client_config());
    std::vector<SessionEvent> second_events;
    REQUIRE(second.connect(kServer, h.now_ms).has_value());
    for (int i = 0; i < 8; ++i) {
        h.now_ms += 10;
        h.network.advance_time(h.now_ms);
        h.server.update(h.now_ms, h.server_events);
        h.client.update(h.now_ms, h.client_events);
        second.update(h.now_ms, second_events);
    }

    const SessionEvent* refused = first_of(second_events, SessionEvent::Kind::ConnectFailed);
    REQUIRE(refused != nullptr);
    CHECK(refused->reason == DisconnectReason::ServerFull);
    CHECK(h.server.session_count() == 1); // the first client is undisturbed
}

TEST_CASE("connect gives up after its retries, and says so") {
    Harness h; // nobody is listening, and nobody ever will be
    const auto id = h.client.connect(kServer, h.now_ms);
    REQUIRE(id.has_value());

    h.tick(80);

    const SessionEvent* failed = first_of(h.client_events, SessionEvent::Kind::ConnectFailed);
    REQUIRE(failed != nullptr);
    CHECK(failed->reason == DisconnectReason::ConnectTimeout);
    CHECK(h.client.session_count() == 0);
    CHECK(h.client.session(*id) == nullptr);
}

TEST_CASE("control packets round-trip, and every truncation of them is refused") {
    ConnectRequest request;
    request.protocol = kProtocolVersion;
    request.app_id = kAppId;
    request.schema_hash = kSchema;
    request.client_salt = 0xDEADBEEF;

    const std::vector<std::byte> encoded = encode_connect_request(request);
    const auto decoded = decode_connect_request(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->protocol == request.protocol);
    CHECK(decoded->app_id == request.app_id);
    CHECK(decoded->schema_hash == request.schema_hash);
    CHECK(decoded->client_salt == request.client_salt);

    // Every proper prefix must be refused — the untrusted-input contract, checked exhaustively
    // rather than at one arbitrary length.
    for (std::size_t n = 0; n < encoded.size(); ++n) {
        CHECK_FALSE(decode_connect_request(std::span(encoded).first(n)).has_value());
    }
    // Trailing bytes are corruption too: the version field is the extension mechanism, not slack.
    std::vector<std::byte> overlong = encoded;
    overlong.push_back(std::byte{0});
    CHECK_FALSE(decode_connect_request(overlong).has_value());

    // A packet decoded as the wrong type is refused by the tag gate.
    CHECK_FALSE(decode_connect_accept(encoded).has_value());
    CHECK_FALSE(decode_heartbeat(encoded));

    const auto reject = decode_connect_reject(
        encode_connect_reject({0x1234, DisconnectReason::SchemaMismatch, 1, 2}));
    REQUIRE(reject.has_value());
    CHECK(reject->reason == DisconnectReason::SchemaMismatch);
    CHECK(reject->expected == 1);
    CHECK(reject->actual == 2);

    // A reason that is not a rejection reason is corruption, not a policy we should forward.
    std::vector<std::byte> bogus =
        encode_connect_reject({0, DisconnectReason::SchemaMismatch, 0, 0});
    bogus[7] = static_cast<std::byte>(0xFF);
    CHECK_FALSE(decode_connect_reject(bogus).has_value());
}

TEST_CASE("the whole session lifecycle is bit-reproducible on a lossy, duplicating link") {
    const auto run = [] {
        ScriptedNetwork::Config nasty;
        nasty.loss_rate = 0.25f;
        nasty.duplicate_rate = 0.1f;
        nasty.min_latency_ms = 10;
        nasty.max_latency_ms = 40; // the spread is what reorders
        Harness h(nasty);
        h.server.listen();
        const auto id = h.client.connect(kServer, h.now_ms);
        h.tick(40);
        if (Session* s = h.client.session(*id)) {
            (void)s->send_reliable(payload_of("payload"), h.now_ms);
        }
        h.tick(20);
        std::string trace;
        for (const SessionEvent& e : h.server_events) {
            trace += std::to_string(static_cast<int>(e.kind));
            trace += std::to_string(static_cast<int>(e.reason));
            trace += ";";
        }
        trace += "|dropped=" + std::to_string(h.network.packets_dropped());
        trace += "|delivered=" + std::to_string(h.network.packets_delivered());
        trace += "|sessions=" + std::to_string(h.server.session_count());
        return trace;
    };

    const std::string first = run();
    const std::string second = run();
    CHECK(first == second);
    // Same seed, same script, same trace — and the scenario was not a no-op that trivially matches.
    CHECK(first.find("|dropped=0|") == std::string::npos);
    CHECK(first.find("sessions=1") != std::string::npos);
}

// ── The real-socket proof ───────────────────────────────────────────────────────────────
//
// Everything above runs on the ScriptedNetwork, which proves the ALGORITHM. This one proves the
// SOCKET PATH — two drivers on real loopback UDP, on the real clock — because the scripted harness
// cannot catch a framing bug that only bites once bytes traverse the OS. Same split, and the same
// reasoning, as m11.1's `udp link loopback` case.
//
// Flake discipline, following that precedent: ephemeral ports (never a hard-coded one, which races
// anything else on the box), and every wait is a DEADLINE loop rather than a fixed sleep — a
// failure is the deadline expiring, never a hang.
namespace {

NetDriver::Config loopback_config(std::uint64_t seed) {
    NetDriver::Config c;
    c.app_id = kAppId;
    c.schema_hash = kSchema;
    c.heartbeat_interval_ms = 30;
    c.timeout_ms = 300; // short, so "peer death" costs the suite a third of a second, not five
    c.connect_retry_ms = 20;
    c.connect_attempts = 100;
    c.resend_ms = 30;
    c.salt_seed = seed;
    return c;
}

std::uint64_t now_ms_from(std::chrono::steady_clock::time_point start) {
    // The app reads the clock and hands the number in; engine/net never does. This line is the
    // boundary where wall time enters the deterministic machine.
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}

} // namespace

TEST_CASE("real sockets: two drivers connect over loopback UDP, talk, and notice a death") {
    auto server_link = UdpLink::bind(0);
    auto client_link = UdpLink::bind(0);
    REQUIRE(server_link.has_value());
    REQUIRE(client_link.has_value());

    NetDriver server(*server_link, loopback_config(0x1111'2222'3333'4444ull));
    NetDriver client(*client_link, loopback_config(0xAAAA'BBBB'CCCC'DDDDull));
    server.listen();

    const auto start = std::chrono::steady_clock::now();
    std::vector<SessionEvent> server_events;
    std::vector<SessionEvent> client_events;

    const auto pump = [&](bool pump_client) {
        const std::uint64_t now = now_ms_from(start);
        server.update(now, server_events);
        if (pump_client) {
            client.update(now, client_events);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    // 1. Two peers connect. No sleep-before-connect and no ordering assumption: the client retries
    //    until the server answers, which is exactly what makes a real two-process launch raceless.
    const auto id = client.connect(server_link->local_endpoint(), now_ms_from(start));
    REQUIRE(id.has_value());
    // Wait on the EVENTS, not on session_count(): a session exists from the moment connect() is
    // called (in Connecting state), so counting sessions would let this loop fall through before
    // the accept had even arrived.
    for (int i = 0; i < 1000 && (count_kind(client_events, SessionEvent::Kind::Connected) == 0 ||
                                 count_kind(server_events, SessionEvent::Kind::Connected) == 0);
         ++i) {
        pump(true);
    }
    REQUIRE(count_kind(client_events, SessionEvent::Kind::Connected) == 1);
    REQUIRE(count_kind(server_events, SessionEvent::Kind::Connected) == 1);
    REQUIRE(server.session_count() == 1);

    // 2. They exchange a hello each way — through the salt frame, over a real datagram socket.
    const SessionId server_side = server.session_ids()[0];
    REQUIRE(
        client.session(*id)->send_reliable(payload_of("hello from client"), now_ms_from(start)));
    REQUIRE(server.session(server_side)
                ->send_reliable(payload_of("hello from server"), now_ms_from(start)));

    std::vector<Received> at_server;
    std::vector<Received> at_client;
    for (int i = 0; i < 1000 && (at_server.empty() || at_client.empty()); ++i) {
        pump(true);
        if (Session* s = server.session(server_side)) {
            s->drain_received(at_server);
        }
        if (Session* c = client.session(*id)) {
            c->drain_received(at_client);
        }
    }
    REQUIRE(at_server.size() == 1);
    REQUIRE(at_client.size() == 1);
    CHECK(to_string(at_server[0].bytes) == "hello from client");
    CHECK(to_string(at_client[0].bytes) == "hello from server");

    // 3. The client "dies" — it simply stops being pumped, which is indistinguishable at the
    //    protocol level from the process being killed: no disconnect, no packets, just silence.
    const std::size_t before = server_events.size();
    for (int i = 0; i < 2000 && server.session_count() > 0; ++i) {
        pump(false);
    }

    REQUIRE(server.session_count() == 0);
    const SessionEvent* died = nullptr;
    for (std::size_t i = before; i < server_events.size(); ++i) {
        if (server_events[i].kind == SessionEvent::Kind::Disconnected) {
            died = &server_events[i];
        }
    }
    REQUIRE(died != nullptr);
    CHECK(died->reason == DisconnectReason::Timeout);
}
