// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! A reader for the PROXY protocol **version 2** header (ADR-0053 decision 2).
//!
//! blackStar, the estate's edge, relays a visitor's TLS connection to a backend *without decrypting
//! it*, so the backend would otherwise see only blackStar's address. To keep the visitor's address it
//! writes a PROXY v2 header — a small binary prefix — before the first byte of the visitor's stream.
//! coturn does not speak PROXY, so `rime-turn-proxy` reads and strips that header and hands coturn
//! what follows, which is the TLS ClientHello, untouched.
//!
//! **This parser is deliberately narrower than the specification.** The bytes it must accept are the
//! ones blackStar's writer emits (`internal/proxyproto/v2.go`): the 12-byte signature, command
//! `PROXY` or `LOCAL`, and TCP over IPv4 (12 address bytes) or IPv6 (36). Anything else — UDP, UNIX
//! sockets, `UNSPEC`, a version-1 text header — is refused rather than guessed at, because this header
//! decides which address gets logged and limited, and a lenient parser is a way to lie about that.
//!
//! **It reads exactly the header's bytes and no more.** The caller passes the raw socket, not a
//! buffered reader, and the bytes after the header are the TLS handshake. A parser that over-read to
//! be efficient would swallow the start of that handshake and the connection would hang.

use std::fmt;
use std::io::{self, Read};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

/// The twelve bytes that open every v2 header.
const SIGNATURE: [u8; 12] = [
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A,
];

const VERSION2_LOCAL: u8 = 0x20;
const VERSION2_PROXY: u8 = 0x21;
const TCP_OVER_IPV4: u8 = 0x11;
const TCP_OVER_IPV6: u8 = 0x21;

/// The longest address-plus-TLV block accepted: 36 address bytes and a generous 500 for TLVs.
/// blackStar writes none; the specification allows 65535, and a peer that could make us buffer that
/// much per connection would be spending our memory for free.
pub const MAX_BODY_BYTES: usize = 536;

/// What the header said. `source` is `None` for a `LOCAL` command, which a proxy sends for its own
/// health checks and which carries no visitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProxyHeader {
    pub source: Option<SocketAddr>,
    pub destination: Option<SocketAddr>,
}

/// Why a header was refused.
#[derive(Debug)]
pub enum ProxyError {
    /// The stream ended, or failed, before the header was complete.
    Truncated,
    /// The first twelve bytes are not the v2 signature (including a v1 `PROXY TCP4 ...` line).
    BadSignature,
    /// Version/command byte is not `0x20` (LOCAL) or `0x21` (PROXY).
    UnsupportedCommand(u8),
    /// Family/protocol byte is not TCP over IPv4 or IPv6.
    UnsupportedProtocol(u8),
    /// The length field exceeds [`MAX_BODY_BYTES`].
    TooLong(usize),
    /// The length is too short to hold the address block the family byte promises.
    ShortAddressBlock { needed: usize, got: usize },
    /// A real I/O failure other than end-of-stream (a reset, a read timeout).
    Io(io::Error),
}

impl fmt::Display for ProxyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ProxyError::Truncated => write!(f, "the stream ended inside the PROXY v2 header"),
            ProxyError::BadSignature => write!(f, "not a PROXY protocol v2 header"),
            ProxyError::UnsupportedCommand(b) => {
                write!(f, "unsupported PROXY v2 version/command byte {b:#04x}")
            }
            ProxyError::UnsupportedProtocol(b) => {
                write!(f, "unsupported PROXY v2 family/protocol byte {b:#04x}")
            }
            ProxyError::TooLong(n) => write!(
                f,
                "PROXY v2 length {n} exceeds the {MAX_BODY_BYTES}-byte limit"
            ),
            ProxyError::ShortAddressBlock { needed, got } => write!(
                f,
                "PROXY v2 address block is {got} bytes, the family needs {needed}"
            ),
            ProxyError::Io(e) => write!(f, "I/O error reading the PROXY v2 header: {e}"),
        }
    }
}

impl std::error::Error for ProxyError {}

fn read_exact_or<R: Read>(r: &mut R, buf: &mut [u8]) -> Result<(), ProxyError> {
    r.read_exact(buf).map_err(|e| {
        if e.kind() == io::ErrorKind::UnexpectedEof {
            ProxyError::Truncated
        } else {
            ProxyError::Io(e)
        }
    })
}

/// blackStar writes a mixed v4/v6 pair as two IPv6 addresses, with the v4 side IPv4-mapped. Undo
/// that so the log shows `203.0.113.7`, not `::ffff:203.0.113.7`.
fn socket_addr(ip: IpAddr, port: u16) -> SocketAddr {
    SocketAddr::new(ip.to_canonical(), port)
}

/// Read one PROXY v2 header from `r`, consuming exactly its bytes.
pub fn read_header<R: Read>(r: &mut R) -> Result<ProxyHeader, ProxyError> {
    let mut head = [0u8; 16];
    read_exact_or(r, &mut head)?;
    if head[..12] != SIGNATURE {
        return Err(ProxyError::BadSignature);
    }
    let command = head[12];
    let family = head[13];
    let length = usize::from(u16::from_be_bytes([head[14], head[15]]));
    if command != VERSION2_LOCAL && command != VERSION2_PROXY {
        return Err(ProxyError::UnsupportedCommand(command));
    }
    // Checked before the body is read, so an oversized claim costs one comparison, not a buffer.
    if length > MAX_BODY_BYTES {
        return Err(ProxyError::TooLong(length));
    }
    let mut body = vec![0u8; length];
    read_exact_or(r, &mut body)?;

    if command == VERSION2_LOCAL {
        return Ok(ProxyHeader {
            source: None,
            destination: None,
        });
    }
    // Any bytes after the address block are TLVs. blackStar sends none, and none is read for meaning.
    match family {
        TCP_OVER_IPV4 => {
            if length < 12 {
                return Err(ProxyError::ShortAddressBlock {
                    needed: 12,
                    got: length,
                });
            }
            let src = Ipv4Addr::new(body[0], body[1], body[2], body[3]);
            let dst = Ipv4Addr::new(body[4], body[5], body[6], body[7]);
            Ok(ProxyHeader {
                source: Some(SocketAddr::new(
                    src.into(),
                    u16::from_be_bytes([body[8], body[9]]),
                )),
                destination: Some(SocketAddr::new(
                    dst.into(),
                    u16::from_be_bytes([body[10], body[11]]),
                )),
            })
        }
        TCP_OVER_IPV6 => {
            if length < 36 {
                return Err(ProxyError::ShortAddressBlock {
                    needed: 36,
                    got: length,
                });
            }
            let ip = |range: std::ops::Range<usize>| {
                let mut octets = [0u8; 16];
                octets.copy_from_slice(&body[range]);
                IpAddr::V6(Ipv6Addr::from(octets))
            };
            Ok(ProxyHeader {
                source: Some(socket_addr(
                    ip(0..16),
                    u16::from_be_bytes([body[32], body[33]]),
                )),
                destination: Some(socket_addr(
                    ip(16..32),
                    u16::from_be_bytes([body[34], body[35]]),
                )),
            })
        }
        other => Err(ProxyError::UnsupportedProtocol(other)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn from_hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    /// The bytes blackStar's own test pins (`v2_test.go`, `TestAnIPv4HeaderMatchesTheSpecification
    /// ByteForByte`): client 203.0.113.7:51234, local 192.168.178.49:443. Copied, not regenerated,
    /// so the two implementations are tied to the same literal.
    const BLACKSTAR_V4: &str = "0d0a0d0a000d0a515549540a2111000ccb007107c0a8b231c82201bb";

    #[test]
    fn a_blackstar_v4_header_parses_and_leaves_the_tls_bytes_unread() {
        let mut wire = from_hex(BLACKSTAR_V4);
        wire.extend_from_slice(b"\x16\x03\x01");
        let mut cursor = Cursor::new(wire);
        let header = read_header(&mut cursor).unwrap();
        assert_eq!(header.source, Some("203.0.113.7:51234".parse().unwrap()));
        assert_eq!(
            header.destination,
            Some("192.168.178.49:443".parse().unwrap())
        );
        // The property the proxy depends on: not one byte of the ClientHello was consumed.
        let mut rest = Vec::new();
        cursor.read_to_end(&mut rest).unwrap();
        assert_eq!(rest, b"\x16\x03\x01");
    }

    #[test]
    fn a_v6_header_parses() {
        // 2001:db8::1 port 443 -> 2001:db8::2 port 8443, laid out as blackStar's writer does.
        let mut wire = SIGNATURE.to_vec();
        wire.extend_from_slice(&[VERSION2_PROXY, TCP_OVER_IPV6, 0, 36]);
        wire.extend_from_slice(&"2001:db8::1".parse::<Ipv6Addr>().unwrap().octets());
        wire.extend_from_slice(&"2001:db8::2".parse::<Ipv6Addr>().unwrap().octets());
        wire.extend_from_slice(&443u16.to_be_bytes());
        wire.extend_from_slice(&8443u16.to_be_bytes());
        wire.push(0x16);
        let mut cursor = Cursor::new(wire);
        let header = read_header(&mut cursor).unwrap();
        assert_eq!(header.source, Some("[2001:db8::1]:443".parse().unwrap()));
        assert_eq!(
            header.destination,
            Some("[2001:db8::2]:8443".parse().unwrap())
        );
        assert_eq!(cursor.position(), 16 + 36, "the trailing byte is unread");
    }

    #[test]
    fn a_mixed_pair_written_as_v6_comes_back_as_v4() {
        // blackStar writes v4 client + v6 local (or vice versa) as two IPv6 addresses.
        let mut wire = SIGNATURE.to_vec();
        wire.extend_from_slice(&[VERSION2_PROXY, TCP_OVER_IPV6, 0, 36]);
        wire.extend_from_slice(&"::ffff:198.51.100.9".parse::<Ipv6Addr>().unwrap().octets());
        wire.extend_from_slice(&"2001:db8::2".parse::<Ipv6Addr>().unwrap().octets());
        wire.extend_from_slice(&40000u16.to_be_bytes());
        wire.extend_from_slice(&443u16.to_be_bytes());
        let header = read_header(&mut Cursor::new(wire)).unwrap();
        assert_eq!(header.source, Some("198.51.100.9:40000".parse().unwrap()));
    }

    #[test]
    fn a_local_command_has_no_source() {
        let mut wire = SIGNATURE.to_vec();
        wire.extend_from_slice(&[VERSION2_LOCAL, 0x00, 0, 0]);
        wire.push(0x16);
        let mut cursor = Cursor::new(wire);
        let header = read_header(&mut cursor).unwrap();
        assert_eq!(header.source, None);
        assert_eq!(header.destination, None);
        assert_eq!(cursor.position(), 16);
    }

    #[test]
    fn tlvs_after_the_address_block_are_skipped_not_interpreted() {
        let mut wire = from_hex(BLACKSTAR_V4);
        wire[15] = 12 + 5; // length: addresses + a 5-byte TLV
        wire.extend_from_slice(&[0x04, 0x00, 0x02, 0xAA, 0xBB]); // type 4, len 2
        wire.push(0x16);
        let mut cursor = Cursor::new(wire);
        let header = read_header(&mut cursor).unwrap();
        assert_eq!(header.source, Some("203.0.113.7:51234".parse().unwrap()));
        assert_eq!(cursor.position() as usize, 16 + 12 + 5);
    }

    #[test]
    fn a_bad_signature_is_refused() {
        // A version-1 text header is a bad signature here: this parser does not accept it.
        let v1 = b"PROXY TCP4 203.0.113.7 192.168.178.49 51234 443\r\n".to_vec();
        assert!(matches!(
            read_header(&mut Cursor::new(v1)),
            Err(ProxyError::BadSignature)
        ));
        // Right length, wrong final signature byte.
        let mut wire = from_hex(BLACKSTAR_V4);
        wire[11] ^= 0xFF;
        assert!(matches!(
            read_header(&mut Cursor::new(wire)),
            Err(ProxyError::BadSignature)
        ));
        // A TLS ClientHello arriving with no header at all is the misconfiguration this catches.
        let hello = vec![
            0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xFC, 0x03, 0x03, 0, 0, 0, 0, 0,
        ];
        assert!(matches!(
            read_header(&mut Cursor::new(hello)),
            Err(ProxyError::BadSignature)
        ));
    }

    #[test]
    fn an_oversized_length_is_refused() {
        let mut wire = from_hex(BLACKSTAR_V4);
        wire[14..16].copy_from_slice(&((MAX_BODY_BYTES as u16) + 1).to_be_bytes());
        // Not enough bytes follow to satisfy the claim; the refusal must come from the LIMIT, not
        // from running out of input, or the limit is not what stopped it.
        let err = read_header(&mut Cursor::new(wire)).unwrap_err();
        assert!(matches!(err, ProxyError::TooLong(537)), "{err}");
        // And exactly at the limit is accepted as far as the length goes (it then needs the bytes).
        let mut at_limit = from_hex(BLACKSTAR_V4);
        at_limit[14..16].copy_from_slice(&(MAX_BODY_BYTES as u16).to_be_bytes());
        at_limit.resize(16 + MAX_BODY_BYTES, 0);
        assert!(read_header(&mut Cursor::new(at_limit)).is_ok());
    }

    #[test]
    fn a_truncated_header_is_refused() {
        let full = from_hex(BLACKSTAR_V4);
        for cut in [0, 5, 15, 16, 20, full.len() - 1] {
            let err = read_header(&mut Cursor::new(full[..cut].to_vec())).unwrap_err();
            assert!(matches!(err, ProxyError::Truncated), "cut at {cut}: {err}");
        }
    }

    #[test]
    fn other_commands_and_families_are_refused() {
        let mut version1 = from_hex(BLACKSTAR_V4);
        version1[12] = 0x11; // version 1 nibble
        assert!(matches!(
            read_header(&mut Cursor::new(version1)),
            Err(ProxyError::UnsupportedCommand(0x11))
        ));
        let mut udp = from_hex(BLACKSTAR_V4);
        udp[13] = 0x12; // UDP over IPv4
        assert!(matches!(
            read_header(&mut Cursor::new(udp)),
            Err(ProxyError::UnsupportedProtocol(0x12))
        ));
        let mut unspec = from_hex(BLACKSTAR_V4);
        unspec[13] = 0x00;
        assert!(matches!(
            read_header(&mut Cursor::new(unspec)),
            Err(ProxyError::UnsupportedProtocol(0x00))
        ));
    }

    #[test]
    fn a_length_too_short_for_the_family_is_refused() {
        let mut wire = from_hex(BLACKSTAR_V4);
        wire[15] = 8;
        wire.truncate(16 + 8);
        assert!(matches!(
            read_header(&mut Cursor::new(wire)),
            Err(ProxyError::ShortAddressBlock { needed: 12, got: 8 })
        ));
    }
}
