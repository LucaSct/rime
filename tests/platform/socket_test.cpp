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
