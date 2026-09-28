# ADR-0053 — Media ingress: three forwarded UDP ports, and TURN over TLS on 443 from the start

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca (both decisions below); `gpt-6-astra` consulted independently before the
  question was asked
- **Relates to:** [ADR-0049](0049-webrtc-transport.md) (its consequence "hosted media cannot traverse
  blackStar" is what this answers), [ADR-0047](0047-two-machines-and-the-starbase-tier.md) §3 (nothing
  in the container listens publicly; backends on `vmbr2` at `10.77.0.<CT − 100>`),
  [ADR-0052](0052-browser-decode-path.md) (the video track that needs this path)

## Context

WebRTC media is UDP. The estate's only public ingress is blackStar (CT 113), and the FRITZ!Box forwards
it **TCP 80 and 443 only** (`~/projects/blackstar/DESIGN.md` §2). blackStar peeks SNI and relays TLS
undecrypted, with a PROXY v2 header, to backends on `vmbr2`. Its nftables layer has `input`, `public`
and `output` chains and **no forwarding or NAT** (`internal/ruleset/ruleset.go`). So today a browser's
media has no way in at all, however well the transport works.

The host runs at most three sessions (ADR-0047 §3), and `transport.rs` binds one UDP socket per
session.

## Decision 1: three UDP ports, forwarded through blackStar to CT 122

- The FRITZ!Box forwards **UDP 50000–50002** to blackStar (`192.168.178.50`), unchanged.
- blackStar port-forwards (DNAT) each one to **`10.77.0.22`** (CT 122) on `vmbr2`, port for port.
  This is its first forwarding rule, and it is scoped to exactly those ports and that destination.
- **One port per admission slot.** The gateway leases a port when a session is admitted and releases it
  when the transport drops, including when negotiation fails. With three sessions, a pool keeps the
  transport's one-socket-per-session model. A single shared port would need STUN-username
  demultiplexing, shared timers and a different ownership story, all to save two port forwards.
- **Return traffic must go back through blackStar.** CT 122 policy-routes replies *from* the media
  ports via `10.77.0.1`, so conntrack can reverse the DNAT. Without that, replies leave by CT 122's own
  route with the wrong source, and media fails silently after ICE appears to work. (astra caught this.)
- **The advertised address is not the bound address.** The socket binds `10.77.0.22:<leased>`; the ICE
  candidate advertises the **public** address and the same port, as a server-reflexive candidate whose
  base is the bound socket. Rewriting the host candidate in the SDP text would not work: str0m checks
  that a datagram's destination matches the candidate's base.
- **The public IPv4 is learned by resolving `rime.peekstar.eu`** when a session starts *(implementation
  choice, Claude)*. The home address may change, and whatever keeps that A record current already
  solves "what is our address". Resolving the name reuses that and adds no dependency. It is wrong only
  inside the record's TTL after an address change, and then the session fails to connect rather than
  connecting somewhere else.

**Abuse protection for UDP**, which has no SNI to peek: in blackStar's new forwarding chain, per-source
and aggregate packet/byte limits apply **before** established flows are accepted, and only the three
destination tuples are allowed. There are **no automatic long bans from UDP bursts**: a UDP source is
spoofable, so a ban on one would let an attacker get a real visitor banned. Behind that, ICE's STUN
integrity check, DTLS and SRTP authenticate everything that creates state, so unauthenticated work
stays bounded. The honest limit, as blackStar's DESIGN.md says for TCP: a flood bigger than the home
downlink (57.5 Mbit/s) fills the line before any filter sees it.

## Decision 2: TURN over TLS on 443 is built now, not on demand

A visitor on a network that blocks UDP (some corporate or hotel Wi-Fi) gets no video by Decision 1
alone. Luca chose to close that gap before deploying, not after the first complaint.

- The browser is given `turns:turn.rime.peekstar.eu:443?transport=tcp` as a fallback ICE server.
- blackStar routes that SNI to CT 122 like any other name. blackStar has **no per-route switch for
  PROXY v2** (`Route` is `{tls, http}`), so the header always arrives. A small adapter strips it
  before coturn, which does not speak PROXY v2, and keeps the visitor's address for logs and limits.
- **coturn** runs in CT 122, TLS on loopback, and its relay sockets bind **CT 122's own addresses**.
  (The consult proposed binding them to `10.77.0.1`, which is blackStar's address, not CT 122's. That
  would not bind; corrected here.)
- Credentials are **short-lived and issued per admitted session** (TURN REST-style HMAC username), and
  the relay may only reach the gateway's own media endpoints, so it cannot become an open relay.
- coturn is a separate process, not linked into Rime, so its BSD licence is not an obligation on
  exported games (ADR-0046 §2 is about what we link).

**What it costs:** a TURN relay's media travels over TCP, so a visitor on that path gets the
head-of-line stalls ADR-0045 decision 3 chose WebRTC to avoid. That is still better than no video, and
UDP visitors never touch it. It also adds a third-party daemon, the adapter, a route, and credential
minting in the gateway, roughly two bricks before deployment.

**The riskiest assumption, stated by the consult and not yet tested:** that browsers send SNI on a
`turns:` connection. blackStar routes by SNI, so without SNI a TURN connection matches no route.
It has to be verified with a real browser **before** the TURN bricks are built on it. If it fails,
this decision comes back to Luca; it does not quietly become something else.

**Verified 2026-09-28, the same day, before any TURN code was written.** Each browser loaded a page
whose only ICE server was `turns:turn.test:8443?transport=tcp` with `iceTransportPolicy: "relay"`,
with `turn.test` mapped to loopback, and a Python TLS listener logged the ClientHello's SNI:

| browser | SNI received | then |
|---|---|---|
| Chromium 151.0.7922.173 Arch Linux (headless) | `turn.test`, on both of its connections | rejected the self-signed cert |
| Mozilla Firefox 154.0 (headless) | `turn.test`, on both of its connections | rejected the self-signed cert |

Both send SNI for `turns:`, so blackStar can route TURN by name. Both also refuse an untrusted
certificate, so the TURN listener needs a real certificate for `turn.rime.peekstar.eu`, obtained the
same way as the site's.

## Consequences

- blackStar gains a forwarding chain and a DNAT rule set, a change to another repository
  (`~/projects/blackstar`). It is made there and reviewed there, not described here and assumed.
- The FRITZ!Box change (three UDP forwards) is Luca's to make; nothing here can do it.
- ADR-0047 §3's "nothing in the container listens publicly" is **narrowed, not broken**: the media
  sockets still bind the private `vmbr2` address, and what reaches them is exactly three filtered
  tuples, forwarded by the edge.
- Deployment order: ~~SNI-on-TURN check~~ (done, above) → gateway port pool and public candidate → blackStar forwarding →
  CT 122 policy routing → router forwards → the adapter and coturn → first browser session over UDP,
  then with UDP blocked.
