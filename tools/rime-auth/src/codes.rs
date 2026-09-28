// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Short mailed codes for registration and two-proof recovery.
//!
//! A code proves access to a mailbox for one purpose and address. It cannot authenticate an
//! existing account on its own: ADR-0048 requires a passkey for login and a separate recovery
//! secret alongside a mailed code when every passkey is lost.

use std::fmt;
use std::io;
use std::time::Duration;

use hmac::{Hmac, Mac};
use sha2::Sha256;
use subtle::ConstantTimeEq;

use crate::fill_random;

/// Ten minutes keeps a delayed mail useful without leaving a short code valid all day.
pub const DEFAULT_CODE_TTL: Duration = Duration::from_secs(10 * 60);

/// Five guesses limit online attacks against an eight-digit code; exhaustion is terminal.
pub const DEFAULT_CODE_ATTEMPTS: u8 = 5;

/// Process-lifetime secret used to MAC short codes before they are held by the caller.
pub struct CodeKey([u8; 32]);

impl CodeKey {
    /// A fresh key must come from the OS; weak fallback entropy would weaken every code.
    pub fn generate() -> Result<Self, io::Error> {
        let mut bytes = [0; 32];
        fill_random(&mut bytes)?;
        Ok(Self(bytes))
    }
}

/// The code to deliver once. The stored challenge never contains these digits.
pub struct MailCode(String);

impl MailCode {
    /// The digits to put in the mail. Keep this out of logs.
    #[must_use]
    pub fn expose(&self) -> &str {
        &self.0
    }
}

// A derived Debug would turn an innocent trace of the return value into a leaked code.
impl fmt::Debug for MailCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "MailCode(<redacted>)")
    }
}

/// What a caller keeps while waiting for the mailed code; no plaintext code is stored here.
#[derive(Debug, Clone)]
pub struct CodeChallenge {
    pub purpose: &'static str,
    pub mac: [u8; 32],
    pub expires_at: u64,
    pub attempts_remaining: u8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CodeOutcome {
    Accepted,
    Rejected,
    Expired,
    Exhausted,
}

/// Mint eight unbiased decimal digits and bind them to the purpose and exact address.
pub fn mint(
    key: &CodeKey,
    purpose: &'static str,
    email: &str,
    now: u64,
    ttl: Duration,
) -> Result<(MailCode, CodeChallenge), io::Error> {
    let mut code = String::with_capacity(8);
    let mut random = [0u8; 32];
    while code.len() < 8 {
        fill_random(&mut random)?;
        for byte in random {
            if byte < 250 {
                // 256 is not divisible by 10: `byte % 10` over all bytes favours six digits.
                // Reject 250..=255 so each digit has exactly 25 source values.
                code.push(char::from(b'0' + byte % 10));
                if code.len() == 8 {
                    break;
                }
            }
        }
    }
    let mac = code_mac(key, purpose, email, &code)?;
    Ok((
        MailCode(code),
        CodeChallenge {
            purpose,
            mac,
            expires_at: now.saturating_add(ttl.as_secs()),
            attempts_remaining: DEFAULT_CODE_ATTEMPTS,
        },
    ))
}

/// Verify one guess. Expiry and exhaustion are checked before touching the guess.
pub fn verify(
    challenge: &mut CodeChallenge,
    key: &CodeKey,
    email: &str,
    candidate: &str,
    now: u64,
) -> CodeOutcome {
    if now >= challenge.expires_at {
        // Record terminal expiry even if a later caller supplies an earlier clock value.
        challenge.attempts_remaining = 0;
        return CodeOutcome::Expired;
    }
    if challenge.attempts_remaining == 0 {
        return CodeOutcome::Exhausted;
    }
    let matches = code_mac(key, challenge.purpose, email, candidate)
        .is_ok_and(|mac| bool::from(challenge.mac.ct_eq(&mac)));
    if matches {
        // A successful challenge is consumed too; reusing a mailed proof should not work.
        challenge.attempts_remaining = 0;
        return CodeOutcome::Accepted;
    }
    challenge.attempts_remaining -= 1;
    if challenge.attempts_remaining == 0 {
        CodeOutcome::Exhausted
    } else {
        CodeOutcome::Rejected
    }
}

fn code_mac(key: &CodeKey, purpose: &str, email: &str, code: &str) -> Result<[u8; 32], io::Error> {
    // HMAC accepts keys of any length. Keep its Result as an error value anyway: no panic in a
    // library path, even if that API changes.
    let mut mac = Hmac::<Sha256>::new_from_slice(&key.0)
        .map_err(|_| io::Error::other("HMAC refused the fixed-size code key"))?;
    mac.update(purpose.as_bytes());
    mac.update(&[0]);
    mac.update(email.as_bytes());
    mac.update(&[0]);
    mac.update(code.as_bytes());
    Ok(mac.finalize().into_bytes().into())
}

// Like brick 1's tests, entropy tests only run where fill_random has a real OS source.
#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[test]
    fn mint_then_verify_accepts() {
        let key = CodeKey::generate().unwrap();
        let (code, mut challenge) =
            mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
        assert_eq!(challenge.attempts_remaining, DEFAULT_CODE_ATTEMPTS);
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 101),
            CodeOutcome::Accepted
        );
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 102),
            CodeOutcome::Exhausted
        );
        assert_eq!(format!("{code:?}"), "MailCode(<redacted>)");
    }

    #[test]
    fn verify_rejects_a_wrong_code_and_decrements_attempts() {
        let key = CodeKey::generate().unwrap();
        let (code, mut challenge) =
            mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
        let wrong = if code.expose() == "00000000" {
            "11111111"
        } else {
            "00000000"
        };
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", wrong, 101),
            CodeOutcome::Rejected
        );
        assert_eq!(challenge.attempts_remaining, DEFAULT_CODE_ATTEMPTS - 1);
    }

    #[test]
    fn expired_challenge_rejects_even_the_correct_code() {
        let key = CodeKey::generate().unwrap();
        let (code, mut challenge) =
            mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 700),
            CodeOutcome::Expired
        );
        assert_eq!(challenge.attempts_remaining, 0);
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 699),
            CodeOutcome::Exhausted
        );
    }

    #[test]
    fn fifth_wrong_attempt_exhausts_and_a_correct_code_after_it_still_fails() {
        let key = CodeKey::generate().unwrap();
        let (code, mut challenge) =
            mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
        let wrong = if code.expose() == "00000000" {
            "11111111"
        } else {
            "00000000"
        };
        for _ in 0..4 {
            assert_eq!(
                verify(&mut challenge, &key, "a@example.com", wrong, 101),
                CodeOutcome::Rejected
            );
        }
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", wrong, 101),
            CodeOutcome::Exhausted
        );
        assert_eq!(challenge.attempts_remaining, 0);
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 101),
            CodeOutcome::Exhausted
        );
    }

    #[test]
    fn a_code_minted_for_one_email_does_not_verify_for_another() {
        let key = CodeKey::generate().unwrap();
        let (code, mut challenge) =
            mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
        assert_eq!(
            verify(&mut challenge, &key, "b@example.com", code.expose(), 101),
            CodeOutcome::Rejected
        );
        assert_eq!(
            verify(&mut challenge, &key, "a@example.com", code.expose(), 101),
            CodeOutcome::Accepted
        );
    }

    #[test]
    fn ten_thousand_codes_are_eight_digits_and_every_digit_appears_in_every_position() {
        let key = CodeKey::generate().unwrap();
        let mut seen = [[false; 10]; 8];
        for _ in 0..10_000 {
            let (code, _) = mint(&key, "register", "a@example.com", 100, DEFAULT_CODE_TTL).unwrap();
            assert_eq!(code.expose().len(), 8);
            for (position, byte) in code.expose().bytes().enumerate() {
                assert!(byte.is_ascii_digit());
                seen[position][usize::from(byte - b'0')] = true;
            }
        }
        assert!(seen
            .iter()
            .all(|position| position.iter().all(|digit| *digit)));
    }
}
