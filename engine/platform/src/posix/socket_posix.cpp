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
#include <sys/un.h> // sockaddr_un (AF_UNIX, S1.4)
#include <unistd.h> // close, unlink

#include <cerrno>
#include <cstddef> // offsetof
#include <cstring> // std::strerror, std::memcpy
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

// Fill a sockaddr_un for `path` (S1.4). AF_UNIX paths are bounded by sizeof(sun_path) (~108 on
// Linux, 104 on macOS); an over-long path cannot be represented, so we reject it rather than
// silently truncate — a truncated path would address the wrong node. Returns the addrlen (using the
// exact length, not the whole struct, so it works on both OSes' conventions), or 0 if too long.
socklen_t fill_sun(sockaddr_un& addr, std::string_view path) {
    addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return 0;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    addr.sun_path[path.size()] = '\0';
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
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

// ── LocalSocket / LocalListener (S1.4, AF_UNIX) ─────────────────────────────────────────
// A connected Unix-domain socket is an ordinary stream socket, so send/recv/close are identical to
// TcpSocket's — only the address (a path, not host:port) and the listener's unlink differ.
std::optional<LocalSocket> LocalSocket::connect(std::string_view path) {
    sockaddr_un addr{};
    const socklen_t len = fill_sun(addr, path);
    if (len == 0) {
        RIME_WARN("local connect: path too long ({} bytes, max {})",
                  path.size(),
                  sizeof(addr.sun_path) - 1);
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        RIME_WARN("local connect {}: socket: {}", path, std::strerror(errno));
        return std::nullopt;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        // ENOENT / ECONNREFUSED here is the common "server not up yet" — a normal, retryable case.
        RIME_WARN("local connect {}: {}", path, std::strerror(errno));
        ::close(fd);
        return std::nullopt;
    }
    suppress_sigpipe(fd); // no TCP_NODELAY: Nagle is a TCP notion, irrelevant on AF_UNIX
    return LocalSocket(static_cast<SocketHandle>(fd));
}

std::optional<std::size_t> LocalSocket::send(std::span<const std::byte> data) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const ssize_t n = ::send(as_fd(handle_), data.data(), data.size(), flags);
    if (n < 0) {
        RIME_WARN("local send: {}", std::strerror(errno));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n);
}

std::optional<std::size_t> LocalSocket::recv(std::span<std::byte> buffer) {
    const ssize_t n = ::recv(as_fd(handle_), buffer.data(), buffer.size(), 0);
    if (n < 0) {
        RIME_WARN("local recv: {}", std::strerror(errno));
        return std::nullopt;
    }
    return static_cast<std::size_t>(n); // 0 == peer closed cleanly (EOF)
}

void LocalSocket::close() noexcept {
    if (handle_ != kInvalidSocket) {
        ::close(as_fd(handle_));
        handle_ = kInvalidSocket;
    }
}

std::optional<LocalListener> LocalListener::bind(std::string_view path) {
    sockaddr_un addr{};
    const socklen_t len = fill_sun(addr, path);
    if (len == 0) {
        RIME_WARN(
            "local bind: path too long ({} bytes, max {})", path.size(), sizeof(addr.sun_path) - 1);
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        RIME_WARN("local bind {}: socket: {}", path, std::strerror(errno));
        return std::nullopt;
    }
    // AF_UNIX has no SO_REUSEADDR for the path: a stale node (a crashed server, or our own prior
    // run) makes bind fail with EADDRINUSE. Unlink it first — same-host, single-owner, so safe.
    const std::string path_s(path);
    ::unlink(path_s.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(fd, SOMAXCONN) != 0) {
        RIME_WARN("local bind {}: {}", path, std::strerror(errno));
        ::close(fd);
        return std::nullopt;
    }
    return LocalListener(static_cast<SocketHandle>(fd), path_s);
}

std::optional<LocalSocket> LocalListener::accept() {
    const int fd = ::accept(as_fd(handle_), nullptr, nullptr);
    if (fd < 0) {
        RIME_WARN("local accept: {}", std::strerror(errno));
        return std::nullopt;
    }
    suppress_sigpipe(fd);
    return LocalSocket(static_cast<SocketHandle>(fd));
}

void LocalListener::close() noexcept {
    if (handle_ != kInvalidSocket) {
        ::close(as_fd(handle_));
        handle_ = kInvalidSocket;
    }
    if (!path_.empty()) {
        ::unlink(
            path_.c_str()); // remove the socket file so a restart isn't blocked by a stale node
        path_.clear();
    }
}

} // namespace rime::platform
