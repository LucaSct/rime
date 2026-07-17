// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The **editor channel** — the M9 messages in the reserved `0x0200..=0x02FF` band (ADR-0016/0031).
//! The engine host sends the component **schema** (so the editor can label inspectors and gate
//! compatibility by hash) and a full-world **snapshot**; the editor sends back component edits,
//! spawns, and despawns. Both blob formats are the engine's reflection-driven serialization
//! (engine/editorhost), mirrored here byte-for-byte:
//!
//! ```text
//! Schema   : [magic 'RSM1':u32][count:u32]  then count × [hash:u64][name_len:u16][name...]
//! Snapshot : [magic 'RSN1':u32][entities:u32] then per entity
//!            [index:u32][generation:u32][comp_count:u16] then per component
//!            [hash:u64][blob_len:u32][blob...]
//! SetComp  : [index:u32][generation:u32][hash:u64][blob_len:u32][blob...]
//! Despawn  : [index:u32][generation:u32]        Spawn: (empty payload)
//! ```
//!
//! Component blobs are opaque here — a payload keyed by its stable `type_hash`; interpreting the
//! fields is the inspector's job (a reflection-schema-driven view), not the transport's.

use crate::wire::{Reader, Writer};
use crate::{Error, Result};

const SCHEMA_MAGIC: u32 = 0x5253_4D31; // 'R''S''M''1'
const SNAPSHOT_MAGIC: u32 = 0x5253_4E31; // 'R''S''N''1'

/// An editor-channel message type (the `0x02xx` band). Mirrors `editorhost::EditorMessage`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditorMessage {
    /// engine → editor: the component registry (type_hash + name per reflected component).
    Schema,
    /// engine → editor: the whole world (entities + components).
    Snapshot,
    /// editor → engine: set a component's bytes on an entity.
    SetComponent,
    /// editor → engine: spawn an empty entity.
    Spawn,
    /// editor → engine: despawn an entity.
    Despawn,
}

impl EditorMessage {
    /// The `u16` wire code (also a valid [`crate::MessageType::Other`] code).
    pub fn to_code(self) -> u16 {
        match self {
            EditorMessage::Schema => 0x0200,
            EditorMessage::Snapshot => 0x0201,
            EditorMessage::SetComponent => 0x0210,
            EditorMessage::Spawn => 0x0211,
            EditorMessage::Despawn => 0x0212,
        }
    }

    /// The editor message for a wire code, or `None` if it is not one of ours.
    pub fn from_code(code: u16) -> Option<Self> {
        match code {
            0x0200 => Some(EditorMessage::Schema),
            0x0201 => Some(EditorMessage::Snapshot),
            0x0210 => Some(EditorMessage::SetComponent),
            0x0211 => Some(EditorMessage::Spawn),
            0x0212 => Some(EditorMessage::Despawn),
            _ => None,
        }
    }
}

/// One reflected component type in the schema: its stable fingerprint and source name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SchemaEntry {
    pub type_hash: u64,
    pub name: String,
}

/// The engine's component registry as sent to the editor.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Schema {
    pub types: Vec<SchemaEntry>,
}

impl Schema {
    /// Serialize the schema blob (the `Schema` message payload).
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u32(SCHEMA_MAGIC);
        w.u32(self.types.len() as u32);
        for e in &self.types {
            w.u64(e.type_hash);
            w.u16(e.name.len() as u16);
            w.bytes(e.name.as_bytes());
        }
        w.into_vec()
    }

    /// Parse a schema blob (from the engine, or [`Schema::encode`]).
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let magic = r.u32()?;
        if magic != SCHEMA_MAGIC {
            return Err(Error::BadTag(magic));
        }
        let count = r.u32()?;
        let mut types = Vec::with_capacity(count as usize);
        for _ in 0..count {
            let type_hash = r.u64()?;
            let name_len = r.u16()? as usize;
            let name = String::from_utf8_lossy(r.take_bytes(name_len)?).into_owned();
            types.push(SchemaEntry { type_hash, name });
        }
        Ok(Schema { types })
    }
}

/// One component on a snapshot entity: its `type_hash` and its still-serialized field bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SnapshotComponent {
    pub type_hash: u64,
    pub data: Vec<u8>,
}

/// One entity in a snapshot: its live handle (index, generation) and its reflected components.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SnapshotEntity {
    pub index: u32,
    pub generation: u32,
    pub components: Vec<SnapshotComponent>,
}

/// A full-world snapshot: the editor's view of the live scene.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Snapshot {
    pub entities: Vec<SnapshotEntity>,
}

impl Snapshot {
    /// Serialize the snapshot blob (the `Snapshot` message payload).
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u32(SNAPSHOT_MAGIC);
        w.u32(self.entities.len() as u32);
        for e in &self.entities {
            w.u32(e.index);
            w.u32(e.generation);
            w.u16(e.components.len() as u16);
            for c in &e.components {
                w.u64(c.type_hash);
                w.u32(c.data.len() as u32);
                w.bytes(&c.data);
            }
        }
        w.into_vec()
    }

    /// Parse a snapshot blob (from the engine, or [`Snapshot::encode`]).
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let magic = r.u32()?;
        if magic != SNAPSHOT_MAGIC {
            return Err(Error::BadTag(magic));
        }
        let entity_count = r.u32()?;
        let mut entities = Vec::with_capacity(entity_count as usize);
        for _ in 0..entity_count {
            let index = r.u32()?;
            let generation = r.u32()?;
            let comp_count = r.u16()?;
            let mut components = Vec::with_capacity(comp_count as usize);
            for _ in 0..comp_count {
                let type_hash = r.u64()?;
                let blob_len = r.u32()? as usize;
                let data = r.take_bytes(blob_len)?.to_vec();
                components.push(SnapshotComponent { type_hash, data });
            }
            entities.push(SnapshotEntity {
                index,
                generation,
                components,
            });
        }
        Ok(Snapshot { entities })
    }
}

/// An editor → engine "set this component's bytes on this entity" edit — the `SetComponent` payload.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SetComponent {
    pub index: u32,
    pub generation: u32,
    pub type_hash: u64,
    pub blob: Vec<u8>,
}

impl SetComponent {
    /// Serialize the `SetComponent` payload.
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u32(self.index);
        w.u32(self.generation);
        w.u64(self.type_hash);
        w.u32(self.blob.len() as u32);
        w.bytes(&self.blob);
        w.into_vec()
    }

    /// Parse a `SetComponent` payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let index = r.u32()?;
        let generation = r.u32()?;
        let type_hash = r.u64()?;
        let blob_len = r.u32()? as usize;
        let blob = r.take_bytes(blob_len)?.to_vec();
        Ok(SetComponent {
            index,
            generation,
            type_hash,
            blob,
        })
    }
}

/// Serialize a `Despawn` payload (an entity handle).
pub fn encode_despawn(index: u32, generation: u32) -> Vec<u8> {
    let mut w = Writer::new();
    w.u32(index);
    w.u32(generation);
    w.into_vec()
}
