// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The editor<->engine transport, named once per platform.
//!
//! The engine's local wire is `AF_UNIX` on **all three** platforms, not just the POSIX two:
//! Windows has supported filesystem `AF_UNIX` sockets since Windows 10 1803, and the C++ side
//! already serves them there (`engine/platform/src/win32/socket_win32.cpp`, the s1.4 backend --
//! see the portability note in `engine/platform/include/rime/platform/socket.hpp`). So the wire
//! was never the thing that stopped the editor running on Windows.
//!
//! What stopped it is narrower, and it is a Rust standard-library gap rather than an OS one:
//! `std::os::unix::net::UnixStream` is, by its module path, Unix-only. Windows std exposes no
//! equivalent even though the OS underneath has the socket family. Naming
//! `std::os::unix::net::UnixStream` directly therefore made the editor's client half refuse to
//! COMPILE on Windows -- `error[E0433]: cannot find `unix` in `os`` -- which is why
//! `--features gui` could not be built here at all and why `--smoke` carried a Unix-only stub.
//!
//! `uds_windows` fills exactly that gap and nothing else: the same `AF_UNIX` sockets through the
//! Win32 API, with std's `UnixStream` shape (`connect`, `try_clone`, `set_read_timeout`, `Read`,
//! `Write`). Because `rime_protocol::Connection<S>` is generic over `S: Read + Write`, no protocol
//! code changes and NO BYTES ON THE WIRE CHANGE -- both halves still speak the same s1.4 socket the
//! Linux and macOS editors have been speaking all along. That is the property that makes this a
//! port rather than a second transport to keep in step: there is one wire, and one set of proofs
//! covering it.
//!
//! Deliberately NOT done here: TCP loopback or a named pipe. Either would have been a genuinely
//! different transport -- a second wire format for the engine to serve, a port or pipe name to
//! allocate and secure, and a divergence between what CI proves on Linux and what runs on Windows.
//! An `AF_UNIX` path has none of that: it is the same filesystem rendezvous on every platform.

/// The local stream the editor speaks to `rime-engine` over. `std`'s on Unix; `uds_windows`'
/// identically-shaped `AF_UNIX` stream on Windows.
#[cfg(unix)]
pub use std::os::unix::net::UnixStream as LocalStream;

#[cfg(windows)]
pub use uds_windows::UnixStream as LocalStream;
