# The platform TCP sockets seam

*Design note for **S0.1** — the transport primitive the graphics-streaming track (Track S) is built
on, and the seed of the future `engine/net` module (M11). Code: `engine/platform/include/rime/
platform/socket.hpp` + `src/socket.cpp` + `src/posix/socket_posix.cpp` + `src/win32/socket_win32.cpp`.*

## Why sockets live in `platform` (for now)

Track S streams an engine-rendered viewport to a thin client and carries input back
([ADR-0016](../adr/0016-editor-is-a-client-of-the-engine.md)). That needs a byte pipe between two
processes. A socket is an OS primitive with per-platform APIs (BSD sockets vs. Winsock), which is
exactly the kind of thing `platform` exists to hide — so the minimal wrapper starts here, next to
window/input/filesystem. It is deliberately tiny: **blocking TCP only**. When the transport grows a
real identity (UDP/QUIC, sessions, congestion control at S2+), it graduates into `engine/net`; the
interface is shaped so that move is additive, not a rewrite.

## The seam: no OS type in the interface

Same discipline as windowing ([ADR-0006](../adr/0006-native-windowing.md)): the public header names
no OS type, and each OS's implementation is a backend compiled only on its platform. The one place
the two worlds must meet is the socket handle — a POSIX descriptor is an `int`, a Win32 `SOCKET` is
a `UINT_PTR` — and both fit in an `intptr_t`, so the header carries the handle as
`SocketHandle = std::intptr_t` and the backend reinterprets it. `kInvalidSocket = -1` is what a
failed POSIX call returns *and* what Win32's `INVALID_SOCKET` (all-ones `UINT_PTR`) narrows to, so
one sentinel serves both.

One difference from windowing: BSD sockets are **shared** by Linux and macOS, so a single
`src/posix/socket_posix.cpp` serves both (windowing needed three separate native backends). Only
Windows needs its own file. The portable half — RAII/move lifetime and the transfer loops — lives in
`src/socket.cpp` and is written once.

## API shape

`TcpListener::bind(port, host="127.0.0.1") → accept() → TcpSocket`, and
`TcpSocket::connect(host, port)`, then `send`/`recv` (single, possibly-partial) with `send_all`/
`recv_exact` layered on top. Fallible calls return `std::optional` — the module's existing
convention (`filesystem::read_file`) — and additionally **log the reason** (`errno` /
`WSAGetLastError`), because a silent network failure is miserable to debug.

## Details a reader should know (the teaching bits)

- **TCP is a byte stream, not messages.** A `send` may write fewer bytes than asked and a `recv`
  may return fewer than are coming; that is not an error. `send_all`/`recv_exact` loop until the
  whole span is transferred, and `recv_exact` treats an early EOF as failure — the shape a
  length-prefixed frame protocol (S0.4) will stand on.
- **`recv` returning 0 means clean EOF** (peer closed), distinct from `nullopt` (error).
- **`TCP_NODELAY`** (Nagle disabled): an interactive/streaming client wants each write on the wire
  now, not coalesced. We trade a little bandwidth for latency.
- **SIGPIPE:** writing to a peer that has gone away raises SIGPIPE and would kill the process.
  Linux passes `MSG_NOSIGNAL` per send; macOS sets `SO_NOSIGPIPE` on the socket; Windows has no such
  signal. Either way a dead-peer write surfaces as an ordinary error.
- **`SO_REUSEADDR`** on the listener so a quick restart isn't blocked by the previous socket's
  `TIME_WAIT`.
- **Ephemeral port:** `bind(0)` lets the OS pick a free port, read back with `local_port()` — how
  the loopback test avoids racing a hard-coded port, and how a dev tool can advertise "connect to
  me on <port>".
- **Winsock lifetime:** Windows requires `WSAStartup` before any socket call; the backend does it
  lazily and exactly once (`std::call_once`) and never calls `WSACleanup` — the library is wanted
  for the whole process and the OS reclaims it at exit.

## Deliberate limitations (labeled, per CLAUDE.md)

- **Blocking only.** No non-blocking/async I/O, no timeouts, no `poll`/`epoll`/IOCP. For S0 the
  bottleneck is frame readback + encode, not a socket stall (measure before optimizing) — async
  readback and non-blocking transport are S1/S2, and fold in behind this same interface.
- **TCP only.** UDP/QUIC (loss-tolerant, congestion-controlled) is S2, when internet-grade streaming
  needs it.
- **No TLS/auth.** S0 is LAN/loopback. Session/auth arrives with the internet transport (S2).
- **No half-close / shutdown()** distinct from close, and **no explicit `Endpoint` type** yet — add
  when a caller needs them.

## Proof

`tests/platform/socket_test.cpp` — loopback round-trip (both directions, via `send_all`/
`recv_exact`), clean-EOF on peer close, a refused connect returning `nullopt` (no crash/hang), and
move-only single-ownership. GPU-free; runs in CI on Windows, Linux, and macOS.
