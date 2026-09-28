// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Plain SMTP handoff to the estate's configured local or LAN relay.
//!
//! The starbase's postfix relay handles SMTPS to Resend and owns the API key. This crate holds no
//! API key, TLS configuration or credentials; it only submits a message to that trusted relay.
//! Mail delivery is a side effect, not authentication, and the caller decides whether to retry.

use std::fmt;
use std::io::{self, BufRead, BufReader, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const TIMEOUT: Duration = Duration::from_secs(5);
static MESSAGE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, PartialEq, Eq)]
pub struct Message {
    pub to: String,
    pub subject: String,
    pub body: String,
}

// Message bodies may carry verification codes. Even debugging a captured message must not log one.
impl fmt::Debug for Message {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Message")
            .field("to", &self.to)
            .field("subject", &self.subject)
            .field("body", &"<redacted>")
            .finish()
    }
}

pub trait Mailer: Send + Sync {
    fn send(&self, message: &Message) -> Result<(), MailError>;
}

/// One SMTP submission endpoint; retries and queueing belong to its caller or relay.
pub struct SmtpRelay {
    host: String,
    port: u16,
    envelope_from: String,
    timeout: Duration,
}

impl SmtpRelay {
    #[must_use]
    pub fn new(host: impl Into<String>, port: u16, envelope_from: impl Into<String>) -> Self {
        Self {
            host: host.into(),
            port,
            envelope_from: envelope_from.into(),
            timeout: TIMEOUT,
        }
    }
}

#[derive(Debug)]
pub enum MailError {
    Rejected { step: &'static str, reply: String },
    HeaderInjection,
    Io(io::Error),
}

impl fmt::Display for MailError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Rejected { step, reply } => write!(f, "SMTP rejected {step}: {reply}"),
            Self::HeaderInjection => write!(f, "mail header contains a forbidden control byte"),
            Self::Io(error) => write!(f, "mail relay i/o: {error}"),
        }
    }
}

impl std::error::Error for MailError {}

impl From<io::Error> for MailError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

impl Mailer for SmtpRelay {
    fn send(&self, message: &Message) -> Result<(), MailError> {
        // SECURITY: refuse CR, LF and NUL in header and envelope fields before opening a socket.
        // Escaping would leave another parsing path where a forged SMTP command or header can hide.
        if [&message.to, &message.subject, &self.envelope_from]
            .iter()
            .any(|field| field.contains(['\r', '\n', '\0']))
        {
            return Err(MailError::HeaderInjection);
        }

        let address = (self.host.as_str(), self.port)
            .to_socket_addrs()?
            .next()
            .ok_or_else(|| io::Error::new(io::ErrorKind::AddrNotAvailable, "no relay address"))?;
        let mut stream = TcpStream::connect_timeout(&address, self.timeout)?;
        stream.set_read_timeout(Some(self.timeout))?;
        stream.set_write_timeout(Some(self.timeout))?;
        let mut replies = BufReader::new(stream.try_clone()?);

        expect_reply(&mut replies, "greeting", 2)?;
        send_command(&mut stream, "EHLO localhost\r\n")?;
        expect_reply(&mut replies, "EHLO", 2)?;
        send_command(
            &mut stream,
            &format!("MAIL FROM:<{}>\r\n", self.envelope_from),
        )?;
        expect_reply(&mut replies, "MAIL FROM", 2)?;
        send_command(&mut stream, &format!("RCPT TO:<{}>\r\n", message.to))?;
        expect_reply(&mut replies, "RCPT TO", 2)?;
        send_command(&mut stream, "DATA\r\n")?;
        expect_reply(&mut replies, "DATA", 3)?;

        write_message(&mut stream, message, &self.envelope_from)?;
        expect_reply(&mut replies, "message", 2)?;
        send_command(&mut stream, "QUIT\r\n")?;
        expect_reply(&mut replies, "QUIT", 2)?;
        Ok(())
    }
}

fn send_command(stream: &mut TcpStream, command: &str) -> Result<(), MailError> {
    stream.write_all(command.as_bytes())?;
    Ok(())
}

fn expect_reply(
    reader: &mut BufReader<TcpStream>,
    step: &'static str,
    class: u16,
) -> Result<(), MailError> {
    let mut reply = String::new();
    let mut first_status = None;
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line)? == 0 {
            return Err(MailError::Io(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "SMTP relay closed before its reply",
            )));
        }
        reply.push_str(&line);
        let bytes = line.as_bytes();
        if bytes.len() < 5
            || !bytes[..3].iter().all(u8::is_ascii_digit)
            || !matches!(bytes[3], b' ' | b'-')
            || !line.ends_with("\r\n")
        {
            return Err(MailError::Rejected { step, reply });
        }
        let status = u16::from(bytes[0] - b'0') * 100
            + u16::from(bytes[1] - b'0') * 10
            + u16::from(bytes[2] - b'0');
        if first_status.is_some_and(|first| first != status) {
            return Err(MailError::Rejected { step, reply });
        }
        first_status = Some(status);
        if bytes[3] == b' ' {
            if status / 100 == class {
                return Ok(());
            }
            return Err(MailError::Rejected { step, reply });
        }
    }
}

fn write_message(stream: &mut TcpStream, message: &Message, from: &str) -> Result<(), MailError> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| io::Error::other("system clock predates Unix epoch"))?;
    let date = rfc5322_date(now.as_secs())?;
    let sequence = MESSAGE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    // The Message-ID's domain is the ENVELOPE SENDER's, not `localhost`. A message id whose right
    // hand side names a domain that does not exist is a small but real spam signal, and the sender's
    // domain is the one thing here that is guaranteed to be routable — the relay rejects an
    // unverified sender outright (`universe/starbase/scripts/pve-mail-relay.sh`).
    let domain = from.rsplit_once('@').map_or("localhost", |(_, d)| d);
    let message_id = format!(
        "<{}.{sequence}.{}@{domain}>",
        now.as_nanos(),
        std::process::id()
    );
    // Header fields were checked before connecting. Date and Message-ID are generated here.
    write!(
        stream,
        "From: {from}\r\nTo: {}\r\nSubject: {}\r\nDate: {date}\r\nMessage-ID: {message_id}\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n",
        message.to, message.subject
    )?;
    // Normalize source line endings, then dot-stuff each line. A lone '.' must not terminate DATA.
    let body = message.body.replace("\r\n", "\n").replace('\r', "\n");
    for line in body.split('\n') {
        if line.starts_with('.') {
            stream.write_all(b".")?;
        }
        stream.write_all(line.as_bytes())?;
        stream.write_all(b"\r\n")?;
    }
    stream.write_all(b".\r\n")?;
    Ok(())
}

fn rfc5322_date(epoch_seconds: u64) -> Result<String, MailError> {
    let seconds = i64::try_from(epoch_seconds)
        .map_err(|_| io::Error::other("timestamp is too large for a mail date"))?;
    let days = seconds / 86_400;
    let day_seconds = seconds % 86_400;
    let weekday = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
        [usize::try_from((days + 4) % 7).map_err(|_| io::Error::other("invalid weekday"))?];
    // Gregorian civil-from-days conversion: leap years and century boundaries are handled before
    // formatting, so Date is an actual RFC 5322 UTC date rather than a Unix timestamp in a header.
    let z = days + 719_468;
    let era = z / 146_097;
    let day_of_era = z - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let mut year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_piece = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_piece + 2) / 5 + 1;
    let month = month_piece + if month_piece < 10 { 3 } else { -9 };
    year += i64::from(month <= 2);
    let month_name = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ][usize::try_from(month - 1).map_err(|_| io::Error::other("invalid month"))?];
    Ok(format!(
        "{weekday}, {day:02} {month_name} {year:04} {:02}:{:02}:{:02} +0000",
        day_seconds / 3_600,
        day_seconds % 3_600 / 60,
        day_seconds % 60
    ))
}

/// A thread-safe test double. Retains messages for assertions; it never prints their bodies.
#[derive(Default)]
pub struct CapturingMailer {
    sent: Mutex<Vec<Message>>,
}

impl CapturingMailer {
    #[must_use]
    pub fn sent(&self) -> Vec<Message> {
        self.sent
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }
}

impl Mailer for CapturingMailer {
    fn send(&self, message: &Message) -> Result<(), MailError> {
        self.sent
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .push(message.clone());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    fn message() -> Message {
        Message {
            to: "a@example.com".to_string(),
            subject: "A code".to_string(),
            body: "first\n.secret\nlast".to_string(),
        }
    }

    fn server_line(reader: &mut BufReader<TcpStream>) -> String {
        let mut line = String::new();
        reader.read_line(&mut line).unwrap();
        line
    }

    #[test]
    fn header_injection_in_subject_or_recipient_is_refused_before_connecting() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let relay = SmtpRelay::new(
            "127.0.0.1",
            listener.local_addr().unwrap().port(),
            "sender@example.com",
        );
        for field in ["bad\rheader", "bad\nheader", "bad\0header"] {
            let mut mail = message();
            mail.subject = field.to_string();
            assert!(matches!(relay.send(&mail), Err(MailError::HeaderInjection)));
            mail = message();
            mail.to = field.to_string();
            assert!(matches!(relay.send(&mail), Err(MailError::HeaderInjection)));
        }
        assert_eq!(
            listener.accept().unwrap_err().kind(),
            io::ErrorKind::WouldBlock
        );
    }

    #[test]
    fn smtp_dialogue_is_exactly_the_expected_sequence() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream.set_read_timeout(Some(TIMEOUT)).unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            stream.write_all(b"220 relay ready\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "EHLO localhost\r\n");
            stream
                .write_all(b"250-relay\r\n250 SIZE 100000\r\n")
                .unwrap();
            assert_eq!(
                server_line(&mut reader),
                "MAIL FROM:<sender@example.com>\r\n"
            );
            stream.write_all(b"250 sender ok\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "RCPT TO:<a@example.com>\r\n");
            stream.write_all(b"250 recipient ok\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "DATA\r\n");
            stream.write_all(b"354 send data\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "From: sender@example.com\r\n");
            assert_eq!(server_line(&mut reader), "To: a@example.com\r\n");
            assert_eq!(server_line(&mut reader), "Subject: A code\r\n");
            let date = server_line(&mut reader);
            assert!(date.starts_with("Date: ") && date.ends_with(" +0000\r\n"));
            let id = server_line(&mut reader);
            assert!(id.starts_with("Message-ID: <") && id.ends_with("@example.com>\r\n"));
            assert_eq!(
                server_line(&mut reader),
                "Content-Type: text/plain; charset=utf-8\r\n"
            );
            assert_eq!(server_line(&mut reader), "\r\n");
            assert_eq!(server_line(&mut reader), "first\r\n");
            assert_eq!(server_line(&mut reader), "..secret\r\n");
            assert_eq!(server_line(&mut reader), "last\r\n");
            assert_eq!(server_line(&mut reader), ".\r\n");
            stream.write_all(b"250 queued\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "QUIT\r\n");
            stream.write_all(b"221 bye\r\n").unwrap();
        });
        SmtpRelay::new("127.0.0.1", port, "sender@example.com")
            .send(&message())
            .unwrap();
        server.join().unwrap();
    }

    #[test]
    fn a_relay_that_rejects_rcpt_to_yields_a_rejected_error_naming_the_step() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream.set_read_timeout(Some(TIMEOUT)).unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            stream.write_all(b"220 relay ready\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "EHLO localhost\r\n");
            stream.write_all(b"250 hello\r\n").unwrap();
            assert_eq!(
                server_line(&mut reader),
                "MAIL FROM:<sender@example.com>\r\n"
            );
            stream.write_all(b"250 sender ok\r\n").unwrap();
            assert_eq!(server_line(&mut reader), "RCPT TO:<a@example.com>\r\n");
            stream.write_all(b"550 recipient rejected\r\n").unwrap();
        });
        let error = SmtpRelay::new("127.0.0.1", port, "sender@example.com")
            .send(&message())
            .unwrap_err();
        assert!(matches!(
            error,
            MailError::Rejected {
                step: "RCPT TO",
                ..
            }
        ));
        server.join().unwrap();
    }
}
