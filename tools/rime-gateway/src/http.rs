// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! A deliberately small HTTP/1.1 server surface — enough for Track H's session API and nothing more.
//!
//! **Why hand-rolled rather than a crate.** `rime-protocol` is dependency-free on purpose: it is the
//! crate the C++ conformance fixtures test against, and a wire format with no third-party parser in
//! it is one whose behaviour is fully described by this repository. The gateway inherits that
//! posture. What we need is one method, one path, one small JSON body and a bounded response — a
//! fraction of HTTP/1.1 — and the cost of a general server (a runtime, TLS, a middleware stack, a
//! supply chain) buys capability we have decided not to have. TLS and the public hostname live in
//! **blackStar**, the estate's edge (ADR-0047 §3), not here.
//!
//! **A hand-rolled parser is a DoS surface unless every length is bounded, so every length here is
//! bounded.** That is the property this module exists to get right, and it is the reason the limits
//! are `pub const` rather than magic numbers: a reader can see the whole budget at once. There is no
//! "read until newline" anywhere without a `take` in front of it. A request that exceeds a limit is
//! **refused with a status, not truncated** — a silently truncated request is one the router then
//! interprets, which is how a length bug becomes a routing bug.
//!
//! **What is deliberately absent:** chunked transfer-encoding (refused, `501`), keep-alive pipelining
//! beyond one request per connection read, `Expect: 100-continue`, compression, and any form of
//! authentication. v1 is LAN and un-authenticated by ADR-0045's last consequence; auth arrives with
//! the production box, and it arrives here rather than in `engine/`.

use std::fmt;
use std::io::{BufRead, Read, Write};
use std::net::SocketAddr;

/// The request line — `METHOD target HTTP/1.1` — including the trailing CRLF.
///
/// 8 KiB is the conventional ceiling (nginx's `large_client_header_buffers` default) and is ~100x
/// more than any route in [`crate::api`] needs. The limit is not tuned to our routes on purpose: a
/// limit that only just fits today's longest path turns adding a route into a protocol change.
pub const MAX_REQUEST_LINE: usize = 8 * 1024;

/// All header lines together. Bounded **in aggregate**, not just per line, because a thousand
/// 20-byte headers is the same attack as one enormous one and a per-line limit does not see it.
pub const MAX_HEADER_BYTES: usize = 16 * 1024;

/// How many header lines we will parse. A second, cheaper bound on the same attack: it fails fast
/// before the byte budget is exhausted, so a pathological request costs allocations proportional to
/// 64, not to 16 KiB.
pub const MAX_HEADERS: usize = 64;

/// The largest request body we will read. The session API's bodies are a few dozen bytes; 64 KiB is
/// slack for a future SDP offer, which is the one body on the roadmap that is genuinely large.
pub const MAX_BODY_BYTES: usize = 64 * 1024;

/// The methods the session API uses. Anything else parses successfully and is refused by the router
/// with `405`, rather than failing to parse — the distinction matters because a `405` names the
/// methods that *are* allowed and a parse failure cannot.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Method {
    Get,
    Post,
    Delete,
    /// A syntactically valid method we do not serve.
    Other(String),
}

impl Method {
    fn parse(token: &str) -> Self {
        match token {
            "GET" => Method::Get,
            "POST" => Method::Post,
            "DELETE" => Method::Delete,
            other => Method::Other(other.to_string()),
        }
    }

    #[must_use]
    pub fn as_str(&self) -> &str {
        match self {
            Method::Get => "GET",
            Method::Post => "POST",
            Method::Delete => "DELETE",
            Method::Other(s) => s,
        }
    }
}

/// One parsed request. `path` is the target with any query string removed; `query` keeps the rest.
#[derive(Debug, Clone)]
pub struct Request {
    pub method: Method,
    /// Path only, percent-decoding **not** applied — see [`Request::path`]'s note below.
    pub path: String,
    /// Everything after the first `?`, empty when there was none. Unparsed; no route needs it yet.
    pub query: String,
    /// Lowercased header names paired with their values, in arrival order.
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Request {
    /// The first header with this (lowercase) name.
    #[must_use]
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.as_str())
    }
}

/// Why a request was refused before routing. Every variant maps to a status, because a parser that
/// cannot say *what* it refused leaves the client guessing and leaves us unable to count.
#[derive(Debug)]
pub enum ParseError {
    /// The peer closed before sending anything. Normal — a health probe does this — and not an error
    /// worth logging as one, which is why it is a distinct variant rather than an `Io`.
    NoRequest,
    /// The request line exceeded [`MAX_REQUEST_LINE`].
    RequestLineTooLong,
    /// The request line was not `METHOD target VERSION`.
    MalformedRequestLine,
    /// Not `HTTP/1.1` or `HTTP/1.0`.
    UnsupportedVersion(String),
    /// More than [`MAX_HEADERS`] header lines.
    TooManyHeaders,
    /// Header bytes exceeded [`MAX_HEADER_BYTES`].
    HeadersTooLarge,
    /// A header line had no `:`.
    MalformedHeader,
    /// `Content-Length` was not a number.
    MalformedContentLength,
    /// `Content-Length` exceeded [`MAX_BODY_BYTES`].
    BodyTooLarge {
        declared: usize,
        limit: usize,
    },
    /// The body ended before `Content-Length` bytes arrived.
    TruncatedBody {
        expected: usize,
        got: usize,
    },
    /// The peer closed inside the header block, before the blank line that ends it.
    TruncatedHeaders,
    /// `Transfer-Encoding: chunked`. Deliberately unimplemented rather than half-implemented: a
    /// chunk-size parser is a second framing layer, and a wrong one is a request-smuggling bug.
    ChunkedUnsupported,
    Io(std::io::Error),
}

impl ParseError {
    /// The status to answer with. `413`/`431` exist precisely so an over-large request is refused
    /// informatively rather than as a generic `400`.
    #[must_use]
    pub fn status(&self) -> u16 {
        match self {
            // A request that did not arrive in time is the client's to retry, and 408 says so.
            ParseError::Io(e) if e.kind() == std::io::ErrorKind::TimedOut => 408,
            ParseError::NoRequest | ParseError::Io(_) => 400,
            ParseError::RequestLineTooLong => 414,
            ParseError::HeadersTooLarge | ParseError::TooManyHeaders => 431,
            ParseError::BodyTooLarge { .. } => 413,
            ParseError::UnsupportedVersion(_) => 505,
            ParseError::ChunkedUnsupported => 501,
            ParseError::MalformedRequestLine
            | ParseError::MalformedHeader
            | ParseError::MalformedContentLength
            | ParseError::TruncatedBody { .. }
            | ParseError::TruncatedHeaders => 400,
        }
    }
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ParseError::NoRequest => write!(f, "peer sent no request"),
            ParseError::RequestLineTooLong => {
                write!(f, "request line exceeds {MAX_REQUEST_LINE} bytes")
            }
            ParseError::MalformedRequestLine => write!(f, "malformed request line"),
            ParseError::UnsupportedVersion(v) => write!(f, "unsupported HTTP version {v}"),
            ParseError::TooManyHeaders => write!(f, "more than {MAX_HEADERS} headers"),
            ParseError::HeadersTooLarge => write!(f, "headers exceed {MAX_HEADER_BYTES} bytes"),
            ParseError::MalformedHeader => write!(f, "malformed header line"),
            ParseError::MalformedContentLength => write!(f, "malformed Content-Length"),
            ParseError::BodyTooLarge { declared, limit } => {
                write!(f, "body of {declared} bytes exceeds the {limit}-byte limit")
            }
            ParseError::TruncatedBody { expected, got } => {
                write!(f, "body ended at {got} of {expected} bytes")
            }
            ParseError::TruncatedHeaders => write!(f, "connection closed inside the header block"),
            ParseError::ChunkedUnsupported => write!(f, "chunked transfer-encoding is not served"),
            ParseError::Io(e) => write!(f, "i/o: {e}"),
        }
    }
}

impl std::error::Error for ParseError {}

/// Read exactly one request from `reader`.
///
/// Takes a [`BufRead`] rather than a `Read` so the line reads are not a syscall per byte, and reads
/// the body from the *same* buffered reader — reading it from the raw stream instead would discard
/// whatever the buffer had already pulled in, which is the classic way a body arrives short.
pub fn read_request<R: BufRead>(reader: &mut R) -> Result<Request, ParseError> {
    let line = match read_line(reader, MAX_REQUEST_LINE, ParseError::RequestLineTooLong)? {
        Some(line) if !line.is_empty() => line,
        // Nothing at all, or a bare CRLF: no request to route either way.
        _ => return Err(ParseError::NoRequest),
    };

    let mut parts = line.split(' ');
    let (method, target, version) = match (parts.next(), parts.next(), parts.next(), parts.next()) {
        // A fourth token means a space inside the target, which we do not accept: a space is the
        // field separator, so accepting it would mean guessing where the target ends.
        (Some(m), Some(t), Some(v), None) if !m.is_empty() && !t.is_empty() => (m, t, v),
        _ => return Err(ParseError::MalformedRequestLine),
    };
    if version != "HTTP/1.1" && version != "HTTP/1.0" {
        return Err(ParseError::UnsupportedVersion(version.to_string()));
    }

    let (path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target.to_string(), String::new()),
    };

    let mut headers: Vec<(String, String)> = Vec::new();
    let mut header_bytes = 0usize;
    loop {
        let remaining = MAX_HEADER_BYTES - header_bytes;
        // `read_line` strips the CRLF, so the blank line that terminates the block is empty. Its own
        // two bytes are not charged to the budget; two bytes of slack is not worth the confusion of
        // an off-by-two in the limit's meaning.
        //
        // `None` is EOF, and EOF here is NOT the end of the headers — it is a peer that closed
        // mid-request. Treating the two as the same thing would route a request whose headers had not
        // finished arriving, with whatever subset did arrive, and a `Content-Length` lost that way
        // reads as a body-less request rather than as a failure.
        let line = match read_line(reader, remaining + 1, ParseError::HeadersTooLarge)? {
            Some(line) => line,
            None => return Err(ParseError::TruncatedHeaders),
        };
        if line.is_empty() {
            break;
        }
        header_bytes += line.len() + 2;
        if header_bytes > MAX_HEADER_BYTES {
            return Err(ParseError::HeadersTooLarge);
        }
        if headers.len() >= MAX_HEADERS {
            return Err(ParseError::TooManyHeaders);
        }
        let (name, value) = line.split_once(':').ok_or(ParseError::MalformedHeader)?;
        if name.is_empty() || name.contains(' ') {
            // `Foo : bar` is a smuggling shape, not a tolerable typo: a peer that normalises it
            // differently from us sees a different request. Refuse rather than normalise.
            return Err(ParseError::MalformedHeader);
        }
        headers.push((name.to_ascii_lowercase(), value.trim().to_string()));
    }

    // Chunked is refused before `Content-Length` is consulted, because a request carrying both is
    // exactly the ambiguity request smuggling exploits, and `Transfer-Encoding` wins per RFC 9112.
    if let Some(te) = headers
        .iter()
        .find(|(k, _)| k == "transfer-encoding")
        .map(|(_, v)| v)
    {
        if !te.eq_ignore_ascii_case("identity") {
            return Err(ParseError::ChunkedUnsupported);
        }
    }

    let declared = match headers.iter().find(|(k, _)| k == "content-length") {
        Some((_, v)) => v
            .parse::<usize>()
            .map_err(|_| ParseError::MalformedContentLength)?,
        None => 0,
    };
    if declared > MAX_BODY_BYTES {
        return Err(ParseError::BodyTooLarge {
            declared,
            limit: MAX_BODY_BYTES,
        });
    }

    // `take(declared)` is the bound *and* the framing: we never read past this request's body, so a
    // pipelined second request stays in the buffer instead of being swallowed as this one's payload.
    let mut body = Vec::new();
    reader
        .take(declared as u64)
        .read_to_end(&mut body)
        .map_err(ParseError::Io)?;
    if body.len() != declared {
        return Err(ParseError::TruncatedBody {
            expected: declared,
            got: body.len(),
        });
    }

    Ok(Request {
        method: Method::parse(method),
        path,
        query,
        headers,
        body,
    })
}

/// Read one CRLF- or LF-terminated line, refusing at `limit` bytes. `Ok(None)` is end of input.
///
/// The `take` is the whole point: `read_until` on an unbounded reader is an attacker-controlled
/// allocation. Reaching the limit without a newline is a refusal, never a truncation. EOF is reported
/// as `None` rather than as an empty line, so a caller cannot confuse "the peer closed" with "a blank
/// line" — they are the same bytes and completely different events.
fn read_line<R: BufRead>(
    reader: &mut R,
    limit: usize,
    too_long: ParseError,
) -> Result<Option<String>, ParseError> {
    let mut raw = Vec::new();
    let read = reader
        .by_ref()
        .take(limit as u64)
        .read_until(b'\n', &mut raw)
        .map_err(ParseError::Io)?;
    if read == 0 {
        return Ok(None);
    }
    if !raw.ends_with(b"\n") {
        return Err(too_long);
    }
    raw.pop();
    if raw.ends_with(b"\r") {
        raw.pop();
    }
    // Header and request-line bytes must be ASCII. A non-ASCII byte here is either a broken client
    // or a deliberate encoding trick; `from_utf8` would accept multi-byte sequences that then differ
    // from what an edge proxy saw.
    if !raw.is_ascii() {
        return Err(ParseError::MalformedHeader);
    }
    Ok(Some(String::from_utf8(raw).expect("checked ascii")))
}

/// A response, built before anything is written.
///
/// Built whole rather than streamed so `Content-Length` is always exact and always present: a
/// response with a wrong or missing length is one the client must guess the end of, and on a
/// keep-alive connection that guess desynchronises every response after it.
#[derive(Debug, Clone)]
pub struct Response {
    pub status: u16,
    pub content_type: &'static str,
    pub body: Vec<u8>,
    /// Extra headers, e.g. `Allow` on a `405`.
    pub extra: Vec<(&'static str, String)>,
}

impl Response {
    #[must_use]
    pub fn json(status: u16, body: String) -> Self {
        Self {
            status,
            content_type: "application/json",
            body: body.into_bytes(),
            extra: Vec::new(),
        }
    }

    /// An error as JSON. The gateway answers errors in the same media type as successes so a client
    /// never has to branch on content type to find out what went wrong.
    #[must_use]
    pub fn error(status: u16, message: &str) -> Self {
        Self::json(status, format!("{{\"error\":{}}}", json_string(message)))
    }

    /// A response in a media type that is not JSON.
    ///
    /// The one caller is the signalling route, whose answer is `application/sdp` because that is what
    /// the offer was and what `setRemoteDescription` expects. Wrapping it in JSON would mean the page
    /// unwrapping a string that is already a well-defined media type with its own parser in every
    /// browser.
    #[must_use]
    pub fn raw(status: u16, content_type: &'static str, body: Vec<u8>) -> Self {
        Self {
            status,
            content_type,
            body,
            extra: Vec::new(),
        }
    }

    #[must_use]
    pub fn empty(status: u16) -> Self {
        Self {
            status,
            content_type: "application/json",
            body: Vec::new(),
            extra: Vec::new(),
        }
    }

    #[must_use]
    pub fn with_header(mut self, name: &'static str, value: impl Into<String>) -> Self {
        self.extra.push((name, value.into()));
        self
    }

    /// Serialise onto the wire.
    ///
    /// `Connection: close` unconditionally, which is honest rather than lazy: [`read_request`] reads
    /// one request and the caller's loop closes after answering, so advertising keep-alive would be
    /// a claim the server does not honour.
    pub fn write_to<W: Write>(&self, out: &mut W) -> std::io::Result<()> {
        // A `204` carries no content, and RFC 9110 says it carries no `Content-Length` either — so
        // neither header is written for it. Sending `Content-Length: 0` on a 204 is tolerated by most
        // clients and rejected by some strict proxies, which is not a class of bug worth inheriting
        // for the sake of one uniform format string.
        let mut head = if self.status == 204 {
            format!(
                "HTTP/1.1 {} {}\r\nConnection: close\r\n",
                self.status,
                reason_phrase(self.status)
            )
        } else {
            format!(
                "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n",
                self.status,
                reason_phrase(self.status),
                self.content_type,
                self.body.len()
            )
        };
        for (name, value) in &self.extra {
            head.push_str(name);
            head.push_str(": ");
            head.push_str(value);
            head.push_str("\r\n");
        }
        head.push_str("\r\n");
        out.write_all(head.as_bytes())?;
        out.write_all(&self.body)?;
        out.flush()
    }
}

fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        204 => "No Content",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        413 => "Content Too Large",
        414 => "URI Too Long",
        415 => "Unsupported Media Type",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        503 => "Service Unavailable",
        505 => "HTTP Version Not Supported",
        _ => "Status",
    }
}

/// Quote and escape a string as a JSON scalar.
///
/// Hand-written for the same reason the parser is: the responses are a handful of flat objects, and
/// the only thing that can go wrong is escaping, so escaping is what this does — including the
/// control characters below 0x20, which are the ones a naive quoter forgets and which turn a
/// user-supplied name into a broken document.
#[must_use]
pub fn json_string(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Read a **flat object of string values** — `{"game":"hello","surface":"play"}` — and nothing else.
///
/// Not a JSON parser, and deliberately not on its way to becoming one. Every request body the
/// session API accepts is of this shape, so anything richer is surface area with no consumer.
/// Nesting, numbers, arrays, `null` and booleans are **refused**, not ignored: a body the server
/// partly understands is a body the client and server disagree about.
pub fn parse_flat_object(body: &[u8]) -> Result<Vec<(String, String)>, &'static str> {
    let text = std::str::from_utf8(body).map_err(|_| "body is not UTF-8")?;
    let text = text.trim();
    let inner = text
        .strip_prefix('{')
        .and_then(|t| t.strip_suffix('}'))
        .ok_or("body is not a JSON object")?;
    let mut fields = Vec::new();
    let mut rest = inner.trim();
    if rest.is_empty() {
        return Ok(fields);
    }
    loop {
        let (key, after) = take_json_string(rest)?;
        let after = after.trim_start();
        let after = after.strip_prefix(':').ok_or("expected ':' after a key")?;
        let (value, after) = take_json_string(after.trim_start())?;
        // A repeated key is refused rather than resolved first-wins or last-wins. Which one a server
        // picks is exactly the disagreement an attacker exploits when a proxy in front of it picks the
        // other, and no legitimate client sends one.
        if fields.iter().any(|(k, _): &(String, String)| *k == key) {
            return Err("duplicate key");
        }
        fields.push((key, value));
        let after = after.trim_start();
        match after.strip_prefix(',') {
            Some(next) => rest = next.trim_start(),
            None if after.is_empty() => return Ok(fields),
            None => return Err("expected ',' or end of object"),
        }
        if fields.len() > 16 {
            return Err("too many fields");
        }
    }
}

/// Consume one double-quoted string, handling the escapes [`json_string`] emits.
fn take_json_string(text: &str) -> Result<(String, &str), &'static str> {
    let body = text.strip_prefix('"').ok_or("expected a quoted string")?;
    let mut out = String::new();
    let mut chars = body.char_indices();
    while let Some((i, c)) = chars.next() {
        match c {
            '"' => return Ok((out, &body[i + 1..])),
            '\\' => match chars.next() {
                Some((_, '"')) => out.push('"'),
                Some((_, '\\')) => out.push('\\'),
                Some((_, 'n')) => out.push('\n'),
                Some((_, 'r')) => out.push('\r'),
                Some((_, 't')) => out.push('\t'),
                // `\uXXXX` is not accepted. No key or value this API takes needs it, and a partial
                // surrogate-pair implementation is worse than none.
                _ => return Err("unsupported escape"),
            },
            c => out.push(c),
        }
    }
    Err("unterminated string")
}

/// Why an address is not one this service will listen on.
#[derive(Debug)]
pub enum BindRefused {
    /// `0.0.0.0` or `[::]`.
    Unspecified(SocketAddr),
}

impl fmt::Display for BindRefused {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BindRefused::Unspecified(addr) => write!(
                f,
                "refusing to listen on the unspecified address {addr}: bind loopback or the \
                 container's own address, so reachability is a firewall decision"
            ),
        }
    }
}

impl std::error::Error for BindRefused {}

/// Refuse to listen on a wildcard address.
///
/// **This is a security decision with code behind it, not a deployment note.** v1 is
/// un-authenticated (ADR-0045), the estate's only public edge is blackStar (CT 113), and
/// `rime.peekstar.eu` becomes a route to the container's `10.77.0.22` — so which interfaces the
/// gateway answers on is the containment boundary, and `0.0.0.0` erases it. Binding an explicit
/// address means adding an interface cannot silently expose the service; a wildcard bind means a new
/// interface exposes it with no change to this repository at all.
///
/// Enforced here rather than left to the caller because a default that has to be remembered is one
/// that will eventually be forgotten, and the forgetting is invisible until someone scans the host.
pub fn check_bind(addr: SocketAddr) -> Result<(), BindRefused> {
    if addr.ip().is_unspecified() {
        return Err(BindRefused::Unspecified(addr));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn parse(raw: &str) -> Result<Request, ParseError> {
        read_request(&mut Cursor::new(raw.as_bytes().to_vec()))
    }

    #[test]
    fn a_plain_get_parses() {
        let req = parse("GET /api/sessions HTTP/1.1\r\nHost: rime\r\n\r\n").unwrap();
        assert_eq!(req.method, Method::Get);
        assert_eq!(req.path, "/api/sessions");
        assert_eq!(req.query, "");
        assert_eq!(req.header("host"), Some("rime"));
        assert!(req.body.is_empty());
    }

    #[test]
    fn header_names_are_matched_case_insensitively() {
        // A client that sends `CONTENT-TYPE` is not a client with a different request.
        let req = parse("POST /x HTTP/1.1\r\nCONTENT-Type: application/json\r\n\r\n").unwrap();
        assert_eq!(req.header("content-type"), Some("application/json"));
    }

    #[test]
    fn a_query_string_is_split_off_the_path() {
        let req = parse("GET /api/sessions?surface=play HTTP/1.1\r\n\r\n").unwrap();
        assert_eq!(req.path, "/api/sessions");
        assert_eq!(req.query, "surface=play");
    }

    #[test]
    fn a_body_is_read_to_its_declared_length_and_no_further() {
        // The trailing bytes are a pipelined second request. They must NOT land in this body — a
        // parser that read to EOF would swallow them and then route a request that was never sent.
        let raw = "POST /api/sessions HTTP/1.1\r\nContent-Length: 4\r\n\r\nbodyGET /next HTTP/1.1\r\n\r\n";
        let mut cursor = Cursor::new(raw.as_bytes().to_vec());
        let req = read_request(&mut cursor).unwrap();
        assert_eq!(req.body, b"body");
        let next = read_request(&mut cursor).unwrap();
        assert_eq!(next.path, "/next");
    }

    #[test]
    fn an_over_long_request_line_is_refused_not_truncated() {
        // The failure this guards: a parser that stops at the limit and routes what it has would see
        // `GET /api/sessions` here and serve it, having discarded the rest of the line.
        let raw = format!("GET /{} HTTP/1.1\r\n\r\n", "a".repeat(MAX_REQUEST_LINE));
        let err = parse(&raw).unwrap_err();
        assert!(matches!(err, ParseError::RequestLineTooLong));
        assert_eq!(err.status(), 414);
    }

    #[test]
    fn many_small_headers_are_refused_by_count_before_the_byte_budget() {
        // The attack a per-line limit cannot see. It must fail on the COUNT, which is the cheap
        // bound — reaching the byte bound instead would mean having allocated 16 KiB first.
        let mut raw = String::from("GET / HTTP/1.1\r\n");
        for i in 0..(MAX_HEADERS + 5) {
            raw.push_str(&format!("x-pad-{i}: v\r\n"));
        }
        raw.push_str("\r\n");
        let err = parse(&raw).unwrap_err();
        assert!(matches!(err, ParseError::TooManyHeaders), "{err}");
        assert_eq!(err.status(), 431);
    }

    #[test]
    fn one_enormous_header_is_refused_by_the_byte_budget() {
        let raw = format!(
            "GET / HTTP/1.1\r\nx-pad: {}\r\n\r\n",
            "a".repeat(MAX_HEADER_BYTES + 1)
        );
        let err = parse(&raw).unwrap_err();
        assert!(matches!(err, ParseError::HeadersTooLarge), "{err}");
        assert_eq!(err.status(), 431);
    }

    #[test]
    fn an_over_large_declared_body_is_refused_before_it_is_read() {
        // Note the body is NOT present: the refusal must come from the declared length, so an
        // attacker cannot make us allocate by promising bytes they never send.
        let raw = format!(
            "POST /x HTTP/1.1\r\nContent-Length: {}\r\n\r\n",
            MAX_BODY_BYTES + 1
        );
        let err = parse(&raw).unwrap_err();
        assert!(matches!(err, ParseError::BodyTooLarge { .. }), "{err}");
        assert_eq!(err.status(), 413);
    }

    #[test]
    fn a_short_body_is_a_truncated_request_not_a_short_one() {
        let err = parse("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nshort").unwrap_err();
        assert!(
            matches!(
                err,
                ParseError::TruncatedBody {
                    expected: 10,
                    got: 5
                }
            ),
            "{err}"
        );
    }

    #[test]
    fn chunked_is_refused_and_wins_over_content_length() {
        // Both headers present is the request-smuggling shape. Refusing is the only answer that
        // cannot disagree with whatever the edge proxy decided.
        let err =
            parse("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n")
                .unwrap_err();
        assert!(matches!(err, ParseError::ChunkedUnsupported));
        assert_eq!(err.status(), 501);
    }

    #[test]
    fn a_space_before_the_header_colon_is_refused() {
        let err = parse("GET / HTTP/1.1\r\nHost : rime\r\n\r\n").unwrap_err();
        assert!(matches!(err, ParseError::MalformedHeader));
    }

    #[test]
    fn malformed_request_lines_and_versions_are_distinguished() {
        assert!(matches!(
            parse("GET /\r\n\r\n").unwrap_err(),
            ParseError::MalformedRequestLine
        ));
        assert!(matches!(
            parse("GET / HTTP/2.0\r\n\r\n").unwrap_err(),
            ParseError::UnsupportedVersion(_)
        ));
        // A space inside the target is ambiguous, not tolerable.
        assert!(matches!(
            parse("GET /a b HTTP/1.1\r\n\r\n").unwrap_err(),
            ParseError::MalformedRequestLine
        ));
    }

    #[test]
    fn a_connection_that_closes_inside_the_headers_is_a_truncated_request() {
        // The framing bug this guards: treating EOF as the blank line that ends the header block
        // routes a request whose headers were still arriving — and a `Content-Length` lost that way
        // reads as a body-less request rather than as a failure.
        let err = parse("POST /x HTTP/1.1\r\nContent-Length: 5\r\n").unwrap_err();
        assert!(matches!(err, ParseError::TruncatedHeaders), "{err}");
        assert_eq!(err.status(), 400);
        // A properly terminated block with no headers at all is still fine.
        assert_eq!(parse("GET / HTTP/1.1\r\n\r\n").unwrap().path, "/");
    }

    #[test]
    fn a_204_carries_neither_content_type_nor_content_length() {
        let mut out = Vec::new();
        Response::empty(204).write_to(&mut out).unwrap();
        let text = String::from_utf8(out).unwrap();
        assert_eq!(text, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    }

    #[test]
    fn a_duplicate_key_is_refused_rather_than_resolved() {
        // First-wins and last-wins are both defensible, which is the problem: a proxy that picks the
        // other one sees a different request than we do.
        assert!(parse_flat_object(b"{\"surface\":\"play\",\"surface\":\"edit\"}").is_err());
    }

    #[test]
    fn a_closed_connection_is_not_an_error_worth_logging() {
        assert!(matches!(parse("").unwrap_err(), ParseError::NoRequest));
    }

    #[test]
    fn a_response_carries_an_exact_content_length() {
        let mut out = Vec::new();
        Response::json(201, "{\"a\":1}".to_string())
            .with_header("Location", "/api/sessions/7")
            .write_to(&mut out)
            .unwrap();
        let text = String::from_utf8(out).unwrap();
        assert!(text.starts_with("HTTP/1.1 201 Created\r\n"));
        assert!(text.contains("Content-Length: 7\r\n"));
        assert!(text.contains("Location: /api/sessions/7\r\n"));
        assert!(text.ends_with("\r\n\r\n{\"a\":1}"));
    }

    #[test]
    fn json_strings_escape_quotes_backslashes_and_control_bytes() {
        assert_eq!(json_string("a\"b\\c"), "\"a\\\"b\\\\c\"");
        assert_eq!(json_string("line\nbreak"), "\"line\\nbreak\"");
        // The one a naive quoter misses, and the one that produces an unparseable document.
        assert_eq!(json_string("\u{1}"), "\"\\u0001\"");
    }

    #[test]
    fn a_flat_object_round_trips_through_its_own_escaping() {
        let body = format!(
            "{{{}:{},{}:{}}}",
            json_string("game"),
            json_string("hello\"game"),
            json_string("surface"),
            json_string("play")
        );
        let fields = parse_flat_object(body.as_bytes()).unwrap();
        assert_eq!(fields[0], ("game".into(), "hello\"game".into()));
        assert_eq!(fields[1], ("surface".into(), "play".into()));
    }

    #[test]
    fn anything_richer_than_a_flat_string_object_is_refused() {
        // Each of these is a shape the router would otherwise have to have an opinion about.
        for body in [
            "{\"a\":1}",
            "{\"a\":null}",
            "{\"a\":true}",
            "{\"a\":[\"b\"]}",
            "{\"a\":{\"b\":\"c\"}}",
            "[\"a\"]",
            "{\"a\"}",
            "{\"a\":\"b\",}",
            "{\"a\":\"unterminated}",
        ] {
            assert!(
                parse_flat_object(body.as_bytes()).is_err(),
                "should have been refused: {body}"
            );
        }
        assert_eq!(parse_flat_object(b"{}").unwrap(), Vec::new());
        assert_eq!(parse_flat_object(b"  { }  ").unwrap(), Vec::new());
    }

    #[test]
    fn the_unspecified_address_is_refused_and_a_real_one_is_not() {
        // The containment boundary, as a test. If this ever passes for `0.0.0.0`, an added interface
        // exposes an un-authenticated gateway with no diff in this repository.
        for wildcard in ["0.0.0.0:8080", "[::]:8080"] {
            let addr: SocketAddr = wildcard.parse().unwrap();
            let err = check_bind(addr).unwrap_err();
            assert!(matches!(err, BindRefused::Unspecified(_)), "{err}");
        }
        for allowed in ["127.0.0.1:8080", "10.77.0.22:8080", "[::1]:8080"] {
            check_bind(allowed.parse().unwrap()).unwrap();
        }
    }
}
