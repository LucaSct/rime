// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The BSD-sockets backend for rime::platform TCP sockets. Compiled on Linux AND macOS: unlike
// windowing (Cocoa vs Xlib vs Wayland), the socket API is genuinely shared POSIX, so both OSes use
// this one file. Only the OS-specific *primitives* live here; lifetime and the transfer loops are
// in the portable src/socket.cpp. See rime/platform/socket.hpp for the contract.

#include <arpa/inet.h>
#include <netdb.h>       // getaddrinfo
#include <netinet/in.h>  // sockaddr_in / sockaddr_in6
#include <netinet/tcp.h> // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h> // close

#include <cerrno>
#include <cstring> // std::strerror
#include <string>

#include "rime/core/diagnostics/log.hpp"
#include "rime/platform/socket.hpp"

namespace rime::platform {
namespace {

// A streaming/interactive connection wants each write on the wire immediately, so disable Nagle.
// Best-effort: if it fails the connection still works, just a touch less responsively.
void set_low_latency(int fd) {
    const int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// Writing to a socket whose peer has gone away raises SIGPIPE by default, which would kill the
// process. On Linux we pass MSG_NOSIGNAL per-send (below); macOS has no such flag, so we ask the
// socket to never raise the signal via SO_NOSIGPIPE and get EPIPE as an ordinary error instead.
void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
    (void)fd; // Linux path: handled per-send with MSG_NOSIGNAL
#endif
}

int as_fd(SocketHandle h) {
    return static_cast<int>(h);
}

} // namespace

// ── TcpSocket ─────────────────────────────────────────────────────────────────────────
std::optional<TcpSocket> TcpSocket::connect(std::string_view host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6, whichever the name resolves to
    hints.ai_socktype = SOCK_STREAM; // TCP
    const std::string host_s(host);
    const std::string port_s = std::to_string(port);

    addrinfo* results = nullptr;
    if (const int gai = ::getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &results); gai != 0) {
        RIME_WARN("tcp connect {}:{}: getaddrinfo: {}", host, port, ::gai_strerror(gai));
        return std::nullopt;
    }

    // Try each resolved address until one connects (e.g. IPv6 then IPv4).
    int fd = -1;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break; // connected
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);

    if (fd < 0) {
        RIME_WARN("tcp connect {}:{}: {}", host, port, std::strerror(errno));
        return std::nullopt;
    }
    set_low_latency(fd);
    suppress_sigpipe(fd);
    return TcpSocket(static_cast<SocketHandle>(fd));
}

std::optional<std::size_t> TcpSocket::send(std::span<const std::byte> data) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL; // Linux: don't raise SIGPIPE on a dead peer (macOS used SO_NOSIGPIPE)
#endif
    const ssize_t n = ::send(as_fd(handle_), data.data(), data.size(), flags);
    if (n < 0) {
        RIME_WARN("tcp send: {}", std::strerror(errno));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n);
}

std::optional<std::size_t> TcpSocket::recv(std::span<std::byte> buffer) {
    const ssize_t n = ::recv(as_fd(handle_), buffer.data(), buffer.size(), 0);
    if (n < 0) {
        RIME_WARN("tcp recv: {}", std::strerror(errno));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n); // 0 == peer closed cleanly (EOF)
}

void TcpSocket::close() noexcept {
    if (handle_ != kInvalidSocket) {
        ::close(as_fd(handle_));
        handle_ = kInvalidSocket;
    }
}

// ── TcpListener ───────────────────────────────────────────────────────────────────────
std::optional<TcpListener> TcpListener::bind(std::uint16_t port, std::string_view host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // for a wildcard host; harmless with an explicit one
    const std::string host_s(host);
    const std::string port_s = std::to_string(port);

    addrinfo* results = nullptr;
    const char* node = host_s.empty() ? nullptr : host_s.c_str();
    if (const int gai = ::getaddrinfo(node, port_s.c_str(), &hints, &results); gai != 0) {
        RIME_WARN("tcp bind {}:{}: getaddrinfo: {}", host, port, ::gai_strerror(gai));
        return std::nullopt;
    }

    int fd = -1;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        // Reuse the address so a restart isn't blocked by the old socket's TIME_WAIT.
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            break; // bound + listening
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);

    if (fd < 0) {
        RIME_WARN("tcp bind {}:{}: {}", host, port, std::strerror(errno));
        return std::nullopt;
    }
    return TcpListener(static_cast<SocketHandle>(fd));
}

std::optional<TcpSocket> TcpListener::accept() {
    const int fd = ::accept(as_fd(handle_), nullptr, nullptr);
    if (fd < 0) {
        RIME_WARN("tcp accept: {}", std::strerror(errno));
        return std::nullopt;
    }
    set_low_latency(fd);
    suppress_sigpipe(fd);
    return TcpSocket(static_cast<SocketHandle>(fd));
}

std::uint16_t TcpListener::local_port() const noexcept {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(as_fd(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    }
    return 0;
}

void TcpListener::close() noexcept {
    if (handle_ != kInvalidSocket) {
        ::close(as_fd(handle_));
        handle_ = kInvalidSocket;
    }
}

} // namespace rime::platform
