// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The m11.1 proofs (ADR-0033 §3): the reliability layer delivers what it promises, demonstrated
// on the deterministic ScriptedNetwork — never on environment luck — plus one real-socket
// UdpLink loopback test, because the scripted harness proves the *algorithm* and something must
// prove the *socket path*. Every scripted scenario is pumped on a virtual clock, so "the same
// seed + the same script ⇒ the same trace" is itself one of the proofs. GPU-free; CI on all OSes.
//
// The pump shape: a virtual clock ticks 10 ms at a time; each tick delivers due packets, drains
// each link ONCE, routes datagrams by sender endpoint to the right channel (the driver pattern —
// one socket, N peers), and lets both channels resend/receive.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/net/reliable_channel.hpp"

using namespace rime::net;

namespace {

constexpr Endpoint kA{0x0A000001, 7000}; // two "machines" on the scripted network
constexpr Endpoint kB{0x0A000002, 7000};

std::vector<std::byte> payload_of(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

std::string to_string(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::vector<Datagram> drain(Link& link) {
    std::vector<Datagram> out;
    link.receive(out);
    return out;
}

// A two-endpoint conversation over one ScriptedNetwork, pumped on a virtual clock, with the
// driver pattern: the harness (not the channels) drains links and routes by endpoint.
struct Harness {
    explicit Harness(ScriptedNetwork::Config config, std::uint64_t seed = 42)
        : network(seed, config), a_link(network.add_node(kA)), b_link(network.add_node(kB)),
          a_to_b(a_link, kB, kResendMs), b_to_a(b_link, kA, kResendMs) {}

    static constexpr std::uint64_t kResendMs = 50;

    // Advance the virtual world by one 10 ms tick: deliver due packets, then each side drains
    // its link once, routes what arrived to the matching channel, and runs housekeeping.
    void tick() {
        now_ms += 10;
        network.advance_time(now_ms);
        for (const Datagram& d : drain(a_link)) {
            if (d.from == kB) {
                a_to_b.process_packet(d.bytes, from_b);
            }
        }
        for (const Datagram& d : drain(b_link)) {
            if (d.from == kA) {
                b_to_a.process_packet(d.bytes, from_a);
            }
        }
        a_to_b.update(now_ms);
        b_to_a.update(now_ms);
    }

    void ticks(int n) {
        for (int i = 0; i < n; ++i) {
            tick();
        }
    }

    std::size_t reliable_from_a() const {
        std::size_t n = 0;
        for (const Received& r : from_a) {
            n += r.channel == Channel::ReliableOrdered ? 1 : 0;
        }
        return n;
    }

    std::uint64_t now_ms = 0;
    ScriptedNetwork network;
    ScriptedLink& a_link;
    ScriptedLink& b_link;
    ReliableChannel a_to_b;       // a's view of the conversation (its sends land at b)
    ReliableChannel b_to_a;       // b's view
    std::vector<Received> from_a; // what b has received from a
    std::vector<Received> from_b; // what a has received from b
};

} // namespace

TEST_CASE("perfect link: reliable arrives in order, unreliable arrives") {
    Harness h({}); // no loss, no latency

    for (int i = 0; i < 5; ++i) {
        REQUIRE(h.a_to_b.send_reliable(payload_of("msg" + std::to_string(i)), h.now_ms));
    }
    REQUIRE(h.a_to_b.send_unreliable(payload_of("snap")));
    h.tick();

    REQUIRE(h.from_a.size() == 6);
    for (int i = 0; i < 5; ++i) {
        CHECK(h.from_a[i].channel == Channel::ReliableOrdered);
        CHECK(to_string(h.from_a[i].bytes) == "msg" + std::to_string(i));
    }
    CHECK(h.from_a[5].channel == Channel::UnreliableSequenced);

    // Acks flow home even though b has no data of its own (the AckOnly path).
    h.tick();
    CHECK(h.a_to_b.pending_count() == 0);
}

TEST_CASE("30% loss: every reliable message still arrives, exactly once, in order") {
    ScriptedNetwork::Config lossy;
    lossy.loss_rate = 0.30f;
    lossy.min_latency_ms = 0;
    lossy.max_latency_ms = 30; // jitter, so reordering happens too
    Harness h(lossy, /*seed=*/7);

    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(h.a_to_b.send_reliable(payload_of("event" + std::to_string(i)), h.now_ms));
        if (i % 10 == 9) {
            h.tick(); // interleave sends with progress, like a real loop
        }
    }
    h.ticks(400); // 4 s of virtual time: plenty of resend rounds

    // The proof's preconditions: the scenario really was lossy (otherwise it proves nothing).
    CHECK(h.network.packets_dropped() > 0);
    CHECK(h.a_to_b.packets_resent() > 0);

    // The contract: 100 messages, exactly once each, in exact send order.
    std::vector<std::string> reliable;
    for (const Received& r : h.from_a) {
        if (r.channel == Channel::ReliableOrdered) {
            reliable.push_back(to_string(r.bytes));
        }
    }
    REQUIRE(reliable.size() == kCount);
    for (int i = 0; i < kCount; ++i) {
        CHECK(reliable[i] == "event" + std::to_string(i));
    }
    // And once everything settled, the sender's resend queue is empty (all acked — including
    // packets recovered late, which the frontier-anchored ack reports permanently).
    CHECK(h.a_to_b.pending_count() == 0);
}

TEST_CASE("regression: a lost first packet is not mistaken for acked (the seq-0 deadlock)") {
    // Scenario: a's seq 0 is lost; b (having received NOTHING) sends traffic of its own. A
    // newest-anchored ack would read b's "newest = 0" as "seq 0 acked" and drop it from the
    // resend queue — the stream deadlocks at message 0. The frontier anchor makes ack=0 mean
    // "nothing delivered yet", which clears nothing.
    Harness h(
        [] {
            ScriptedNetwork::Config c;
            c.loss_rate = 1.0f; // drop EVERYTHING a sends — the packet never arrives
            return c;
        }(),
        /*seed=*/3);

    REQUIRE(h.a_to_b.send_reliable(payload_of("first"), h.now_ms));
    REQUIRE(h.b_to_a.send_unreliable(payload_of("b speaks first")));
    h.tick(); // a's packet is dropped; b's arrives at a

    // The bug's signature would be pending_count() == 0 here (seq 0 wrongly "acked" by a peer
    // that has received nothing).
    CHECK(h.a_to_b.pending_count() == 1);
    CHECK(h.b_to_a.messages_delivered() == 0); // b received nothing — and said so honestly

    // The message must instead stay queued and keep resending, so a healed link would deliver
    // it (delivery itself is proven by the 30%-loss case above).
    h.ticks(20);
    CHECK(h.a_to_b.pending_count() == 1);
    CHECK(h.a_to_b.packets_resent() > 0);
}

TEST_CASE(
    "regression: a packet recovered far behind the frontier still clears (late-recovery ack)") {
    // Scenario: one packet lands far behind newer ones (a huge latency spread reorders
    // aggressively). A newest-anchored backward bitfield can no longer report a packet recovered
    // more than 32 seqs behind the newest — it stays in the resend queue forever. The frontier
    // anchor reports it as seq < ack permanently.
    Harness h(
        [] {
            ScriptedNetwork::Config c;
            c.min_latency_ms = 0;
            c.max_latency_ms = 300; // huge spread: a packet can land after dozens of newer ones
            return c;
        }(),
        /*seed=*/11);

    for (int i = 0; i < 41; ++i) {
        REQUIRE(h.a_to_b.send_reliable(payload_of("m" + std::to_string(i)), h.now_ms));
    }
    h.ticks(200); // 2 s virtual: every packet, however reordered, lands

    // All 41 delivered, in order...
    REQUIRE(h.reliable_from_a() == 41);
    for (int i = 0; i < 41; ++i) {
        CHECK(to_string(h.from_a[i].bytes) == "m" + std::to_string(i));
    }
    // ...and the resend queue fully drains: no packet is stuck unacked behind the frontier.
    h.ticks(10); // let the final acks ride home
    CHECK(h.a_to_b.pending_count() == 0);
}

TEST_CASE("unreliable-sequenced: stale packets are dropped, never resent, latest wins") {
    ScriptedNetwork::Config jitter;
    jitter.min_latency_ms = 0;
    jitter.max_latency_ms = 40; // heavy jitter reorders aggressively, no loss
    Harness h(jitter, /*seed=*/99);

    constexpr int kSnaps = 50;
    for (int i = 0; i < kSnaps; ++i) {
        REQUIRE(h.a_to_b.send_unreliable(payload_of("snap" + std::to_string(i))));
    }
    h.ticks(20);

    std::vector<int> delivered;
    for (const Received& r : h.from_a) {
        if (r.channel == Channel::UnreliableSequenced) {
            delivered.push_back(std::stoi(to_string(r.bytes).substr(4)));
        }
    }
    REQUIRE_FALSE(delivered.empty());
    // Strictly increasing delivery: nothing stale ever surfaces, however reordered the wire was.
    for (std::size_t i = 1; i < delivered.size(); ++i) {
        CHECK(delivered[i] > delivered[i - 1]);
    }
    // The newest snapshot always lands (no loss on this link), so the final state converges.
    CHECK(delivered.back() == kSnaps - 1);
    // And nothing was resent: the sequenced channel has no resend path at all.
    CHECK(h.a_to_b.packets_resent() == 0);
}

TEST_CASE("duplicates are suppressed on both channels") {
    ScriptedNetwork::Config duppy;
    duppy.duplicate_rate = 0.50f; // half of all packets arrive twice
    Harness h(duppy, /*seed=*/5);

    for (int i = 0; i < 20; ++i) {
        REQUIRE(h.a_to_b.send_reliable(payload_of("r" + std::to_string(i)), h.now_ms));
        REQUIRE(h.a_to_b.send_unreliable(payload_of("u" + std::to_string(i))));
    }
    h.ticks(50);

    int reliable = 0, unreliable = 0;
    for (const Received& r : h.from_a) {
        if (r.channel == Channel::ReliableOrdered) {
            CHECK(to_string(r.bytes) == "r" + std::to_string(reliable));
            ++reliable;
        } else {
            ++unreliable;
        }
    }
    CHECK(reliable == 20);   // exactly once each, in order
    CHECK(unreliable <= 20); // sequenced: duplicates and stale ones dropped
    CHECK(unreliable > 0);
}

TEST_CASE("a dead peer backs the queue up to the backpressure cap") {
    Harness h({}); // a talks; NOTHING ever delivers (we never advance the network)
    // Send into the void: nothing is ever acked, so the queue must fill and then send_reliable
    // must REFUSE, not grow without bound (unbounded queues are how servers OOM).
    int accepted = 0;
    for (int i = 0; i < 400; ++i) {
        if (h.a_to_b.send_reliable(payload_of("x"), h.now_ms)) {
            ++accepted;
        }
    }
    CHECK(accepted == ReliableChannel::kMaxPending);
    CHECK(h.a_to_b.pending_count() == ReliableChannel::kMaxPending);
    // Housekeeping only — no deliveries, no acks: in-flight stays window-bounded and resends
    // keep trying (the peer may come back).
    for (int i = 0; i < 10; ++i) {
        h.now_ms += 10;
        h.a_to_b.update(h.now_ms);
    }
    CHECK(h.a_to_b.packets_resent() > 0);
    CHECK(h.a_to_b.pending_count() == ReliableChannel::kMaxPending); // nothing was acked
}

TEST_CASE("bit-reproducibility: same seed + same script ⇒ same trace, twice — and it delivers") {
    struct Outcome {
        std::string trace;
        std::size_t reliable_to_b; // a→b reliable messages delivered
        std::size_t reliable_to_a; // b→a reliable messages delivered
        std::size_t pending_ab;    // a's unacked queue at the end
        std::size_t pending_ba;    // b's unacked queue at the end
    };

    auto run_trace = [] {
        ScriptedNetwork::Config rough;
        rough.loss_rate = 0.25f;
        rough.duplicate_rate = 0.10f;
        rough.min_latency_ms = 0;
        rough.max_latency_ms = 50;
        Harness h(rough, /*seed=*/1234);
        for (int i = 0; i < 60; ++i) {
            h.a_to_b.send_reliable(payload_of("m" + std::to_string(i)), h.now_ms);
            h.a_to_b.send_unreliable(payload_of("s" + std::to_string(i)));
            h.b_to_a.send_reliable(payload_of("b" + std::to_string(i)), h.now_ms);
            h.tick();
        }
        h.ticks(320); // settle: every resend lands, every ack rides home
        // The trace is everything the run produced: deliveries (both directions) + counters.
        Outcome out;
        out.reliable_to_b = h.reliable_from_a();
        out.reliable_to_a = 0;
        for (const Received& r : h.from_a) {
            out.trace += std::to_string(static_cast<int>(r.channel)) + to_string(r.bytes) + ";";
        }
        out.trace += "|";
        for (const Received& r : h.from_b) {
            out.trace += std::to_string(static_cast<int>(r.channel)) + to_string(r.bytes) + ";";
            out.reliable_to_a += r.channel == Channel::ReliableOrdered ? 1 : 0;
        }
        out.trace += "|" + std::to_string(h.network.packets_sent()) + "," +
                     std::to_string(h.network.packets_dropped()) + "," +
                     std::to_string(h.a_to_b.packets_resent());
        out.pending_ab = h.a_to_b.pending_count();
        out.pending_ba = h.b_to_a.pending_count();
        return out;
    };

    const Outcome first = run_trace();
    const Outcome second = run_trace();
    CHECK(first.trace == second.trace); // the harness is a proof instrument, not a slot machine
    CHECK_FALSE(first.trace.empty());

    // And determinism is not hiding a broken channel: the run actually DELIVERED, both ways.
    CHECK(first.reliable_to_b == 60);
    CHECK(first.reliable_to_a == 60);
    CHECK(first.pending_ab == 0);
    CHECK(first.pending_ba == 0);
}

TEST_CASE("udp link loopback: the channel runs over a real socket, not just the harness") {
    // The scripted network proves the algorithm; this proves the m11.1 socket path the ADR
    // actually promised — two UdpLinks on loopback carrying a channel conversation.
    auto link_a = UdpLink::bind(0);
    auto link_b = UdpLink::bind(0);
    REQUIRE(link_a.has_value());
    REQUIRE(link_b.has_value());

    ReliableChannel a_to_b(*link_a, link_b->local_endpoint(), /*resend_ms=*/20);
    ReliableChannel b_to_a(*link_b, link_a->local_endpoint(), /*resend_ms=*/20);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(a_to_b.send_reliable(payload_of("wire" + std::to_string(i)), 0));
    }
    REQUIRE(a_to_b.send_unreliable(payload_of("wire-snap")));

    // Pump on wall time (this is the real-socket test): drain + route + update until the
    // conversation completes or we time out — a failure is the deadline, never a hang.
    std::vector<Received> from_a, from_b;
    const auto wall_now = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
    const auto deadline = wall_now() + 5000;
    // Pump until the conversation completes AND the acks have ridden home (the deliveries finish
    // before the trailing AckOnly packets arrive — settle for those too).
    while ((from_a.size() < 11 || a_to_b.pending_count() > 0) && wall_now() < deadline) {
        for (const Datagram& d : drain(*link_a)) {
            a_to_b.process_packet(d.bytes, from_b);
        }
        for (const Datagram& d : drain(*link_b)) {
            b_to_a.process_packet(d.bytes, from_a);
        }
        a_to_b.update(wall_now());
        b_to_a.update(wall_now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(from_a.size() == 11);
    for (int i = 0; i < 10; ++i) {
        CHECK(from_a[i].channel == Channel::ReliableOrdered);
        CHECK(to_string(from_a[i].bytes) == "wire" + std::to_string(i));
    }
    CHECK(from_a[10].channel == Channel::UnreliableSequenced);
    // Acks came home over the real wire too.
    CHECK(a_to_b.pending_count() == 0);
}

// Two kinds of unreliable message sent in the same tick used to race each other, and the loser was
// discarded before the application ever saw its bytes — not lost on the wire, dropped on arrival
// for being "stale" when it was nothing of the sort. With one sender that never mattered; the
// moment a second kind exists it is a coin flip every tick.
//
// The proof runs the same traffic twice over the same seed and link, once sharing a stream and once
// on two, and contrasts them. Written this way deliberately: an absolute threshold on the
// two-stream run alone would pass just as happily against the unfixed code if the seed happened to
// be kind, whereas the shared-stream half pins what the hazard actually costs.
TEST_CASE("unreliable streams: two kinds sent in the same tick do not evict each other") {
    constexpr int kRounds = 40;

    const auto run = [](std::uint8_t stream_a, std::uint8_t stream_b) {
        ScriptedNetwork::Config jitter;
        jitter.min_latency_ms = 0;
        jitter.max_latency_ms = 40; // wide enough that same-tick siblings routinely swap order
        Harness h(jitter, /*seed=*/7);
        for (int i = 0; i < kRounds; ++i) {
            // The shape that bit us: two sends, back to back, every tick.
            REQUIRE(h.a_to_b.send_unreliable(payload_of("a" + std::to_string(i)), stream_a));
            REQUIRE(h.a_to_b.send_unreliable(payload_of("b" + std::to_string(i)), stream_b));
            h.ticks(1);
        }
        h.ticks(20); // let the tail land
        struct Result {
            int a = 0, b = 0;
            std::uint64_t superseded = 0;
        } r;
        for (const Received& m : h.from_a) {
            if (m.channel == Channel::UnreliableSequenced) {
                (to_string(m.bytes)[0] == 'a' ? r.a : r.b)++;
            }
        }
        r.superseded = h.b_to_a.unreliable_superseded();
        return r;
    };

    // THE HAZARD, pinned. One stream: the two series are one supersedes-chain, so roughly every
    // other message is killed by its own sibling on a link that loses nothing at all.
    const auto shared = run(0, 0);
    MESSAGE("shared a=" << shared.a << " b=" << shared.b << " sup=" << shared.superseded);
    CHECK(shared.superseded > 0); // non-vacuous: the link really did reorder same-tick siblings

    // THE FIX. Independent streams, same seed, same link, same traffic: each series is now only
    // ever superseded by its own newer messages, so both arrive very nearly whole.
    const auto split = run(0, 1);
    MESSAGE("split a=" << split.a << " b=" << split.b << " sup=" << split.superseded);
    // Every one of these reads IDENTICAL to the shared run against the unfixed channel, because
    // there the stream argument is ignored and the two runs are the same run. The contrast is the
    // proof; an absolute threshold on the split run alone would not be one.
    CHECK(split.a > shared.a);
    CHECK(split.b > shared.b);
    CHECK(split.a + split.b > shared.a + shared.b);
    CHECK(split.superseded < shared.superseded);

    // What survives is still bounded by honest within-series supersession: a message delayed past
    // its own successor is late state, and late state is garbage by the channel's contract. The
    // fix removes the sibling collisions, not the jitter — measured 47/80 shared vs 61/80 split.
    CHECK(split.a + split.b < 2 * kRounds);
}

// The stream id is untrusted input. A peer that passed the handshake cannot name a stream this
// build lacks, so one that does is lying or corrupt — count it and drop it, never index with it.
TEST_CASE("unreliable streams: an out-of-range stream is refused, not clamped") {
    ScriptedNetwork::Config perfect;
    Harness h(perfect, /*seed=*/11);

    // The sender refuses locally rather than emitting a packet no peer could accept. Refusing
    // beats clamping: a clamp would silently merge two spaces the caller believed were separate,
    // which is the exact bug the stream id exists to prevent.
    CHECK_FALSE(h.a_to_b.send_unreliable(payload_of("nope"), ReliableChannel::kUnreliableStreams));
    CHECK_FALSE(h.a_to_b.send_unreliable(payload_of("nope"), 255));

    // And a hand-forged packet naming a bad stream is counted and dropped on the receiving side.
    // Hand-built because an honest sender cannot produce one — the same "unreachable, so construct
    // it directly" standard the refused-part branch is held to.
    std::vector<std::byte> packet;
    rime::core::ByteWriter forged(packet);
    forged.u8(static_cast<std::uint8_t>(Channel::UnreliableSequenced));
    forged.u32(0);  // seq
    forged.u32(0);  // ack
    forged.u32(0);  // ack_bits
    forged.u8(200); // stream — well past kUnreliableStreams
    forged.bytes(payload_of("forged"));

    std::vector<Received> out;
    const std::size_t delivered = h.b_to_a.process_packet(packet, out);
    CHECK(delivered == 0);
    CHECK(out.empty());
    CHECK(h.b_to_a.unreliable_bad_stream() == 1);
}
