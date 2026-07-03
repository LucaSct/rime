// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The OS-agnostic half of the TCP sockets seam: lifetime (move + RAII close) and the transfer
// loops. The primitives these build on — connect/bind/accept/send/recv/close/local_port — are the
// genuinely per-OS part and live in src/posix/ (BSD sockets, Linux + macOS) and src/win32/
// (Winsock). Because a stolen/moved handle is just an integer, everything here is portable and is
// written once. See rime/platform/socket.hpp.

#include "rime/platform/socket.hpp"

namespace rime::platform {

// ── TcpSocket lifetime ────────────────────────────────────────────────────────────────
TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocket; // moved-from socket owns nothing
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close(); // release whatever we currently hold before taking the new handle
        handle_ = other.handle_;
        other.handle_ = kInvalidSocket;
    }
    return *this;
}

bool TcpSocket::send_all(std::span<const std::byte> data) {
    while (!data.empty()) {
        const std::optional<std::size_t> n = send(data);
        if (!n || *n == 0) {
            return false; // error, or a 0-length write we can't make progress on
        }
        data = data.subspan(*n);
    }
    return true;
}

bool TcpSocket::recv_exact(std::span<std::byte> buffer) {
    while (!buffer.empty()) {
        const std::optional<std::size_t> n = recv(buffer);
        if (!n || *n == 0) {
            return false; // error, or the peer closed before the whole message arrived
        }
        buffer = buffer.subspan(*n);
    }
    return true;
}

// ── TcpListener lifetime ──────────────────────────────────────────────────────────────
TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocket;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidSocket;
    }
    return *this;
}

} // namespace rime::platform
