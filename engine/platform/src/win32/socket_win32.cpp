// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The Winsock backend for rime::platform TCP sockets — the Windows counterpart of
// src/posix/socket_posix.cpp. The shape mirrors the POSIX file; the differences are Winsock's: the
// library must be started once with WSAStartup, the handle type is SOCKET (not int), errors come
// from WSAGetLastError() (not errno), and the buffer lengths are `int`. Lifetime and the transfer
// loops are shared and live in src/socket.cpp. See rime/platform/socket.hpp for the contract.

#include "rime/platform/socket.hpp"

// winsock2.h must precede any windows.h; ws2tcpip.h adds getaddrinfo. We include no windows.h here.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <limits>
#include <mutex>
#include <string>

#include "rime/core/diagnostics/log.hpp"

namespace rime::platform {
namespace {

// Winsock needs an explicit per-process startup before any socket call. Do it lazily and exactly
// once. We intentionally never call WSACleanup: sockets are wanted for the whole process lifetime
// and the OS reclaims the library at exit — pairing cleanup with a lazy init isn't worth it.
void ensure_wsa_started() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        if (const int rc = ::WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
            RIME_ERROR("WSAStartup failed: {}", rc);
        }
    });
}

void set_low_latency(SOCKET s) {
    const BOOL yes = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

SOCKET as_sock(SocketHandle h) {
    return static_cast<SOCKET>(h);
}

// Winsock's send/recv take an `int` length; clamp so a huge span can't wrap to negative.
int clamp_len(std::size_t n) {
    constexpr std::size_t kMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    return static_cast<int>(n < kMax ? n : kMax);
}

} // namespace

// ── TcpSocket ─────────────────────────────────────────────────────────────────────────
std::optional<TcpSocket> TcpSocket::connect(std::string_view host, std::uint16_t port) {
    ensure_wsa_started();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string host_s(host);
    const std::string port_s = std::to_string(port);

    addrinfo* results = nullptr;
    if (const int gai = ::getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &results); gai != 0) {
        RIME_WARN("tcp connect {}:{}: getaddrinfo: WSA error {}", host, port, gai);
        return std::nullopt;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(results);

    if (s == INVALID_SOCKET) {
        RIME_WARN("tcp connect {}:{}: WSA error {}", host, port, ::WSAGetLastError());
        return std::nullopt;
    }
    set_low_latency(s);
    return TcpSocket(static_cast<SocketHandle>(s));
}

std::optional<std::size_t> TcpSocket::send(std::span<const std::byte> data) {
    const int n = ::send(
        as_sock(handle_), reinterpret_cast<const char*>(data.data()), clamp_len(data.size()), 0);
    if (n == SOCKET_ERROR) {
        RIME_WARN("tcp send: WSA error {}", ::WSAGetLastError());
        return std::nullopt;
    }
    return static_cast<std::size_t>(n);
}

std::optional<std::size_t> TcpSocket::recv(std::span<std::byte> buffer) {
    const int n = ::recv(
        as_sock(handle_), reinterpret_cast<char*>(buffer.data()), clamp_len(buffer.size()), 0);
    if (n == SOCKET_ERROR) {
        RIME_WARN("tcp recv: WSA error {}", ::WSAGetLastError());
        return std::nullopt;
    }
    return static_cast<std::size_t>(n); // 0 == peer closed cleanly (EOF)
}

void TcpSocket::close() noexcept {
    if (handle_ != kInvalidSocket) {
        ::closesocket(as_sock(handle_));
        handle_ = kInvalidSocket;
    }
}

// ── TcpListener ───────────────────────────────────────────────────────────────────────
std::optional<TcpListener> TcpListener::bind(std::uint16_t port, std::string_view host) {
    ensure_wsa_started();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string host_s(host);
    const std::string port_s = std::to_string(port);

    addrinfo* results = nullptr;
    const char* node = host_s.empty() ? nullptr : host_s.c_str();
    if (const int gai = ::getaddrinfo(node, port_s.c_str(), &hints, &results); gai != 0) {
        RIME_WARN("tcp bind {}:{}: getaddrinfo: WSA error {}", host, port, gai);
        return std::nullopt;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        const BOOL yes = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            ::listen(s, SOMAXCONN) == 0) {
            break;
        }
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(results);

    if (s == INVALID_SOCKET) {
        RIME_WARN("tcp bind {}:{}: WSA error {}", host, port, ::WSAGetLastError());
        return std::nullopt;
    }
    return TcpListener(static_cast<SocketHandle>(s));
}

std::optional<TcpSocket> TcpListener::accept() {
    const SOCKET s = ::accept(as_sock(handle_), nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        RIME_WARN("tcp accept: WSA error {}", ::WSAGetLastError());
        return std::nullopt;
    }
    set_low_latency(s);
    return TcpSocket(static_cast<SocketHandle>(s));
}

std::uint16_t TcpListener::local_port() const noexcept {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (::getsockname(as_sock(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
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
        ::closesocket(as_sock(handle_));
        handle_ = kInvalidSocket;
    }
}

} // namespace rime::platform
