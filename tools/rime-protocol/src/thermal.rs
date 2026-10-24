// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//! Optional readonly thermal preview v1. Mirrors editorhost/thermal_preview.hpp, separate from
//! authored component blobs so Save/undo cannot capture evolving solver state.
use crate::wire::{Reader, Writer};
use crate::{Error, Result};

#[derive(Debug, Clone, PartialEq)]
pub struct ThermalPreviewCell {
    pub editor_id: u64,
    pub temperature_kelvin: f64,
    pub liquid_mass_fraction: f64,
    pub energy_joules: f64,
}
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ThermalPreview {
    /// Empty, Ready, InvalidDescription, CapacityExceeded, StepRefused.
    pub status: u8,
    pub phase: u8,
    pub field_status: u8,
    pub heat_status: u8,
    pub contact_status: u8,
    pub session_id: u64,
    pub revision: u64,
    pub host_tick: u64,
    pub accepted_host_tick: u64,
    pub field_tick: u64,
    pub problem_editor_id: u64,
    pub failed_substep: u32,
    pub failed_descriptor: u32,
    pub cells: Vec<ThermalPreviewCell>,
}
impl ThermalPreview {
    pub const MAX_CELLS: usize = 16;
    fn valid(&self) -> bool {
        if self.status > 4
            || self.phase > 2
            || self.field_status > 8
            || self.heat_status > 4
            || self.contact_status > 6
            || self.cells.len() > Self::MAX_CELLS
            || self.accepted_host_tick > self.host_tick
        {
            return false;
        }
        if matches!(self.status, 0 | 2 | 3) && !self.cells.is_empty() {
            return false;
        }
        if self.status == 1 && self.cells.is_empty() {
            return false;
        }
        if self.phase == 0
            && (self.status != 0
                || self.host_tick != 0
                || self.field_tick != 0
                || self.accepted_host_tick != 0)
        {
            return false;
        }
        self.cells.iter().enumerate().all(|(i, c)| {
            c.editor_id != 0
                && c.temperature_kelvin.is_finite()
                && c.temperature_kelvin > 0.
                && c.liquid_mass_fraction.is_finite()
                && (0. ..=1.).contains(&c.liquid_mass_fraction)
                && c.energy_joules.is_finite()
                && !self.cells[..i]
                    .iter()
                    .any(|old| old.editor_id == c.editor_id)
        })
    }
    pub fn encode(&self) -> Result<Vec<u8>> {
        if !self.valid() {
            return Err(Error::BadThermalPreview);
        }
        let mut w = Writer::new();
        for v in [
            1,
            self.status,
            self.phase,
            self.field_status,
            self.heat_status,
            self.contact_status,
        ] {
            w.u8(v);
        }
        for v in [
            self.session_id,
            self.revision,
            self.host_tick,
            self.accepted_host_tick,
            self.field_tick,
            self.problem_editor_id,
        ] {
            w.u64(v);
        }
        w.u32(self.failed_substep);
        w.u32(self.failed_descriptor);
        w.u8(self.cells.len() as u8);
        for c in &self.cells {
            w.u64(c.editor_id);
            w.f64(c.temperature_kelvin);
            w.f64(c.liquid_mass_fraction);
            w.f64(c.energy_joules);
        }
        Ok(w.into_vec())
    }
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        if r.u8()? != 1 {
            return Err(Error::BadThermalPreview);
        }
        let mut m = Self {
            status: r.u8()?,
            phase: r.u8()?,
            field_status: r.u8()?,
            heat_status: r.u8()?,
            contact_status: r.u8()?,
            session_id: r.u64()?,
            revision: r.u64()?,
            host_tick: r.u64()?,
            accepted_host_tick: r.u64()?,
            field_tick: r.u64()?,
            problem_editor_id: r.u64()?,
            failed_substep: r.u32()?,
            failed_descriptor: r.u32()?,
            cells: vec![],
        };
        let count = r.u8()? as usize;
        if count > Self::MAX_CELLS || r.remaining() != 32 * count {
            return Err(Error::BadThermalPreview);
        }
        m.cells.reserve(count);
        for _ in 0..count {
            m.cells.push(ThermalPreviewCell {
                editor_id: r.u64()?,
                temperature_kelvin: r.f64()?,
                liquid_mass_fraction: r.f64()?,
                energy_joules: r.f64()?,
            });
        }
        if !m.valid() {
            return Err(Error::BadThermalPreview);
        }
        Ok(m)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bounded_roundtrip_and_malformed_packets() {
        let m = ThermalPreview {
            status: 1,
            phase: 2,
            session_id: 1,
            revision: 2,
            host_tick: 12,
            accepted_host_tick: 12,
            field_tick: 12,
            cells: vec![ThermalPreviewCell {
                editor_id: 7,
                temperature_kelvin: 273.15,
                liquid_mass_fraction: 0.25,
                energy_joules: 83375.,
            }],
            ..Default::default()
        };
        let bytes = m.encode().unwrap();
        assert_eq!(
            bytes,
            include_bytes!("../tests/fixtures/thermal_preview.bin")
        );
        assert_eq!(bytes.len(), 95);
        assert_eq!(ThermalPreview::decode(&bytes).unwrap(), m);
        for n in 0..bytes.len() {
            assert!(ThermalPreview::decode(&bytes[..n]).is_err());
        }
        let mut bad = bytes.clone();
        bad.push(0);
        assert!(ThermalPreview::decode(&bad).is_err());
        for (at, value) in [(0, 2), (1, 5), (2, 3), (3, 9), (4, 5), (5, 7), (62, 17)] {
            let mut bad = bytes.clone();
            bad[at] = value;
            assert!(ThermalPreview::decode(&bad).is_err());
        }
        let mut duplicate = m.clone();
        duplicate.cells.push(m.cells[0].clone());
        assert!(duplicate.encode().is_err());
        let mut bad = m.clone();
        bad.cells[0].temperature_kelvin = f64::NAN;
        assert!(bad.encode().is_err());
        for at in [71, 79, 87] {
            let mut bad = bytes.clone();
            bad[at..at + 8].copy_from_slice(&f64::NAN.to_le_bytes());
            assert!(ThermalPreview::decode(&bad).is_err());
        }
        let mut stopped = m.clone();
        stopped.phase = 0;
        assert!(stopped.encode().is_err());
    }
}
