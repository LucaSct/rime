// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Blocking TCP sockets — the transport primitive the graphics-streaming track (Track S / S0) is
// built on, and the seed of the future `engine/net` module (M11 grows from here).
//
// This is a *seam*, the same discipline as windowing (ADR-0006): the public interface names no OS
// type, and each OS's BSD-sockets / Winsock implementation lives in a backend under src/ that is
// compiled only on its platform. The one place the two worlds must meet is the socket handle
// itself — a POSIX file descriptor is an `int`, a Win32 `SOCKET` is a `UINT_PTR` — and both fit in
// an `intptr_t`, so we carry the handle as one and let the backend reinterpret it. No
// `<winsock2.h>` or `<sys/socket.h>` leaks upward.
//
// Scope for S0 is deliberately tiny: *blocking* TCP only. That is enough to see and control a
// headless engine over a LAN, and its bottlenecks (frame readback + encode) dominate any blocking
// stall — which we measure rather than pre-optimize (house rule). UDP/QUIC, TLS, and non-blocking /
// async I/O are explicitly later (S1–S2); the seam is shaped so they arrive as additions.
namespace rime::platform {

// An OS socket handle with no OS type in sight (see the file header). The backends cast it to `int`
// (POSIX) or `SOCKET` (Win32). `kInvalidSocket` is -1, which is exactly what a POSIX error return
// and Win32's `INVALID_SOCKET` (an all-ones `UINT_PTR`) both become when narrowed to `intptr_t`.
using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = -1;

// A connected TCP endpoint. Move-only: it owns the OS handle and closes it on destruction (RAII),
// so a socket cannot be accidentally copied into two owners that would double-close it.
class TcpSocket {
public:
    // Connect to host:port and block until the connection is established. `host` may be a name or a
    // literal IPv4/IPv6 address (resolved with getaddrinfo, so IPv6 works for free). Returns
    // nullopt on failure (the reason is logged). TCP_NODELAY is set on success: a
    // streaming/interactive client wants each write on the wire *now*, not coalesced by Nagle's
    // algorithm.
    [[nodiscard]] static std::optional<TcpSocket> connect(std::string_view host,
                                                          std::uint16_t port);

    TcpSocket() = default; // an unconnected socket (is_open() == false)
    ~TcpSocket();
    TcpSocket(TcpSocket&&) noexcept;
    TcpSocket& operator=(TcpSocket&&) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }

    // Send once. Returns the number of bytes actually written, which for TCP may be **less** than
    // data.size() (the kernel send buffer decides); nullopt on error. Use send_all when you need
    // every byte out.
    [[nodiscard]] std::optional<std::size_t> send(std::span<const std::byte> data);

    // Receive once into `buffer`. Returns bytes read: **>0** = that many bytes, **0** = the peer
    // closed the connection cleanly (EOF), **nullopt** = error. A short read is normal for TCP.
    [[nodiscard]] std::optional<std::size_t> recv(std::span<std::byte> buffer);

    // Loop the primitives until the whole span is transferred — the shape a length-prefixed frame
    // protocol (S0.4) needs. send_all returns false on any error. recv_exact returns false on error
    // *or* on an early EOF (peer closed mid-message), so a truncated frame is a clean failure, not
    // a silent short read.
    [[nodiscard]] bool send_all(std::span<const std::byte> data);
    [[nodiscard]] bool recv_exact(std::span<std::byte> buffer);

    // Close now; also run by the destructor. Idempotent (a no-op on an already-closed socket).
    void close() noexcept;

    // The raw handle, for tests and diagnostics only. The socket still owns it — do not close it.
    [[nodiscard]] SocketHandle native_handle() const noexcept { return handle_; }

private:
    explicit TcpSocket(SocketHandle h) noexcept : handle_(h) {}
    friend class TcpListener; // accept() mints connected TcpSockets

    SocketHandle handle_ = kInvalidSocket;
};

// A listening TCP socket: bind a port, then accept() connections from it. Move-only / RAII, exactly
// like TcpSocket.
class TcpListener {
public:
    // Bind and listen on host:port. `host` defaults to loopback (127.0.0.1) — the S0 dev-stream is
    // LAN/loopback; pass "0.0.0.0" to accept on every interface. A `port` of 0 asks the OS for a
    // free ("ephemeral") port, which you read back with local_port() — this is how the loopback
    // test avoids racing a hard-coded port. SO_REUSEADDR is set so a quick restart is not blocked
    // by the previous socket lingering in TIME_WAIT. Returns nullopt on failure (logged).
    [[nodiscard]] static std::optional<TcpListener> bind(std::uint16_t port,
                                                         std::string_view host = "127.0.0.1");

    TcpListener() = default;
    ~TcpListener();
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }

    // Block until a client connects and return the accepted connection; nullopt on error.
    [[nodiscard]] std::optional<TcpSocket> accept();

    // The port actually bound (resolve it after binding to port 0). 0 if closed or unknown.
    [[nodiscard]] std::uint16_t local_port() const noexcept;

    void close() noexcept;

    [[nodiscard]] SocketHandle native_handle() const noexcept { return handle_; }

private:
    explicit TcpListener(SocketHandle h) noexcept : handle_(h) {}

    SocketHandle handle_ = kInvalidSocket;
};

} // namespace rime::platform
