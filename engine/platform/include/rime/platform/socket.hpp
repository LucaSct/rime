// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

// ── Transport genericity (S1.4) ─────────────────────────────────────────────────────────
// A connected, blocking, byte-ordered stream — the minimal surface a length-prefixed protocol
// needs (send_all / recv_exact), abstracted over *which* transport carries it. A consumer can hold
// a `ByteStream` and not care whether a LAN `TcpSocket` or a same-host `LocalSocket` is underneath:
// the M9 editor viewport rides a local stream, remote play a TCP one, over the *same* protocol
// (ADR-0016). This is the seam s1.4 adds so `ProtocolConnection` stops hard-owning a TcpSocket.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    [[nodiscard]] virtual bool send_all(std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual bool recv_exact(std::span<std::byte> buffer) = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    virtual void close() noexcept = 0;
};

// Type-erase a concrete socket (TcpSocket, LocalSocket — anything with send_all/recv_exact/is_open/
// close) into a ByteStream, so a consumer holds `unique_ptr<ByteStream>` without a transport
// switch. It simply forwards — zero behaviour change on either transport.
template <class Socket> class SocketByteStream final : public ByteStream {
public:
    explicit SocketByteStream(Socket socket) noexcept : socket_(std::move(socket)) {}

    [[nodiscard]] bool send_all(std::span<const std::byte> d) override {
        return socket_.send_all(d);
    }

    [[nodiscard]] bool recv_exact(std::span<std::byte> b) override { return socket_.recv_exact(b); }

    [[nodiscard]] bool is_open() const noexcept override { return socket_.is_open(); }

    void close() noexcept override { socket_.close(); }

    [[nodiscard]] Socket& socket() noexcept { return socket_; }

private:
    Socket socket_;
};

// A connected **local** (same-host) endpoint: a Unix-domain socket, addressed by a filesystem path
// rather than host:port. Its send/recv are ordinary stream-socket operations — identical to TCP's —
// so it satisfies the same byte-stream contract; only how it is *established* differs. Same-host
// only, so it skips TCP's network stack and its Nagle/loopback overhead: the low-latency wire the
// editor wants (ADR-0016), and the lossless-by-default (LZ4) path (ADR-0017). Move-only / RAII.
//
// Portability: AF_UNIX is the backend on **both** POSIX and Windows — Windows has supported
// filesystem AF_UNIX sockets since Windows 10 1803 (2018), so one implementation serves all three
// OSes and the send/recv path is shared with TcpSocket, rather than a separate Win32 named-pipe API
// with its own ReadFile/WriteFile semantics. (Abstract-namespace sockets and shared-memory
// zero-copy are documented later seams; see docs/design/net-sockets.md.)
class LocalSocket {
public:
    // Connect to the socket bound at `path` and block until connected. Returns nullopt on failure
    // (logged) — including the common "server not up yet" (ENOENT/ECONNREFUSED). The path must fit
    // the platform's sockaddr_un limit (~108 bytes); an over-long path is refused, not truncated.
    [[nodiscard]] static std::optional<LocalSocket> connect(std::string_view path);

    LocalSocket() = default;
    ~LocalSocket();
    LocalSocket(LocalSocket&&) noexcept;
    LocalSocket& operator=(LocalSocket&&) noexcept;
    LocalSocket(const LocalSocket&) = delete;
    LocalSocket& operator=(const LocalSocket&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }

    [[nodiscard]] std::optional<std::size_t> send(std::span<const std::byte> data);
    [[nodiscard]] std::optional<std::size_t> recv(std::span<std::byte> buffer);
    [[nodiscard]] bool send_all(std::span<const std::byte> data);
    [[nodiscard]] bool recv_exact(std::span<std::byte> buffer);

    void close() noexcept;

    [[nodiscard]] SocketHandle native_handle() const noexcept { return handle_; }

private:
    explicit LocalSocket(SocketHandle h) noexcept : handle_(h) {}
    friend class LocalListener; // accept() mints connected LocalSockets

    SocketHandle handle_ = kInvalidSocket;
};

// A listening Unix-domain socket: bind a filesystem path, then accept() connections. Move-only /
// RAII. Binding creates the path as a socket file; the listener **unlinks it on close** so a
// restart is not blocked by a stale node (AF_UNIX has no SO_REUSEADDR equivalent for the path).
class LocalListener {
public:
    // Bind and listen on `path`. Any existing file at `path` is unlinked first (a stale socket from
    // a crashed server, or the caller's own previous run). Returns nullopt on failure (logged):
    // an over-long path, a permission error, or a path whose directory does not exist.
    [[nodiscard]] static std::optional<LocalListener> bind(std::string_view path);

    LocalListener() = default;
    ~LocalListener();
    LocalListener(LocalListener&&) noexcept;
    LocalListener& operator=(LocalListener&&) noexcept;
    LocalListener(const LocalListener&) = delete;
    LocalListener& operator=(const LocalListener&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }

    [[nodiscard]] std::optional<LocalSocket> accept();

    void close() noexcept; // closes the socket AND unlinks the bound path

private:
    LocalListener(SocketHandle h, std::string path) noexcept : handle_(h), path_(std::move(path)) {}

    SocketHandle handle_ = kInvalidSocket;
    std::string path_; // the bound path, kept so close() can unlink it
};

} // namespace rime::platform
