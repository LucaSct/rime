// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Loopback tests for the platform TCP sockets seam (S0.1). Everything runs over 127.0.0.1 with an
// OS-assigned ephemeral port, so the suite is GPU-free and self-contained and runs in CI on all
// three OSes. doctest's assertion macros are not thread-safe, so the client thread only *records*
// its outcome into a plain struct; join() establishes the happens-before, and every CHECK/REQUIRE
// is evaluated on the main thread afterwards.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "rime/platform/socket.hpp"

using namespace rime::platform;

namespace {
std::span<const std::byte> bytes_of(const std::string& s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

std::string to_string(std::span<const std::byte> b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

TEST_CASE("tcp loopback: send/recv round-trip both directions") {
    auto listener = TcpListener::bind(0); // ephemeral loopback port
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();
    REQUIRE(port != 0);

    const std::string request = "hello from the frost";
    const std::string reply = "PONG!";

    struct ClientOutcome {
        bool connected = false;
        bool sent = false;
        bool got_reply = false;
        std::string received;
    } out;

    std::thread client([&] {
        auto sock = TcpSocket::connect("127.0.0.1", port);
        if (!sock) {
            return;
        }
        out.connected = true;
        out.sent = sock->send_all(bytes_of(request));
        std::vector<std::byte> buf(reply.size());
        if (sock->recv_exact(buf)) {
            out.got_reply = true;
            out.received = to_string(buf);
        }
    });

    auto conn = listener->accept();
    REQUIRE(conn.has_value());

    std::vector<std::byte> buf(request.size());
    REQUIRE(conn->recv_exact(buf));
    CHECK(to_string(buf) == request);
    REQUIRE(conn->send_all(bytes_of(reply)));

    client.join(); // publishes `out`
    CHECK(out.connected);
    CHECK(out.sent);
    CHECK(out.got_reply);
    CHECK(out.received == reply);
}

TEST_CASE("tcp loopback: recv returns 0 (clean EOF) when the peer closes") {
    auto listener = TcpListener::bind(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();

    std::thread client([&] {
        auto sock = TcpSocket::connect("127.0.0.1", port);
        // Drop it immediately: the socket closes at end of scope, which the server sees as EOF.
    });

    auto conn = listener->accept();
    REQUIRE(conn.has_value());
    std::array<std::byte, 16> buf{};
    const auto n = conn->recv(buf); // blocks until the peer closes
    client.join();

    REQUIRE(n.has_value()); // EOF is not an error...
    CHECK(*n == 0);         // ...it is a zero-length read
}

TEST_CASE("tcp connect to a port with no listener fails cleanly") {
    // Port 1 is privileged and effectively never listened on, so a loopback connect is refused
    // fast. The point is that failure is a clean nullopt, not a crash or a hang.
    auto sock = TcpSocket::connect("127.0.0.1", 1);
    CHECK_FALSE(sock.has_value());
}

TEST_CASE("tcp sockets are movable, single-owner") {
    auto listener = TcpListener::bind(0);
    REQUIRE(listener.has_value());
    REQUIRE(listener->is_open());

    TcpListener moved = std::move(*listener);
    CHECK(moved.is_open());           // the handle transferred...
    CHECK_FALSE(listener->is_open()); // ...leaving the source empty (no double-close)
}

// ── UDP (m11.1, ADR-0033) ─────────────────────────────────────────────────────────────
// Datagrams are connectionless, so no accept/connect dance and no threads: bind two loopback
// sockets on ephemeral ports and talk both ways. recv_from is non-blocking, so a bounded spin
// stands in for "wait for the packet" — loopback delivery is effectively immediate, and a
// failure shows up as the spin expiring (a test failure), never a hang.

TEST_CASE("udp loopback: datagram round-trip both directions") {
    auto a = UdpSocket::bind(0);
    auto b = UdpSocket::bind(0);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    const std::uint16_t port_a = a->local_port();
    const std::uint16_t port_b = b->local_port();
    REQUIRE(port_a != 0);
    REQUIRE(port_b != 0);

    const auto to_a = Endpoint::from_string("127.0.0.1", port_a);
    const auto to_b = Endpoint::from_string("127.0.0.1", port_b);
    REQUIRE(to_a.has_value());
    REQUIRE(to_b.has_value());

    const std::string ping = "frost datagram";
    const std::string pong = "pong";

    // a → b.
    REQUIRE(a->send_to(*to_b, bytes_of(ping)).has_value());
    std::array<std::byte, 64> buf{};
    Endpoint from{};
    std::optional<std::size_t> n;
    for (int spin = 0; spin < 100000 && !n; ++spin) {
        n = b->recv_from(from, buf);
    }
    REQUIRE(n.has_value());
    CHECK(to_string(std::span(buf.data(), *n)) == ping);
    CHECK(from.address == to_a->address); // the reply address is the sender's
    CHECK(from.port == port_a);

    // b → a, replying to the address the datagram came from (the server pattern).
    REQUIRE(b->send_to(from, bytes_of(pong)).has_value());
    n.reset();
    for (int spin = 0; spin < 100000 && !n; ++spin) {
        n = a->recv_from(from, buf);
    }
    REQUIRE(n.has_value());
    CHECK(to_string(std::span(buf.data(), *n)) == pong);
    CHECK(from.port == port_b);
}

TEST_CASE("udp recv_from on an idle socket reports nothing, not an error") {
    auto sock = UdpSocket::bind(0);
    REQUIRE(sock.has_value());
    std::array<std::byte, 16> buf{};
    Endpoint from{};
    // The whole point of the non-blocking poll: an empty socket is a nullopt, immediately.
    CHECK_FALSE(sock->recv_from(from, buf).has_value());
}

TEST_CASE("udp socket is movable, single-owner") {
    auto sock = UdpSocket::bind(0);
    REQUIRE(sock.has_value());
    const std::uint16_t port = sock->local_port();

    UdpSocket moved = std::move(*sock);
    CHECK(moved.is_open());
    CHECK_FALSE(sock->is_open());      // moved-from owns nothing (no double-close)
    CHECK(moved.local_port() == port); // the binding moved with the handle
}

TEST_CASE("endpoint parsing: dotted quads round-trip, junk refused") {
    const auto loop = Endpoint::from_string("127.0.0.1", 7777);
    REQUIRE(loop.has_value());
    CHECK(loop->address == 0x7F000001);
    CHECK(loop->port == 7777);
    CHECK(loop->address_string() == "127.0.0.1");

    CHECK(Endpoint::from_string("0.0.0.0", 0)->address == 0);
    CHECK(Endpoint::from_string("255.255.255.255", 1)->address == 0xFFFFFFFF);

    CHECK_FALSE(Endpoint::from_string("127.0.0", 1).has_value());     // too few octets
    CHECK_FALSE(Endpoint::from_string("127.0.0.1.5", 1).has_value()); // too many
    CHECK_FALSE(Endpoint::from_string("127.0.0.256", 1).has_value()); // octet overflow
    CHECK_FALSE(Endpoint::from_string("localhost", 1).has_value());   // names don't parse
    CHECK_FALSE(Endpoint::from_string("1.2.3.", 1).has_value());      // trailing dot, empty octet
    CHECK_FALSE(Endpoint::from_string("1..2.3", 1).has_value());      // empty middle octet
    CHECK_FALSE(Endpoint::from_string(".1.2.3", 1).has_value());      // empty first octet
}
