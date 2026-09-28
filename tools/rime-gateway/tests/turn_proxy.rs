// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `rime-turn-proxy` driven as the compiled binary it ships as, over loopback TCP.
//!
//! The binary is the unit under test, not its functions: the properties that matter here — the
//! header is stripped and the payload behind it survives, an untrusted peer is closed unanswered,
//! nothing reaches the upstream unless it should — only exist once the accept loop, the trust check
//! and the relay are wired together.
#![cfg(unix)]

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

/// A loopback upstream that echoes every byte and counts the connections it was given.
struct Echo {
    addr: SocketAddr,
    connections: Arc<AtomicUsize>,
}

fn echo_upstream() -> Echo {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let connections = Arc::new(AtomicUsize::new(0));
    let counted = Arc::clone(&connections);
    thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            counted.fetch_add(1, Ordering::SeqCst);
            thread::spawn(move || {
                let (mut r, mut w) = (stream.try_clone().unwrap(), stream);
                let mut buf = [0u8; 4096];
                while let Ok(n) = r.read(&mut buf) {
                    if n == 0 || w.write_all(&buf[..n]).is_err() {
                        break;
                    }
                }
                let _ = w.shutdown(Shutdown::Write);
            });
        }
    });
    Echo { addr, connections }
}

/// The proxy process, with its stderr collected so a test can assert on what it logged.
struct Proxy {
    child: Child,
    addr: SocketAddr,
    log: Arc<Mutex<Vec<String>>>,
}

impl Proxy {
    fn start(trust: &str, upstream: SocketAddr, extra: &[&str]) -> Self {
        let mut child = Command::new(env!("CARGO_BIN_EXE_rime-turn-proxy"))
            .args(["--listen", "127.0.0.1:0", "--trust", trust, "--upstream"])
            .arg(upstream.to_string())
            .args(extra)
            .stderr(Stdio::piped())
            .stdout(Stdio::null())
            .stdin(Stdio::null())
            .spawn()
            .unwrap();
        let mut lines = BufReader::new(child.stderr.take().unwrap());
        let mut first = String::new();
        lines.read_line(&mut first).unwrap();
        let addr = first
            .split("listening on ")
            .nth(1)
            .and_then(|rest| rest.split_whitespace().next())
            .unwrap_or_else(|| panic!("no listening line, got {first:?}"))
            .parse()
            .unwrap();
        let log = Arc::new(Mutex::new(vec![first]));
        let sink = Arc::clone(&log);
        thread::spawn(move || {
            for line in lines.lines().map_while(Result::ok) {
                sink.lock().unwrap().push(line);
            }
        });
        Proxy { child, addr, log }
    }

    /// Wait for a log line containing `needle`; the log is written after the connection ends.
    fn wait_for_log(&self, needle: &str) -> String {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(line) = self.log.lock().unwrap().iter().find(|l| l.contains(needle)) {
                return line.clone();
            }
            assert!(
                Instant::now() < deadline,
                "no log line containing {needle:?}; log: {:#?}",
                self.log.lock().unwrap()
            );
            thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for Proxy {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// A PROXY v2 header exactly as blackStar's writer lays out an IPv4 one: 203.0.113.9:41234 -> 10.77.0.22:443.
fn header() -> Vec<u8> {
    let mut h = vec![
        0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A, 0x21, 0x11, 0x00,
        0x0C,
    ];
    h.extend_from_slice(&[203, 0, 113, 9, 10, 77, 0, 22]);
    h.extend_from_slice(&41234u16.to_be_bytes());
    h.extend_from_slice(&443u16.to_be_bytes());
    h
}

fn connect(proxy: &Proxy) -> TcpStream {
    let s = TcpStream::connect(proxy.addr).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    s
}

/// True if the proxy closed the connection without sending anything: a clean end or a reset, but no data.
fn closed_unanswered(s: &mut TcpStream) -> bool {
    let mut buf = [0u8; 64];
    match s.read(&mut buf) {
        Ok(0) => true,
        Ok(_) => false,
        Err(e) => e.kind() == std::io::ErrorKind::ConnectionReset,
    }
}

#[test]
fn a_trusted_peer_gets_the_payload_back_with_the_header_stripped() {
    let upstream = echo_upstream();
    let proxy = Proxy::start("127.0.0.1", upstream.addr, &[]);

    let mut client = connect(&proxy);
    // The three bytes are a TLS record header. The echo returning exactly them proves the upstream
    // saw the bytes AFTER the PROXY header and none of the header itself.
    let payload = b"\x16\x03\x01 client hello";
    client.write_all(&header()).unwrap();
    client.write_all(payload).unwrap();
    let mut back = vec![0u8; payload.len()];
    client.read_exact(&mut back).unwrap();
    assert_eq!(back, payload);

    // Half-close: our end-of-stream must reach the upstream and come back as EOF.
    client.shutdown(Shutdown::Write).unwrap();
    let mut rest = Vec::new();
    client.read_to_end(&mut rest).unwrap();
    assert!(rest.is_empty(), "nothing beyond the echo: {rest:?}");

    let line = proxy.wait_for_log("relayed");
    assert!(line.contains("visitor=203.0.113.9:41234"), "{line}");
    assert!(
        line.contains(&format!("to_upstream={}B", payload.len())),
        "{line}"
    );
    assert!(
        line.contains(&format!("to_visitor={}B", payload.len())),
        "{line}"
    );
    assert_eq!(upstream.connections.load(Ordering::SeqCst), 1);
}

#[test]
fn a_peer_that_is_not_trusted_is_closed_unanswered_and_never_reaches_upstream() {
    let upstream = echo_upstream();
    // We connect from 127.0.0.1; only 10.77.0.1 is trusted.
    let proxy = Proxy::start("10.77.0.1", upstream.addr, &[]);

    let mut client = connect(&proxy);
    let _ = client.write_all(&header());
    let _ = client.write_all(b"\x16\x03\x01 client hello");
    assert!(closed_unanswered(&mut client), "an untrusted peer got data");

    proxy.wait_for_log("not the trusted peer 10.77.0.1");
    assert_eq!(
        upstream.connections.load(Ordering::SeqCst),
        0,
        "the upstream must never see an untrusted peer"
    );
}

#[test]
fn a_trusted_peer_without_a_header_is_refused_and_counted() {
    let upstream = echo_upstream();
    let proxy = Proxy::start("127.0.0.1", upstream.addr, &[]);

    // A bare TLS ClientHello, as if blackStar's header were missing.
    let mut client = connect(&proxy);
    let _ = client.write_all(&[
        0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xFC, 0x03, 0x03,
    ]);
    let _ = client.write_all(&[0u8; 16]);
    assert!(closed_unanswered(&mut client));

    let line = proxy.wait_for_log("bad PROXY header");
    assert!(line.contains("bad headers so far: 1"), "{line}");
    assert_eq!(upstream.connections.load(Ordering::SeqCst), 0);
}

#[test]
fn connections_beyond_the_limit_are_refused_not_queued() {
    let upstream = echo_upstream();
    let proxy = Proxy::start("127.0.0.1", upstream.addr, &["--max-connections", "1"]);

    // The first connection is held open and proven live with a round trip.
    let mut first = connect(&proxy);
    first.write_all(&header()).unwrap();
    first.write_all(b"ping").unwrap();
    let mut back = [0u8; 4];
    first.read_exact(&mut back).unwrap();
    assert_eq!(&back, b"ping");

    let mut second = connect(&proxy);
    let _ = second.write_all(&header());
    assert!(
        closed_unanswered(&mut second),
        "the second connection was served"
    );
    let line = proxy.wait_for_log("connection limit");
    assert!(line.contains("over-capacity so far: 1"), "{line}");

    // The first is unaffected, and once it ends its slot is free again.
    first.write_all(b"pong").unwrap();
    first.read_exact(&mut back).unwrap();
    assert_eq!(&back, b"pong");
    drop(first);
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let mut third = connect(&proxy);
        // The slot is freed asynchronously, so an early attempt is refused and may reset before the
        // write finishes; that is a retry, not a failure.
        let _ = third.write_all(&header());
        let _ = third.write_all(b"back");
        let mut got = [0u8; 4];
        if third.read_exact(&mut got).is_ok() && &got == b"back" {
            break;
        }
        assert!(Instant::now() < deadline, "the slot was never freed");
        thread::sleep(Duration::from_millis(50));
    }
}

#[test]
fn a_wildcard_listen_address_is_refused_at_startup() {
    let out = Command::new(env!("CARGO_BIN_EXE_rime-turn-proxy"))
        .args(["--listen", "0.0.0.0:0", "--upstream", "127.0.0.1:1"])
        .stdin(Stdio::null())
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&out.stderr).contains("unspecified"));
}
