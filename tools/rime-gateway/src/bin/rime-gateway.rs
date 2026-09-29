// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `rime-gateway` — the process that serves the hosted front end on one port.
//!
//! A thin wrapper by design: everything worth testing lives in `rime_gateway::serve`, and this file
//! only turns flags into a `Server`. The flags are parsed by hand rather than with `clap` because this
//! crate has exactly one third-party dependency and a reason for it; a dozen flags do not earn a
//! second. Exit status 2 means "you invoked me wrongly", 1 means "I could not start".

#[cfg(unix)]
use std::net::SocketAddr;
#[cfg(unix)]
use std::path::PathBuf;

#[cfg(all(unix, feature = "auth"))]
use rime_auth::{ceremony::CeremonyConfig, flow::Auth, mail::SmtpRelay, AuthStore};
#[cfg(unix)]
use rime_gateway::serve::{Server, ServerConfig};
#[cfg(all(unix, feature = "auth"))]
use rime_gateway::AuthApi;
#[cfg(unix)]
use rime_gateway::{AccessPolicy, AdmissionPolicy, Catalogue, ProcessLauncher};

#[cfg(unix)]
const HELP: &str = "Usage: rime-gateway --catalogue <path> [--bind <addr:port>] [--socket-dir <path>] [--open] [--max-sessions <n>] [--max-play <n>] [--trust-forwarded-from-loopback]\n\
With auth: --store <path> --rp-id <id> --rp-origin <url> --rp-name <name> --mail-relay <host:port> --mail-from <address>";

#[cfg(unix)]
fn value(args: &mut impl Iterator<Item = String>, flag: &str) -> Result<String, String> {
    args.next()
        .ok_or_else(|| format!("missing value for {flag}"))
}

#[cfg(all(unix, feature = "auth"))]
fn required<T>(flag: &str, value: Option<T>) -> Result<T, String> {
    value.ok_or_else(|| format!("{flag} is required with --store"))
}

#[cfg(unix)]
fn run() -> Result<(), (i32, String)> {
    let mut bind: SocketAddr = "127.0.0.1:8787".parse().expect("fixed address");
    let mut catalogue = None;
    let mut socket_dir = std::env::temp_dir();
    let mut access = AccessPolicy::default();
    let mut trust_forwarded_from_loopback = false;
    // The measured starbase tier; `AdmissionPolicy::default` carries the evidence for its numbers.
    let mut admission = AdmissionPolicy::default();
    #[cfg(feature = "auth")]
    let mut auth_flags = AuthFlags::default();

    let mut args = std::env::args().skip(1);
    while let Some(flag) = args.next() {
        if flag == "--help" {
            println!("{HELP}");
            return Ok(());
        }
        let needs_value = match flag.as_str() {
            "--open" | "--trust-forwarded-from-loopback" => false,
            "--bind" | "--catalogue" | "--socket-dir" | "--max-sessions" | "--max-play" => true,
            #[cfg(feature = "auth")]
            "--store" | "--rp-id" | "--rp-origin" | "--rp-name" | "--mail-relay"
            | "--mail-from" => true,
            _ => return Err((2, format!("unknown flag: {flag}"))),
        };
        let arg = if needs_value {
            Some(value(&mut args, &flag).map_err(|e| (2, e))?)
        } else {
            None
        };
        match flag.as_str() {
            "--bind" => {
                bind = arg
                    .unwrap()
                    .parse()
                    .map_err(|e| (2, format!("invalid --bind: {e}")))?
            }
            "--catalogue" => catalogue = arg.map(PathBuf::from),
            "--socket-dir" => socket_dir = PathBuf::from(arg.unwrap()),
            "--open" => access.require_account = false,
            "--trust-forwarded-from-loopback" => trust_forwarded_from_loopback = true,
            "--max-sessions" => {
                admission.max_sessions = arg
                    .unwrap()
                    .parse()
                    .map_err(|e| (2, format!("invalid --max-sessions: {e}")))?
            }
            "--max-play" => {
                admission.max_play_sessions = arg
                    .unwrap()
                    .parse()
                    .map_err(|e| (2, format!("invalid --max-play: {e}")))?
            }
            #[cfg(feature = "auth")]
            "--store" => auth_flags.store = arg.map(PathBuf::from),
            #[cfg(feature = "auth")]
            "--rp-id" => auth_flags.rp_id = arg,
            #[cfg(feature = "auth")]
            "--rp-origin" => auth_flags.rp_origin = arg,
            #[cfg(feature = "auth")]
            "--rp-name" => auth_flags.rp_name = arg,
            #[cfg(feature = "auth")]
            "--mail-relay" => auth_flags.mail_relay = arg,
            #[cfg(feature = "auth")]
            "--mail-from" => auth_flags.mail_from = arg,
            _ => unreachable!("flag was checked above"),
        }
    }

    #[cfg(feature = "auth")]
    auth_flags.validate().map_err(|error| (2, error))?;
    // Requiring an account on a host with no account service would start a server that answers
    // every session request 401 and offers no way to earn anything else. Refuse to start instead.
    #[cfg(feature = "auth")]
    let has_accounts = auth_flags.store.is_some();
    #[cfg(not(feature = "auth"))]
    let has_accounts = false;
    if access.require_account && !has_accounts {
        return Err((
            2,
            "accounts are required but none are configured: pass the auth flags, or --open".into(),
        ));
    }
    let catalogue = catalogue.ok_or_else(|| (2, "--catalogue is required".to_string()))?;
    let contents = std::fs::read_to_string(&catalogue).map_err(|e| {
        (
            1,
            format!("cannot read catalogue {}: {e}", catalogue.display()),
        )
    })?;
    let catalogue =
        Catalogue::from_config(&contents).map_err(|e| (2, format!("invalid catalogue: {e}")))?;
    let launcher = ProcessLauncher::new(socket_dir);
    let config = ServerConfig {
        bind,
        catalogue,
        admission,
        access,
        trust_forwarded_from_loopback,
    };
    let server = Server::new(config, launcher);
    #[cfg(feature = "auth")]
    let server = match auth_flags.build().map_err(|e| (2, e))? {
        Some(auth) => server.with_accounts(AuthApi::new(auth)),
        None => server,
    };
    server.listen().map_err(|e| (1, e.to_string()))
}

#[cfg(all(unix, feature = "auth"))]
#[derive(Default)]
struct AuthFlags {
    store: Option<PathBuf>,
    rp_id: Option<String>,
    rp_origin: Option<String>,
    rp_name: Option<String>,
    mail_relay: Option<String>,
    mail_from: Option<String>,
}

#[cfg(all(unix, feature = "auth"))]
impl AuthFlags {
    fn validate(&self) -> Result<(), String> {
        let any = self.store.is_some()
            || self.rp_id.is_some()
            || self.rp_origin.is_some()
            || self.rp_name.is_some()
            || self.mail_relay.is_some()
            || self.mail_from.is_some();
        if !any {
            return Ok(());
        }
        for (flag, supplied) in [
            ("--store", self.store.is_some()),
            ("--rp-id", self.rp_id.is_some()),
            ("--rp-origin", self.rp_origin.is_some()),
            ("--rp-name", self.rp_name.is_some()),
            ("--mail-relay", self.mail_relay.is_some()),
            ("--mail-from", self.mail_from.is_some()),
        ] {
            if !supplied {
                return Err(format!("{flag} is required with --store"));
            }
        }
        Ok(())
    }

    fn build(self) -> Result<Option<Auth>, String> {
        let any = self.store.is_some()
            || self.rp_id.is_some()
            || self.rp_origin.is_some()
            || self.rp_name.is_some()
            || self.mail_relay.is_some()
            || self.mail_from.is_some();
        if !any {
            return Ok(None);
        }
        let store = required("--store", self.store)?;
        let config = CeremonyConfig {
            rp_id: required("--rp-id", self.rp_id)?,
            rp_origin: required("--rp-origin", self.rp_origin)?,
            rp_name: required("--rp-name", self.rp_name)?,
        };
        let relay = required("--mail-relay", self.mail_relay)?;
        let from = required("--mail-from", self.mail_from)?;
        let (host, port) = relay
            .rsplit_once(':')
            .ok_or("--mail-relay must be host:port")?;
        let port = port
            .parse::<u16>()
            .map_err(|_| "--mail-relay has an invalid port")?;
        if host.is_empty() {
            return Err("--mail-relay has an empty host".into());
        }
        let mailer = SmtpRelay::new(
            host.trim_start_matches('[').trim_end_matches(']'),
            port,
            from,
        );
        let store = AuthStore::open(&store).map_err(|e| e.to_string())?;
        Auth::new(store, &config, Box::new(mailer))
            .map(Some)
            .map_err(|e| e.to_string())
    }
}

#[cfg(unix)]
fn main() {
    if let Err((status, message)) = run() {
        eprintln!("rime-gateway: {message}");
        std::process::exit(status);
    }
}

#[cfg(not(unix))]
fn main() {
    eprintln!("rime-gateway: session serving requires Unix sockets");
    std::process::exit(1);
}
