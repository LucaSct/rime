// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! `rime-gateway` — the process that serves the hosted front end on one port.
//!
//! A thin wrapper by design: everything worth testing lives in `rime_gateway::serve`, and this file
//! only turns flags into a `Server`. The flags are parsed by hand rather than with `clap` because this
//! crate has exactly one third-party dependency and a reason for it; a dozen flags do not earn a
//! second. Exit status 2 means "you invoked me wrongly", 1 means "I could not start".

#[cfg(unix)]
use std::net::{IpAddr, SocketAddr};
#[cfg(unix)]
use std::path::PathBuf;

#[cfg(all(unix, feature = "auth"))]
use rime_auth::{ceremony::CeremonyConfig, flow::Auth, mail::SmtpRelay, AuthStore};
#[cfg(unix)]
use rime_gateway::media::{DnsPublicAddress, MediaPorts};
#[cfg(unix)]
use rime_gateway::relay::{RelayConfig, Str0mFactory, TurnIceProvider};
#[cfg(unix)]
use rime_gateway::serve::{Server, ServerConfig};
#[cfg(unix)]
use rime_gateway::turn::TurnConfig;
#[cfg(all(unix, feature = "auth"))]
use rime_gateway::AuthApi;
#[cfg(unix)]
use rime_gateway::{AccessPolicy, AdmissionPolicy, Catalogue, MediaConfig, ProcessLauncher};

#[cfg(unix)]
const HELP: &str = "Usage: rime-gateway --catalogue <path> [--bind <addr:port>] [--socket-dir <path>] [--open] [--max-sessions <n>] [--max-play <n>] [--trust-forwarded-from-loopback]\n\
With auth: --store <path> --rp-id <id> --rp-origin <url> --rp-name <name> --mail-relay <host:port> --mail-from <address>\n\
With media: --media-bind <ip> --media-ports <lo-hi> --public-name <dns> [--lan-lossless]\n\
With TURN: --turn-uri <uri> --turn-secret-file <path>";

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
    let mut media_flags = MediaFlags::default();

    let mut args = std::env::args().skip(1);
    while let Some(flag) = args.next() {
        if flag == "--help" {
            println!("{HELP}");
            return Ok(());
        }
        let needs_value = match flag.as_str() {
            "--open" | "--trust-forwarded-from-loopback" | "--lan-lossless" => false,
            "--bind" | "--catalogue" | "--socket-dir" | "--max-sessions" | "--max-play" => true,
            "--media-bind" | "--media-ports" | "--public-name" | "--turn-uri"
            | "--turn-secret-file" => true,
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
            "--media-bind" => media_flags.bind = Some(arg.unwrap()),
            "--media-ports" => media_flags.ports = Some(arg.unwrap()),
            "--public-name" => media_flags.public_name = arg,
            "--lan-lossless" => media_flags.lan_lossless = true,
            "--turn-uri" => media_flags.turn_uri = arg,
            "--turn-secret-file" => media_flags.turn_secret = arg.map(PathBuf::from),
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
    let (media, ice) = media_flags.build().map_err(|error| (2, error))?;
    let launcher = ProcessLauncher::new(socket_dir);
    let config = ServerConfig {
        bind,
        catalogue,
        admission,
        access,
        trust_forwarded_from_loopback,
        media,
        ice,
    };
    let server = Server::new(config, launcher);
    #[cfg(feature = "auth")]
    let server = match auth_flags.build().map_err(|e| (2, e))? {
        Some(auth) => server.with_accounts(AuthApi::new(auth)),
        None => server,
    };
    server.listen().map_err(|e| (1, e.to_string()))
}

/// The media and TURN flags, and the rule that they come in complete sets.
///
/// **All or nothing, deliberately.** A port pool with no public name advertises a candidate the
/// internet cannot reach; a public name with no pool has nothing to advertise; a `--lan-lossless`
/// with neither is an operator who thinks they enabled a media path and did not. Each of those
/// starts a gateway that looks configured and answers every offer `503`, and the failure only shows
/// up when somebody tries to play. So an incomplete set is a **startup error**, in the same spirit as
/// the binary already refusing to start when accounts are required but not configured.
#[cfg(unix)]
#[derive(Default)]
struct MediaFlags {
    bind: Option<String>,
    ports: Option<String>,
    public_name: Option<String>,
    lan_lossless: bool,
    turn_uri: Option<String>,
    turn_secret: Option<PathBuf>,
}

#[cfg(unix)]
impl MediaFlags {
    #[allow(clippy::type_complexity)]
    fn build(
        self,
    ) -> Result<
        (
            Option<MediaConfig>,
            Option<Box<dyn rime_gateway::relay::IceProvider>>,
        ),
        String,
    > {
        // TURN is checked first and independently: it is useful to know that the pair is wrong even on
        // a host that has no media flags at all, and a secret in `ps` output is exactly what the
        // file-based flag exists to prevent (`turn.rs`), so a bare `--turn-uri` must not be tolerated.
        let ice: Option<Box<dyn rime_gateway::relay::IceProvider>> =
            match (self.turn_uri, self.turn_secret) {
                (Some(uri), Some(path)) => Some(Box::new(TurnIceProvider(
                    TurnConfig::from_secret_file(uri, &path)?,
                ))),
                (None, None) => None,
                _ => {
                    return Err(
                        "--turn-uri and --turn-secret-file must be given together".to_string()
                    )
                }
            };

        let any = self.bind.is_some()
            || self.ports.is_some()
            || self.public_name.is_some()
            || self.lan_lossless;
        if !any {
            return Ok((None, ice));
        }
        let bind = self
            .bind
            .ok_or("--media-bind is required with the media flags")?;
        let ports = self
            .ports
            .ok_or("--media-ports is required with the media flags")?;
        let public_name = self
            .public_name
            .ok_or("--public-name is required with the media flags")?;

        let ip: IpAddr = bind
            .parse()
            .map_err(|e| format!("invalid --media-bind: {e}"))?;
        let (low, high) = ports
            .split_once('-')
            .ok_or("--media-ports must be <lo>-<hi>")?;
        let low: u16 = low
            .trim()
            .parse()
            .map_err(|e| format!("invalid --media-ports low bound: {e}"))?;
        let high: u16 = high
            .trim()
            .parse()
            .map_err(|e| format!("invalid --media-ports high bound: {e}"))?;
        // `MediaPorts::new` refuses a reversed or empty range and a wildcard address; it is the one
        // place those rules live, so this does not re-implement them.
        let pool = MediaPorts::new(ip, low..=high)?;

        Ok((
            Some(MediaConfig {
                ports: pool,
                public: Box::new(DnsPublicAddress { name: public_name }),
                factory: Box::new(Str0mFactory),
                relay: RelayConfig {
                    lan_lossless: self.lan_lossless,
                },
            }),
            ice,
        ))
    }
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

#[cfg(all(unix, test))]
mod tests {
    use super::*;

    /// The refusal text, or a panic naming what was wrongly accepted.
    ///
    /// Hand-written rather than `unwrap_err`, which would need `Debug` on the success type — and the
    /// success type holds boxed traits whose whole point is that they are not printable.
    fn refusal(flags: MediaFlags) -> String {
        match flags.build() {
            Ok(_) => panic!("an incomplete media configuration was accepted"),
            Err(error) => error,
        }
    }

    fn flags() -> MediaFlags {
        MediaFlags {
            bind: Some("10.77.0.22".into()),
            ports: Some("50000-50002".into()),
            public_name: Some("rime.peekstar.eu".into()),
            ..MediaFlags::default()
        }
    }

    #[test]
    fn the_media_flags_come_as_a_complete_set_or_not_at_all() {
        // A host with none of them is a valid deployment: it serves the page and the account routes and
        // answers every offer 503. A host with SOME of them is an operator who thinks they configured
        // media, and that is the failure worth refusing at startup rather than at play time.
        let (media, ice) = MediaFlags::default().build().unwrap();
        assert!(media.is_none() && ice.is_none());
        assert_eq!(flags().build().unwrap().0.unwrap().ports.capacity(), 3);

        for (missing, flags) in [
            (
                "--media-bind",
                MediaFlags {
                    bind: None,
                    ..flags()
                },
            ),
            (
                "--media-ports",
                MediaFlags {
                    ports: None,
                    ..flags()
                },
            ),
            (
                "--public-name",
                MediaFlags {
                    public_name: None,
                    ..flags()
                },
            ),
        ] {
            let error = refusal(flags);
            assert!(error.contains(missing), "{error}");
        }
        // `--lan-lossless` alone counts as "the operator meant to configure media", so it is refused
        // too rather than silently doing nothing.
        let error = refusal(MediaFlags {
            lan_lossless: true,
            ..MediaFlags::default()
        });
        assert!(error.contains("--media-bind"), "{error}");
    }

    #[test]
    fn a_bad_port_range_or_bind_address_is_a_startup_error() {
        // The rules themselves live in `MediaPorts::new` — this only proves the flags reach them, so a
        // reversed range or a wildcard bind cannot get past the command line.
        for ports in ["50002-50000", "50000", "abc-50002", "50000-"] {
            assert!(
                MediaFlags {
                    ports: Some(ports.into()),
                    ..flags()
                }
                .build()
                .is_err(),
                "accepted --media-ports {ports}"
            );
        }
        assert!(MediaFlags {
            bind: Some("0.0.0.0".into()),
            ..flags()
        }
        .build()
        .is_err());
        assert!(MediaFlags {
            bind: Some("not-an-ip".into()),
            ..flags()
        }
        .build()
        .is_err());
    }

    #[test]
    fn turn_needs_both_of_its_flags() {
        // One without the other is refused in both directions. A `--turn-uri` alone would leave the
        // page trying to relay through a server it has no credential for; a secret file alone is a
        // secret read for nothing.
        for flags in [
            MediaFlags {
                turn_uri: Some("turns:t:443".into()),
                ..MediaFlags::default()
            },
            MediaFlags {
                turn_secret: Some(PathBuf::from("/nonexistent")),
                ..MediaFlags::default()
            },
        ] {
            let error = refusal(flags);
            assert!(error.contains("must be given together"), "{error}");
        }

        // With both, the secret's own rules apply — `turn.rs` refuses a short one, and the error must
        // not echo it.
        let short = std::env::temp_dir().join(format!("rime-bin-turn-{}", std::process::id()));
        std::fs::write(&short, b"tooshort\n").unwrap();
        let error = refusal(MediaFlags {
            turn_uri: Some("turns:t:443".into()),
            turn_secret: Some(short.clone()),
            ..MediaFlags::default()
        });
        assert!(error.contains("bytes"), "{error}");
        assert!(!error.contains("tooshort"), "the secret leaked: {error}");

        std::fs::write(&short, b"0123456789abcdef0123456789abcdef\n").unwrap();
        let (media, ice) = MediaFlags {
            turn_uri: Some("turns:t:443".into()),
            turn_secret: Some(short.clone()),
            ..MediaFlags::default()
        }
        .build()
        .unwrap();
        // TURN is independent of the port pool — coturn is a separate service — so configuring it on a
        // host with no media flags is allowed rather than refused.
        assert!(media.is_none() && ice.is_some());
        let _ = std::fs::remove_file(short);
    }
}
