// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Short-lived TURN credentials (ADR-0053 decision 2).
//!
//! A visitor whose network blocks UDP reaches the media through a TURN relay over TLS on 443. The
//! relay must not be an open relay, so every admitted session is handed its own credential that
//! stops working an hour later.
//!
//! **The scheme is coturn's `use-auth-secret` REST mode**, not something invented here. The gateway
//! and coturn share one secret; the gateway computes
//!
//! ```text
//! username   = "<unix-expiry>:<session-id>"
//! credential = base64( HMAC-SHA1(secret, username) )
//! ```
//!
//! and coturn recomputes the HMAC from the username it is shown. Nothing is stored on either side:
//! the expiry lives *inside* the username, and coturn refuses a username whose timestamp has passed.
//! That is what makes the credential self-expiring without a revocation list.
//!
//! The secret is read from a file, never a flag, so it does not appear in `ps` output.

use std::fmt;
use std::path::Path;

use hmac::{Hmac, Mac};
use sha1::Sha1;

/// Shorter than this and the secret is guessable enough that anyone who can capture one credential
/// could recover it offline. coturn accepts a secret of any length, so the floor has to be ours.
pub const MIN_SECRET_BYTES: usize = 32;

/// How long a minted credential lasts. One hour: long enough for a play session's ICE restarts,
/// short enough that a leaked credential is a small window rather than a standing key.
pub const DEFAULT_TTL_SECS: u64 = 3600;

/// What the gateway needs to mint credentials for one TURN server.
#[derive(Clone)]
pub struct TurnConfig {
    /// The ICE-server URL handed to the browser, e.g.
    /// `turns:turn.rime.peekstar.eu:443?transport=tcp`.
    pub uri: String,
    secret: Vec<u8>,
    pub ttl_secs: u64,
}

// Hand-written so a `{:?}` in a log line can never print the shared secret.
impl fmt::Debug for TurnConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TurnConfig")
            .field("uri", &self.uri)
            .field("secret", &"<redacted>")
            .field("ttl_secs", &self.ttl_secs)
            .finish()
    }
}

/// One entry of the browser's `iceServers` list, in the shape `RTCPeerConnection` takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IceServer {
    pub urls: Vec<String>,
    pub username: String,
    pub credential: String,
}

impl TurnConfig {
    /// Read the shared secret from a file, trimmed of ONE trailing newline (the editor's, not part of
    /// the secret). An empty secret and one shorter than [`MIN_SECRET_BYTES`] are refused. The error
    /// text names the path and the length but never the contents.
    pub fn from_secret_file(uri: String, path: &Path) -> Result<Self, String> {
        let mut secret = std::fs::read(path)
            .map_err(|e| format!("cannot read the TURN secret file {}: {e}", path.display()))?;
        if secret.last() == Some(&b'\n') {
            secret.pop();
        }
        if secret.is_empty() {
            return Err(format!("the TURN secret file {} is empty", path.display()));
        }
        if secret.len() < MIN_SECRET_BYTES {
            return Err(format!(
                "the TURN secret in {} is {} bytes; at least {MIN_SECRET_BYTES} are required",
                path.display(),
                secret.len()
            ));
        }
        Ok(Self {
            uri,
            secret,
            ttl_secs: DEFAULT_TTL_SECS,
        })
    }

    /// Mint the credential for `session_id`, valid until `now + ttl_secs` (unix seconds).
    #[must_use]
    pub fn mint(&self, session_id: &str, now: u64) -> IceServer {
        let username = format!("{}:{session_id}", now.saturating_add(self.ttl_secs));
        // HMAC accepts a key of any length, so `new_from_slice` cannot fail here.
        let mut mac =
            Hmac::<Sha1>::new_from_slice(&self.secret).expect("HMAC takes any key length");
        mac.update(username.as_bytes());
        IceServer {
            urls: vec![self.uri.clone()],
            credential: base64_standard(&mac.finalize().into_bytes()),
            username,
        }
    }
}

/// Standard-alphabet, padded base64 (RFC 4648 §4), which is what coturn compares against.
///
/// Hand-written because this is the only place the gateway encodes base64 and the input is always a
/// 20-byte HMAC. The `base64` crate is in `Cargo.lock` only through `webauthn-rs`, which the
/// portable build does not include, so depending on it would add code to every exported bundle to
/// save twelve lines. The vector test pins the output against Python's `base64`.
fn base64_standard(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b = [
            chunk[0],
            *chunk.get(1).unwrap_or(&0),
            *chunk.get(2).unwrap_or(&0),
        ];
        let n = (u32::from(b[0]) << 16) | (u32::from(b[1]) << 8) | u32::from(b[2]);
        out.push(ALPHABET[(n >> 18) as usize & 63] as char);
        out.push(ALPHABET[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 {
            ALPHABET[(n >> 6) as usize & 63] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            ALPHABET[n as usize & 63] as char
        } else {
            '='
        });
    }
    out
}

/// Escape `"` and `\` and control characters for a JSON string. Every field is already restricted
/// to a safe alphabet; this is the belt and braces, in one place.
fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if u32::from(c) < 0x20 => out.push_str(&format!("\\u{:04x}", u32::from(c))),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// `{"ice_servers":[...]}` — each entry exactly what `new RTCPeerConnection({iceServers})` takes.
/// An empty list when TURN is not configured.
#[must_use]
pub fn ice_servers_json(servers: &[IceServer]) -> String {
    let entries: Vec<String> = servers
        .iter()
        .map(|s| {
            let urls: Vec<String> = s.urls.iter().map(|u| json_string(u)).collect();
            format!(
                "{{\"urls\":[{}],\"username\":{},\"credential\":{}}}",
                urls.join(","),
                json_string(&s.username),
                json_string(&s.credential)
            )
        })
        .collect();
    format!("{{\"ice_servers\":[{}]}}", entries.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(secret: &[u8]) -> TurnConfig {
        TurnConfig {
            uri: "turns:turn.rime.peekstar.eu:443?transport=tcp".into(),
            secret: secret.to_vec(),
            ttl_secs: 3600,
        }
    }

    fn secret_file(name: &str, contents: &[u8]) -> std::path::PathBuf {
        let path = std::env::temp_dir().join(format!("rime-turn-{}-{name}", std::process::id()));
        std::fs::write(&path, contents).unwrap();
        path
    }

    #[test]
    fn mint_matches_a_known_vector() {
        // Expected value computed independently of this crate:
        //   python3 -c "import hmac,hashlib,base64;print(base64.b64encode(hmac.new(b'0123456789abcdef0123456789abcdef',b'1790003600:s-7f3a',hashlib.sha1).digest()).decode())"
        // now = 1790000000, ttl = 3600, so the username's expiry is 1790003600.
        let server = config(b"0123456789abcdef0123456789abcdef").mint("s-7f3a", 1_790_000_000);
        assert_eq!(server.username, "1790003600:s-7f3a");
        assert_eq!(server.credential, "zxo07qyMEJZVxtzW/1DDvNyxJe0=");
        assert_eq!(server.urls, vec![config(b"x").uri]);
    }

    #[test]
    fn base64_pads_every_remainder() {
        // RFC 4648 §10 test vectors: one, two and zero leftover bytes.
        assert_eq!(base64_standard(b""), "");
        assert_eq!(base64_standard(b"f"), "Zg==");
        assert_eq!(base64_standard(b"fo"), "Zm8=");
        assert_eq!(base64_standard(b"foo"), "Zm9v");
        assert_eq!(base64_standard(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn a_short_secret_is_refused() {
        let empty = secret_file("empty", b"\n");
        let short = secret_file("short", b"0123456789abcdef0123456789abcde\n"); // 31 bytes
        let exact = secret_file("exact", b"0123456789abcdef0123456789abcdef\n"); // 32 bytes
        let no_newline = secret_file("bare", b"0123456789abcdef0123456789abcdef");
        let uri = || "turns:t:443".to_string();

        assert!(TurnConfig::from_secret_file(uri(), &empty)
            .unwrap_err()
            .contains("empty"));
        let err = TurnConfig::from_secret_file(uri(), &short).unwrap_err();
        assert!(err.contains("31 bytes"), "{err}");
        assert!(
            !err.contains("0123456789abcdef"),
            "the error must not echo the secret: {err}"
        );
        let ok = TurnConfig::from_secret_file(uri(), &exact).unwrap();
        assert_eq!(
            ok.secret.len(),
            32,
            "exactly one trailing newline is trimmed"
        );
        assert_eq!(
            TurnConfig::from_secret_file(uri(), &no_newline)
                .unwrap()
                .secret,
            ok.secret
        );
        assert!(
            TurnConfig::from_secret_file(uri(), Path::new("/nonexistent/turn-secret")).is_err()
        );
        for p in [empty, short, exact, no_newline] {
            let _ = std::fs::remove_file(p);
        }
    }

    #[test]
    fn debug_never_prints_the_secret() {
        let shown = format!("{:?}", config(b"0123456789abcdef0123456789abcdef"));
        assert!(!shown.contains("0123456789abcdef"), "{shown}");
    }

    #[test]
    fn the_ice_json_is_the_shape_the_page_passes_to_rtcpeerconnection() {
        let server = config(b"0123456789abcdef0123456789abcdef").mint("s-7f3a", 1_790_000_000);
        assert_eq!(
            ice_servers_json(&[server]),
            "{\"ice_servers\":[{\"urls\":[\"turns:turn.rime.peekstar.eu:443?transport=tcp\"],\
             \"username\":\"1790003600:s-7f3a\",\"credential\":\"zxo07qyMEJZVxtzW/1DDvNyxJe0=\"}]}"
        );
        // Not configured: an empty list, still a valid object for the page to read.
        assert_eq!(ice_servers_json(&[]), "{\"ice_servers\":[]}");
        // Defensive escaping.
        let odd = IceServer {
            urls: vec!["a\"b".into()],
            username: "c\\d".into(),
            credential: "e\nf".into(),
        };
        assert_eq!(
            ice_servers_json(&[odd]),
            "{\"ice_servers\":[{\"urls\":[\"a\\\"b\"],\"username\":\"c\\\\d\",\"credential\":\"e\\u000af\"}]}"
        );
    }
}
