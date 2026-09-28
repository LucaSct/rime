// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `rime-turn-proxy` — strips blackStar's PROXY v2 header in front of coturn (ADR-0053 decision 2).
//!
//! ```text
//!   visitor ──TLS──> blackStar ──[PROXY v2 header][TLS bytes]──> rime-turn-proxy ──[TLS bytes]──> coturn
//! ```
//!
//! blackStar has no per-route switch for PROXY v2, so every relayed connection starts with the
//! header, and coturn does not speak it. This process reads the header, keeps the visitor's address
//! for the log, and then does nothing but copy bytes in both directions. It never looks inside the
//! TLS: the certificate for `turn.rime.peekstar.eu` is coturn's, not ours.
//!
//! **The header is a claim about who the visitor is, so it is trusted from exactly one peer.** A
//! connection from any other address is closed without a byte being read — if anyone could send a
//! header, anyone could choose the address that gets logged and limited.
//!
//! **Nothing here refuses silently.** A proxy that drops connections without a trace looks exactly
//! like a working one, so every refusal path has its own counter and its own log line.
//!
//! Flags (hand-parsed; this crate avoids dependencies on principle):
//!
//! ```text
//!   --listen <addr:port>       where blackStar connects (never a wildcard address)
//!   --trust <ip>               the only peer allowed to send a header   [10.77.0.1]
//!   --upstream <addr:port>     coturn's TLS listener, e.g. 127.0.0.1:5350
//!   --max-connections <n>      concurrent connections; beyond it, refuse [64]
//! ```

#[cfg(not(unix))]
fn main() {
    eprintln!("rime-turn-proxy runs on Unix only (it sits beside coturn in the Linux container)");
    std::process::exit(1);
}

#[cfg(unix)]
fn main() {
    app::main();
}

#[cfg(unix)]
mod app {
    use std::io::{self, Read, Write};
    use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream};
    use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use std::sync::Arc;
    use std::thread;
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    use rime_gateway::http::check_bind;
    use rime_gateway::proxy_v2::{read_header, ProxyError};

    /// The whole header must arrive inside this, measured from accept. A per-read timeout would let a
    /// peer send one byte every 4.9 seconds and hold a slot for as long as it liked.
    const HEADER_DEADLINE: Duration = Duration::from_secs(5);
    const UPSTREAM_CONNECT_TIMEOUT: Duration = Duration::from_secs(5);

    struct Config {
        listen: SocketAddr,
        trust: IpAddr,
        upstream: SocketAddr,
        max_connections: usize,
    }

    const USAGE: &str = "usage: rime-turn-proxy --listen <addr:port> --upstream <addr:port> \
                         [--trust <ip> (default 10.77.0.1)] [--max-connections <n> (default 64)]";

    fn parse_args<I: Iterator<Item = String>>(mut args: I) -> Result<Config, String> {
        let mut listen = None;
        let mut trust: IpAddr = "10.77.0.1".parse().expect("literal");
        let mut upstream = None;
        let mut max_connections = 64usize;
        while let Some(flag) = args.next() {
            if flag == "--help" || flag == "-h" {
                return Err(USAGE.to_string());
            }
            let mut value = || {
                args.next()
                    .ok_or_else(|| format!("{flag} needs a value\n{USAGE}"))
            };
            match flag.as_str() {
                "--listen" => {
                    let v = value()?;
                    listen = Some(
                        v.parse::<SocketAddr>()
                            .map_err(|e| format!("--listen {v}: {e}"))?,
                    );
                }
                "--trust" => {
                    let v = value()?;
                    trust = v
                        .parse::<IpAddr>()
                        .map_err(|e| format!("--trust {v}: {e}"))?;
                }
                "--upstream" => {
                    let v = value()?;
                    upstream = Some(
                        v.parse::<SocketAddr>()
                            .map_err(|e| format!("--upstream {v}: {e}"))?,
                    );
                }
                "--max-connections" => {
                    let v = value()?;
                    max_connections = v
                        .parse::<usize>()
                        .map_err(|e| format!("--max-connections {v}: {e}"))?;
                    if max_connections == 0 {
                        return Err("--max-connections must be at least 1".into());
                    }
                }
                other => return Err(format!("unknown flag {other}\n{USAGE}")),
            }
        }
        let listen = listen.ok_or_else(|| format!("--listen is required\n{USAGE}"))?;
        let upstream = upstream.ok_or_else(|| format!("--upstream is required\n{USAGE}"))?;
        check_bind(listen).map_err(|e| e.to_string())?;
        Ok(Config {
            listen,
            trust,
            upstream,
            max_connections,
        })
    }

    /// One counter per way a connection can end, so the totals account for every accept.
    #[derive(Default)]
    struct Counters {
        accepted: AtomicU64,
        relayed: AtomicU64,
        refused_untrusted: AtomicU64,
        refused_capacity: AtomicU64,
        refused_bad_header: AtomicU64,
        refused_header_timeout: AtomicU64,
        refused_upstream_unreachable: AtomicU64,
        refused_spawn_failed: AtomicU64,
    }

    fn log(line: &str) {
        let secs = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_secs());
        eprintln!("rime-turn-proxy {secs}: {line}");
    }

    /// Reads with a deadline for the WHOLE operation, re-derived before every read.
    struct DeadlineReader<'a> {
        stream: &'a TcpStream,
        deadline: Instant,
    }

    impl Read for DeadlineReader<'_> {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let remaining = self
                .deadline
                .checked_duration_since(Instant::now())
                .filter(|d| !d.is_zero())
                .ok_or_else(|| io::Error::from(io::ErrorKind::TimedOut))?;
            self.stream.set_read_timeout(Some(remaining))?;
            let mut stream = self.stream;
            stream.read(buf)
        }
    }

    /// Frees the connection slot however the handler ends.
    struct Slot(Arc<AtomicUsize>);
    impl Drop for Slot {
        fn drop(&mut self) {
            self.0.fetch_sub(1, Ordering::SeqCst);
        }
    }

    /// Copy `from` to `to` until `from` ends, returning the bytes moved.
    ///
    /// A clean end (`Ok(0)`) is a half-close: it is passed on with `shutdown(Write)` so the other
    /// side sees end-of-stream while its own direction keeps flowing. An error is not a half-close —
    /// both sockets are shut down entirely, which is also what unblocks the sibling pump.
    fn pump(mut from: TcpStream, mut to: TcpStream) -> u64 {
        let mut buf = [0u8; 16 * 1024];
        let mut total = 0u64;
        loop {
            match from.read(&mut buf) {
                Ok(0) => {
                    let _ = to.shutdown(Shutdown::Write);
                    return total;
                }
                Ok(n) => {
                    if to.write_all(&buf[..n]).is_err() {
                        break;
                    }
                    total += n as u64;
                }
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => break,
            }
        }
        let _ = from.shutdown(Shutdown::Both);
        let _ = to.shutdown(Shutdown::Both);
        total
    }

    /// Returns (client -> upstream bytes, upstream -> client bytes).
    fn relay(client: TcpStream, upstream: TcpStream) -> io::Result<(u64, u64)> {
        let client_read = client.try_clone()?;
        let upstream_write = upstream.try_clone()?;
        let forward = thread::Builder::new()
            .name("turn-proxy-c2u".into())
            .spawn(move || pump(client_read, upstream_write))?;
        let u2c = pump(upstream, client);
        let c2u = forward.join().unwrap_or(0);
        Ok((c2u, u2c))
    }

    fn serve(client: TcpStream, peer: SocketAddr, cfg: &Config, counters: &Counters) {
        let started = Instant::now();
        let header = {
            let mut reader = DeadlineReader {
                stream: &client,
                deadline: started + HEADER_DEADLINE,
            };
            match read_header(&mut reader) {
                Ok(h) => h,
                Err(ProxyError::Io(e))
                    if matches!(
                        e.kind(),
                        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
                    ) =>
                {
                    let n = counters
                        .refused_header_timeout
                        .fetch_add(1, Ordering::Relaxed)
                        + 1;
                    log(&format!(
                        "refused peer={peer}: no complete PROXY header within {}s (timeouts so far: {n})",
                        HEADER_DEADLINE.as_secs()
                    ));
                    return;
                }
                Err(e) => {
                    let n = counters.refused_bad_header.fetch_add(1, Ordering::Relaxed) + 1;
                    log(&format!(
                        "refused peer={peer}: bad PROXY header: {e} (bad headers so far: {n})"
                    ));
                    return;
                }
            }
        };
        let _ = client.set_read_timeout(None);
        let visitor = header
            .source
            .map_or_else(|| "local".to_string(), |a| a.to_string());

        let upstream = match TcpStream::connect_timeout(&cfg.upstream, UPSTREAM_CONNECT_TIMEOUT) {
            Ok(s) => s,
            Err(e) => {
                let n = counters
                    .refused_upstream_unreachable
                    .fetch_add(1, Ordering::Relaxed)
                    + 1;
                log(&format!(
                    "refused visitor={visitor}: cannot reach upstream {}: {e} (failures so far: {n})",
                    cfg.upstream
                ));
                return;
            }
        };
        let _ = client.set_nodelay(true);
        let _ = upstream.set_nodelay(true);

        match relay(client, upstream) {
            Ok((c2u, u2c)) => {
                let n = counters.relayed.fetch_add(1, Ordering::Relaxed) + 1;
                log(&format!(
                    "relayed visitor={visitor} to_upstream={c2u}B to_visitor={u2c}B duration={:.3}s \
                     (relayed so far: {n} of {} accepted)",
                    started.elapsed().as_secs_f64(),
                    counters.accepted.load(Ordering::Relaxed)
                ));
            }
            Err(e) => log(&format!("relay for visitor={visitor} failed to start: {e}")),
        }
    }

    pub fn main() {
        let cfg = match parse_args(std::env::args().skip(1)) {
            Ok(c) => Arc::new(c),
            Err(msg) => {
                eprintln!("rime-turn-proxy: {msg}");
                std::process::exit(2);
            }
        };
        let listener = match TcpListener::bind(cfg.listen) {
            Ok(l) => l,
            Err(e) => {
                eprintln!("rime-turn-proxy: cannot listen on {}: {e}", cfg.listen);
                std::process::exit(1);
            }
        };
        let bound = listener.local_addr().unwrap_or(cfg.listen);
        // The integration test reads this line to learn the port when it asks for port 0.
        log(&format!(
            "listening on {bound} trust={} upstream={} max_connections={}",
            cfg.trust, cfg.upstream, cfg.max_connections
        ));

        let counters = Arc::new(Counters::default());
        let active = Arc::new(AtomicUsize::new(0));
        for incoming in listener.incoming() {
            let stream = match incoming {
                Ok(s) => s,
                Err(e) => {
                    log(&format!("accept failed: {e}"));
                    thread::sleep(Duration::from_millis(50));
                    continue;
                }
            };
            counters.accepted.fetch_add(1, Ordering::Relaxed);
            let Ok(peer) = stream.peer_addr() else {
                continue;
            };

            // Trust first, before a byte is read and before a slot is spent.
            if peer.ip().to_canonical() != cfg.trust.to_canonical() {
                let n = counters.refused_untrusted.fetch_add(1, Ordering::Relaxed) + 1;
                log(&format!(
                    "refused peer={peer}: not the trusted peer {} (untrusted so far: {n})",
                    cfg.trust
                ));
                continue;
            }
            if active.fetch_add(1, Ordering::SeqCst) >= cfg.max_connections {
                active.fetch_sub(1, Ordering::SeqCst);
                let n = counters.refused_capacity.fetch_add(1, Ordering::Relaxed) + 1;
                log(&format!(
                    "refused peer={peer}: at the {} connection limit (over-capacity so far: {n})",
                    cfg.max_connections
                ));
                continue;
            }

            let slot = Slot(Arc::clone(&active));
            let (task_cfg, task_counters) = (Arc::clone(&cfg), Arc::clone(&counters));
            let spawned = thread::Builder::new()
                .name("turn-proxy-conn".into())
                .spawn(move || {
                    let _slot = slot;
                    serve(stream, peer, &task_cfg, &task_counters);
                });
            if let Err(e) = spawned {
                // `slot` moved into the closure, which was dropped with the failed spawn, so the
                // slot is already freed.
                let n = counters
                    .refused_spawn_failed
                    .fetch_add(1, Ordering::Relaxed)
                    + 1;
                log(&format!(
                    "refused peer={peer}: cannot start a thread: {e} (spawn failures so far: {n})"
                ));
            }
        }
    }
}
