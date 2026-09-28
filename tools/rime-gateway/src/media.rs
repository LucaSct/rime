// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! One private UDP port per admitted media session, and the public address to advertise for it
//! (ADR-0053, Decision 1).
//!
//! **Why a pool and not an ephemeral port.** The edge forwards exactly three UDP ports; a socket on
//! any other port is unreachable from the internet however well it negotiates. So a session's port is
//! *leased* from the forwarded set at admission and returned when the lease drops. Returning on `Drop`
//! means every exit path returns it, a failed SDP negotiation included; nothing has to remember to.
//!
//! **Why the public address comes from DNS.** The home address can change. Whatever keeps
//! `rime.peekstar.eu`'s A record current already answers "what is our address", so the gateway asks
//! the same question rather than keeping a second answer that can drift from the first.

use std::collections::BTreeSet;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, ToSocketAddrs};
use std::ops::RangeInclusive;
use std::sync::{Arc, Mutex};

use crate::http;

struct PoolState {
    free: BTreeSet<u16>,
    capacity: usize,
}

/// A fixed pool of UDP ports on one private address, one per admission slot.
#[derive(Clone)]
pub struct MediaPorts {
    ip: IpAddr,
    state: Arc<Mutex<PoolState>>,
}

impl MediaPorts {
    /// Create a pool without binding any sockets.
    pub fn new(ip: IpAddr, ports: RangeInclusive<u16>) -> Result<Self, String> {
        // An empty range has no slot to lease and usually signals reversed configuration bounds.
        if ports.is_empty() {
            return Err("media port range is empty".to_owned());
        }
        let capacity = usize::from(*ports.end()) - usize::from(*ports.start()) + 1;
        // More than 64 forwarded ports is a firewall hole, not a session-sized pool.
        if capacity > 64 {
            return Err("media port range exceeds 64 ports".to_owned());
        }
        // The gateway must bind a specific interface; a wildcard address could expose media on
        // every present or future interface. Reuse the HTTP boundary's bind check.
        http::check_bind(SocketAddr::new(ip, *ports.start()))
            .map_err(|reason| format!("media bind address refused: {reason:?}"))?;

        Ok(Self {
            ip,
            state: Arc::new(Mutex::new(PoolState {
                free: ports.collect(),
                capacity,
            })),
        })
    }

    /// Lease the lowest free port, or return `None` when the pool is exhausted.
    pub fn lease(&self) -> Option<PortLease> {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let port = state.free.pop_first()?;
        Some(PortLease {
            addr: SocketAddr::new(self.ip, port),
            state: Arc::clone(&self.state),
        })
    }

    /// Number of ports currently free.
    pub fn available(&self) -> usize {
        self.state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .free
            .len()
    }

    /// Number of ports in the configured pool.
    pub fn capacity(&self) -> usize {
        self.state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .capacity
    }
}

/// One leased port. Dropping it releases the slot, including after failed negotiation.
pub struct PortLease {
    addr: SocketAddr,
    state: Arc<Mutex<PoolState>>,
}

impl PortLease {
    /// The private bind address and this lease's port.
    pub fn addr(&self) -> SocketAddr {
        self.addr
    }
}

impl Drop for PortLease {
    fn drop(&mut self) {
        self.state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .free
            .insert(self.addr.port());
    }
}

/// Source for the public IPv4 address advertised to browsers.
pub trait PublicAddress: Send {
    fn resolve(&self) -> Result<Ipv4Addr, String>;
}

/// Resolve the site's own A record when a session starts.
pub struct DnsPublicAddress {
    pub name: String,
}

impl PublicAddress for DnsPublicAddress {
    fn resolve(&self) -> Result<Ipv4Addr, String> {
        let answers = (self.name.as_str(), 0)
            .to_socket_addrs()
            .map_err(|error| format!("could not resolve {}: {error}", self.name))?;
        let ip = answers
            .filter_map(|answer| match answer.ip() {
                IpAddr::V4(ip) => Some(ip),
                IpAddr::V6(_) => None,
            })
            .next()
            .ok_or_else(|| format!("{} has no IPv4 A record", self.name))?;
        validate_public(ip)
    }
}

/// A configured public IPv4 address, useful for static sites and tests.
pub struct FixedPublicAddress(pub Ipv4Addr);

impl PublicAddress for FixedPublicAddress {
    fn resolve(&self) -> Result<Ipv4Addr, String> {
        validate_public(self.0)
    }
}

fn validate_public(ip: Ipv4Addr) -> Result<Ipv4Addr, String> {
    let [first, second, third, _] = ip.octets();
    // Private answers are reachable inside the estate, not from a visitor's browser.
    if ip.is_private() {
        return Err(format!("{ip} is a private address"));
    }
    // Loopback points the browser back at itself rather than at this gateway.
    if ip.is_loopback() {
        return Err(format!("{ip} is a loopback address"));
    }
    // Link-local addresses only work on the sender's local network segment.
    if ip.is_link_local() {
        return Err(format!("{ip} is a link-local address"));
    }
    // An unspecified answer does not identify any media destination.
    if ip.is_unspecified() {
        return Err(format!("{ip} is an unspecified address"));
    }
    // Multicast would fan a visitor's media out to a group, not to this gateway.
    if ip.is_multicast() {
        return Err(format!("{ip} is a multicast address"));
    }
    // Broadcast addresses target a network, not this session's socket.
    if ip.is_broadcast() {
        return Err(format!("{ip} is a broadcast address"));
    }
    // RFC 5737 documentation addresses cannot carry a visitor's media.
    if (first == 192 && second == 0 && third == 2)
        || (first == 198 && second == 51 && third == 100)
        || (first == 203 && second == 0 && third == 113)
    {
        return Err(format!("{ip} is a documentation address"));
    }
    // 100.64/10 is shared carrier space, not a globally reachable destination.
    if first == 100 && (64..=127).contains(&second) {
        return Err(format!("{ip} is a shared address"));
    }
    Ok(ip)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pool(ports: RangeInclusive<u16>) -> MediaPorts {
        MediaPorts::new(IpAddr::V4(Ipv4Addr::new(10, 77, 0, 22)), ports).unwrap()
    }

    #[test]
    fn a_pool_hands_out_each_port_once_until_it_is_released() {
        let pool = pool(50000..=50002);
        assert_eq!(pool.capacity(), 3);
        assert_eq!(pool.available(), 3);
        let first = pool.lease().unwrap();
        let second = pool.lease().unwrap();
        let third = pool.lease().unwrap();
        assert_eq!(first.addr(), "10.77.0.22:50000".parse().unwrap());
        assert_eq!(second.addr(), "10.77.0.22:50001".parse().unwrap());
        assert_eq!(third.addr(), "10.77.0.22:50002".parse().unwrap());
        assert_eq!(pool.available(), 0);
    }

    #[test]
    fn an_exhausted_pool_returns_none_and_a_dropped_lease_frees_its_port() {
        let pool = pool(50000..=50001);
        let first = pool.lease().unwrap();
        let _second = pool.lease().unwrap();
        assert!(pool.lease().is_none());
        drop(first);
        assert_eq!(pool.available(), 1);
        assert_eq!(pool.lease().unwrap().addr().port(), 50000);
    }

    #[test]
    fn a_pool_refuses_an_empty_or_oversized_range() {
        assert!(
            MediaPorts::new(IpAddr::V4(Ipv4Addr::LOCALHOST), RangeInclusive::new(2, 1)).is_err()
        );
        assert!(MediaPorts::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 1..=65).is_err());
        assert_eq!(pool(1..=64).capacity(), 64);
    }

    #[test]
    fn a_pool_refuses_the_unspecified_address() {
        assert!(MediaPorts::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), 50000..=50002).is_err());
    }

    #[test]
    fn the_public_address_refuses_private_and_loopback_answers() {
        for address in [
            Ipv4Addr::new(10, 0, 0, 1),
            Ipv4Addr::new(192, 168, 1, 1),
            Ipv4Addr::new(172, 16, 0, 1),
            Ipv4Addr::new(127, 0, 0, 1),
            Ipv4Addr::new(100, 64, 0, 1),
            Ipv4Addr::new(169, 254, 1, 1),
            Ipv4Addr::new(224, 0, 0, 1),
        ] {
            assert!(FixedPublicAddress(address).resolve().is_err(), "{address}");
        }
        assert!(DnsPublicAddress {
            name: "localhost".to_owned(),
        }
        .resolve()
        .is_err());
    }

    #[test]
    fn a_public_address_is_returned_unchanged() {
        let address = Ipv4Addr::new(95, 89, 215, 226);
        assert_eq!(FixedPublicAddress(address).resolve(), Ok(address));
    }
}
