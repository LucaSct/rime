// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//! Optional play-control handshake. Mirrors `editorhost/play_control.hpp`.
//!
//! The editor cannot assume a host has a player controller, or that a pointer is captured: the host
//! advertises `supported_modes` (bit0 Inspect required, bit1 Player optional) and every state and
//! command carries a `session_id`, so a stale command/reply is recognisable by the runtime. This
//! codec only carries the ids faithfully; judging staleness is not its job. A host that predates
//! these messages drops the unknown types, which the editor reads as "Inspect only".
use crate::wire::{Reader, Writer};
use crate::{Error, Result};

/// What the editor asks for (`PlayControl`, 0x021E, editor → engine).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum PlayControlAction {
    #[default]
    SelectInspect = 0,
    SelectPlayer = 1,
    Capture = 2,
    Release = 3,
}

impl PlayControlAction {
    fn from_u8(v: u8) -> Option<Self> {
        Some(match v {
            0 => Self::SelectInspect,
            1 => Self::SelectPlayer,
            2 => Self::Capture,
            3 => Self::Release,
            _ => return None,
        })
    }
}

/// Wire: `action:u8, session_id:u64`.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PlayControl {
    pub action: PlayControlAction,
    pub session_id: u64,
}

impl PlayControl {
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u8(self.action as u8);
        w.u64(self.session_id);
        w.into_vec()
    }
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let action = PlayControlAction::from_u8(r.u8()?).ok_or(Error::BadPlayControl)?;
        let session_id = r.u64()?;
        if r.remaining() != 0 {
            return Err(Error::BadPlayControl);
        }
        Ok(Self { action, session_id })
    }
}

/// `PlayControlState` (0x020B, engine → editor). mode: 0 Inspect, 1 Player; phase: 0 Edit,
/// 1 Playing, 2 Paused; status: 0 Ready, 1 Active, 2 Refused.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlayControlState {
    pub supported_modes: u8,
    pub mode: u8,
    pub phase: u8,
    pub status: u8,
    pub captured: bool,
    pub session_id: u64,
    pub tick_count: u64,
    pub reason: String,
}

impl Default for PlayControlState {
    /// Inspect-only host, Edit phase, Ready — the state of a host that says nothing.
    fn default() -> Self {
        Self {
            supported_modes: 1,
            mode: 0,
            phase: 0,
            status: 0,
            captured: false,
            session_id: 0,
            tick_count: 0,
            reason: String::new(),
        }
    }
}

impl PlayControlState {
    pub const MAX_REASON_BYTES: usize = 1024;

    fn valid(&self) -> bool {
        if self.supported_modes != 1 && self.supported_modes != 3 {
            return false;
        }
        if self.mode > 1 || (self.mode == 1 && self.supported_modes & 2 == 0) {
            return false;
        }
        if self.phase > 2 || self.status > 2 {
            return false;
        }
        if self.status != 1 && (self.phase != 0 || self.captured) {
            return false;
        }
        if self.status == 1 && (self.phase == 0 || self.session_id == 0) {
            return false;
        }
        if self.captured
            && (self.mode != 1 || self.phase != 1 || self.status != 1 || self.session_id == 0)
        {
            return false;
        }
        self.reason.len() <= Self::MAX_REASON_BYTES
    }

    pub fn encode(&self) -> Result<Vec<u8>> {
        if !self.valid() {
            return Err(Error::BadPlayControl);
        }
        let mut w = Writer::new();
        for v in [
            self.supported_modes,
            self.mode,
            self.phase,
            self.status,
            u8::from(self.captured),
        ] {
            w.u8(v);
        }
        w.u64(self.session_id);
        w.u64(self.tick_count);
        w.u32(self.reason.len() as u32);
        w.bytes(self.reason.as_bytes());
        Ok(w.into_vec())
    }

    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let (supported_modes, mode, phase, status) = (r.u8()?, r.u8()?, r.u8()?, r.u8()?);
        let captured = match r.u8()? {
            0 => false,
            1 => true,
            _ => return Err(Error::BadPlayControl),
        };
        let (session_id, tick_count) = (r.u64()?, r.u64()?);
        let len = r.u32()? as usize;
        if len > Self::MAX_REASON_BYTES || r.remaining() != len {
            return Err(Error::BadPlayControl);
        }
        // Strict UTF-8, same set the C++ validator accepts; invalid text is a bad packet, not
        // something to repair with replacement characters.
        let reason =
            String::from_utf8(r.take_bytes(len)?.to_vec()).map_err(|_| Error::BadPlayControl)?;
        let m = Self {
            supported_modes,
            mode,
            phase,
            status,
            captured,
            session_id,
            tick_count,
            reason,
        };
        if !m.valid() {
            return Err(Error::BadPlayControl);
        }
        Ok(m)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Same literals as tests/editorhost/play_control_test.cpp.
    const CMD: [u8; 9] = [1, 8, 7, 6, 5, 4, 3, 2, 1];
    const ACTIVE: [u8; 25] = [
        3, 1, 1, 1, 1, 10, 0, 0, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];
    const REFUSED: [u8; 27] = [
        3, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, b'n', b'o',
    ];

    fn st(sup: u8, mode: u8, phase: u8, status: u8, cap: bool, sid: u64) -> PlayControlState {
        PlayControlState {
            supported_modes: sup,
            mode,
            phase,
            status,
            captured: cap,
            session_id: sid,
            ..Default::default()
        }
    }

    #[test]
    fn command_bytes_and_actions() {
        let m = PlayControl {
            action: PlayControlAction::SelectPlayer,
            session_id: 0x0102030405060708,
        };
        assert_eq!(m.encode(), CMD);
        assert_eq!(PlayControl::decode(&CMD).unwrap(), m);
        for a in 0..=3u8 {
            let m = PlayControl {
                action: PlayControlAction::from_u8(a).unwrap(),
                session_id: u64::MAX,
            };
            assert_eq!(PlayControl::decode(&m.encode()).unwrap(), m);
        }
        for n in 0..CMD.len() {
            assert!(PlayControl::decode(&CMD[..n]).is_err());
        }
        let mut extra = CMD.to_vec();
        extra.push(0);
        assert!(PlayControl::decode(&extra).is_err());
        let mut bad = CMD;
        bad[0] = 4;
        assert!(PlayControl::decode(&bad).is_err());
    }

    #[test]
    fn state_bytes_roundtrip_and_stale_ids() {
        let mut a = st(3, 1, 1, 1, true, 10);
        a.tick_count = 0x0102;
        assert_eq!(a.encode().unwrap(), ACTIVE);
        assert_eq!(PlayControlState::decode(&ACTIVE).unwrap(), a);
        let mut r = st(3, 0, 0, 2, false, 0);
        r.reason = "no".into();
        assert_eq!(r.encode().unwrap(), REFUSED);
        assert_eq!(PlayControlState::decode(&REFUSED).unwrap(), r);

        let mut stale = st(3, 1, 0, 2, false, 3);
        stale.reason = "player unavailable \u{2713}".into();
        stale.tick_count = 123456789012;
        for s in [
            PlayControlState::default(),
            st(3, 1, 0, 0, false, 0),
            st(3, 1, 1, 1, false, 7),
            st(3, 1, 2, 1, false, 7),
            st(1, 0, 1, 1, false, u64::MAX),
            stale,
        ] {
            assert_eq!(PlayControlState::decode(&s.encode().unwrap()).unwrap(), s);
        }
    }

    #[test]
    fn inconsistent_states_refused() {
        for s in [
            st(2, 0, 0, 0, false, 0),
            st(0, 0, 0, 0, false, 0),
            st(7, 0, 0, 0, false, 0),
            st(1, 1, 0, 0, false, 0),
            st(3, 2, 0, 0, false, 0),
            st(3, 0, 3, 0, false, 0),
            st(3, 0, 0, 3, false, 0),
            st(3, 1, 1, 0, false, 0),
            st(3, 1, 1, 2, false, 0),
            st(3, 1, 0, 0, true, 0),
            st(3, 1, 0, 2, true, 0),
            st(3, 1, 0, 1, false, 4),
            st(3, 1, 1, 1, false, 0),
            st(3, 0, 1, 1, true, 4),
            st(3, 1, 2, 1, true, 4),
        ] {
            assert!(s.encode().is_err(), "{s:?}");
        }
        for (at, v) in [(4, 2), (2, 2), (0, 1), (5, 0)] {
            let mut b = ACTIVE;
            if at == 2 {
                b[4] = 1;
            }
            b[at] = v;
            assert!(PlayControlState::decode(&b).is_err(), "byte {at}");
        }
    }

    #[test]
    fn truncation_extra_cap_and_utf8() {
        for n in 0..REFUSED.len() {
            assert!(PlayControlState::decode(&REFUSED[..n]).is_err());
        }
        let mut extra = REFUSED.to_vec();
        extra.push(0);
        assert!(PlayControlState::decode(&extra).is_err());
        let mut s = PlayControlState {
            reason: "x".repeat(1024),
            ..Default::default()
        };
        let b = s.encode().unwrap();
        assert_eq!(b.len(), 25 + 1024);
        assert!(PlayControlState::decode(&b).is_ok());
        s.reason.push('x');
        assert!(s.encode().is_err());
        let mut over = b.clone();
        over[21] = 1;
        over[22] = 4;
        over.push(b'x');
        assert!(PlayControlState::decode(&over).is_err());
        let mut bad = REFUSED;
        bad[25] = 0xFF;
        assert!(PlayControlState::decode(&bad).is_err());
        for bytes in [
            &[0xC3u8][..],
            &[0xC0, 0x80],
            &[0xED, 0xA0, 0x80],
            &[0xF4, 0x90, 0x80, 0x80],
        ] {
            let mut b = REFUSED[..25].to_vec();
            b[25 - 4..].copy_from_slice(&(bytes.len() as u32).to_le_bytes());
            b.extend_from_slice(bytes);
            assert!(PlayControlState::decode(&b).is_err());
        }
    }

    #[test]
    fn message_tags() {
        use crate::editor::EditorMessage;
        assert_eq!(EditorMessage::PlayControlState.to_code(), 0x020B);
        assert_eq!(EditorMessage::PlayControl.to_code(), 0x021E);
        assert_eq!(
            EditorMessage::from_code(0x021E),
            Some(EditorMessage::PlayControl)
        );
        assert_eq!(
            EditorMessage::from_code(0x020B),
            Some(EditorMessage::PlayControlState)
        );
    }
}
