// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The **editor channel** — the M9 messages in the reserved `0x0200..=0x02FF` band (ADR-0016/0031).
//! The engine host sends the reflected-type **schema** and a full-world **snapshot**; the editor
//! sends back component edits, spawns/despawns, add/remove-component, and snapshot requests. Both
//! blob formats are the engine's reflection-driven serialization (engine/editorhost), mirrored here
//! byte-for-byte:
//!
//! ```text
//! Schema   : [magic 'RSM2':u32][type_count:u32] then per type
//!            [hash:u64][name_len:u16][name...][is_component:u8][field_count:u16] then per field
//!            [name_len:u16][name...][kind:u8][nested_hash:u64]   (nested_hash 0 unless kind=Struct)
//! Snapshot : [magic 'RSN1':u32][entities:u32] then per entity
//!            [index:u32][generation:u32][comp_count:u16] then per component
//!            [hash:u64][blob_len:u32][blob...]
//! SetComp  : [index:u32][generation:u32][hash:u64][blob_len:u32][blob...]
//! Add/Remove: [index:u32][generation:u32][hash:u64]     Despawn: [index:u32][generation:u32]
//! Spawn / RequestSnapshot : (empty payload)
//! ```
//!
//! The schema (m9.4) is the whole point: it carries each type's **field layout**, so a snapshot's
//! opaque component blob is decoded into typed, editable fields ([`decode_value`]) and an edit is
//! re-encoded ([`encode_value`]) — the reflection-driven inspector, with no per-type code. `kind` is
//! the engine's `core::FieldType` tag; `Struct` fields recurse into `nested_hash`'s type.

use std::collections::HashMap;

use crate::wire::{Reader, Writer};
use crate::{Error, Result};

const SCHEMA_MAGIC: u32 = 0x5253_4D32; // 'R''S''M''2' (m9.4: now carries field layout)
const SNAPSHOT_MAGIC: u32 = 0x5253_4E31; // 'R''S''N''1'

/// An editor-channel message type (the `0x02xx` band). Mirrors `editorhost::EditorMessage`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditorMessage {
    /// engine → editor: the reflected-type dictionary (layout per type; the inspector is built from
    /// it).
    Schema,
    /// engine → editor: the whole world (entities + components).
    Snapshot,
    /// editor → engine: set a component's bytes on an entity.
    SetComponent,
    /// editor → engine: spawn an empty entity.
    Spawn,
    /// editor → engine: despawn an entity.
    Despawn,
    /// editor → engine: add a **default-constructed** component to an entity (defaults come from the
    /// engine, not a zeroed blob).
    AddComponent,
    /// editor → engine: remove a component from an entity.
    RemoveComponent,
    /// editor → engine: resend the world — the engine replies with a fresh [`EditorMessage::Snapshot`].
    RequestSnapshot,
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
            EditorMessage::AddComponent => 0x0213,
            EditorMessage::RemoveComponent => 0x0214,
            EditorMessage::RequestSnapshot => 0x0215,
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
            0x0213 => Some(EditorMessage::AddComponent),
            0x0214 => Some(EditorMessage::RemoveComponent),
            0x0215 => Some(EditorMessage::RequestSnapshot),
            _ => None,
        }
    }
}

/// A primitive field kind — the wire mirror of the engine's `core::FieldType` (declared order is the
/// wire tag). `Struct` means "recurse into another reflected type" (see [`FieldDesc::nested_hash`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldKind {
    Bool,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    Struct,
}

impl FieldKind {
    /// The `u8` wire tag (== the engine's `core::FieldType` enumerator value).
    pub fn to_u8(self) -> u8 {
        match self {
            FieldKind::Bool => 0,
            FieldKind::I32 => 1,
            FieldKind::U32 => 2,
            FieldKind::I64 => 3,
            FieldKind::U64 => 4,
            FieldKind::F32 => 5,
            FieldKind::F64 => 6,
            FieldKind::Struct => 7,
        }
    }

    /// The kind for a wire tag, or `None` for a byte this build does not know.
    pub fn from_u8(v: u8) -> Option<Self> {
        Some(match v {
            0 => FieldKind::Bool,
            1 => FieldKind::I32,
            2 => FieldKind::U32,
            3 => FieldKind::I64,
            4 => FieldKind::U64,
            5 => FieldKind::F32,
            6 => FieldKind::F64,
            7 => FieldKind::Struct,
            _ => return None,
        })
    }
}

/// One reflected field: its source name, primitive kind, and — for a `Struct` kind — the `type_hash`
/// of the nested type to recurse into (0 otherwise).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FieldDesc {
    pub name: String,
    pub kind: FieldKind,
    pub nested_hash: u64,
}

/// One reflected type in the schema: its stable fingerprint, source name, whether it is a top-level
/// component (vs. a nested value type like `Vec3`), and its ordered fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SchemaEntry {
    pub type_hash: u64,
    pub name: String,
    pub is_component: bool,
    pub fields: Vec<FieldDesc>,
}

/// The engine's reflected-type dictionary as sent to the editor — components and the nested value
/// types they contain, each with its field layout. The editor decodes/edits component blobs against
/// it (see [`decode_value`] / [`encode_value`]).
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
        for t in &self.types {
            w.u64(t.type_hash);
            write_name(&mut w, &t.name);
            w.u8(u8::from(t.is_component));
            w.u16(t.fields.len() as u16);
            for f in &t.fields {
                write_name(&mut w, &f.name);
                w.u8(f.kind.to_u8());
                w.u64(f.nested_hash);
            }
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
            let name = read_name(&mut r)?;
            let is_component = r.u8()? != 0;
            let field_count = r.u16()? as usize;
            let mut fields = Vec::with_capacity(field_count);
            for _ in 0..field_count {
                let name = read_name(&mut r)?;
                let kind = FieldKind::from_u8(r.u8()?).ok_or(Error::Truncated)?;
                let nested_hash = r.u64()?;
                fields.push(FieldDesc {
                    name,
                    kind,
                    nested_hash,
                });
            }
            types.push(SchemaEntry {
                type_hash,
                name,
                is_component,
                fields,
            });
        }
        Ok(Schema { types })
    }

    /// Find a type by its stable `type_hash` (used to resolve a component or a nested Struct field).
    pub fn type_by_hash(&self, type_hash: u64) -> Option<&SchemaEntry> {
        self.types.iter().find(|t| t.type_hash == type_hash)
    }

    /// A `type_hash → SchemaEntry` map for repeated lookups (the inspector builds one per schema).
    pub fn by_hash(&self) -> HashMap<u64, &SchemaEntry> {
        self.types.iter().map(|t| (t.type_hash, t)).collect()
    }
}

/// A decoded field value — the typed tree an inspector edits. Mirrors the reflected kinds; a `Struct`
/// carries its child fields as `(name, value)` in declared order. Not `Eq` (floats), but `PartialEq`
/// makes "did this component actually change?" checks easy.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Bool(bool),
    I32(i32),
    U32(u32),
    I64(i64),
    U64(u64),
    F32(f32),
    F64(f64),
    Struct(Vec<(String, Value)>),
}

/// Decode a component's packed blob into a typed [`Value`] tree, using `schema` to resolve the root
/// type and any nested Struct fields. The bytes are the engine's reflection serialization (packed
/// little-endian, fields in declared order, no padding), so this is its exact inverse. Errors if the
/// type is unknown to the schema or the blob is short.
pub fn decode_value(schema: &Schema, type_hash: u64, blob: &[u8]) -> Result<Value> {
    let entry = schema.type_by_hash(type_hash).ok_or(Error::BadTag(0))?;
    let mut r = Reader::new(blob);
    read_struct(schema, entry, &mut r)
}

/// Re-encode a [`Value`] tree (from [`decode_value`], after edits) into the packed blob the engine
/// expects. The tree already carries the full structure, so no schema is needed — it just walks the
/// values in order, exactly matching the C++ serializer.
pub fn encode_value(value: &Value) -> Vec<u8> {
    let mut w = Writer::new();
    write_value(&mut w, value);
    w.into_vec()
}

fn read_struct(schema: &Schema, entry: &SchemaEntry, r: &mut Reader) -> Result<Value> {
    let mut fields = Vec::with_capacity(entry.fields.len());
    for f in &entry.fields {
        fields.push((f.name.clone(), read_field(schema, f, r)?));
    }
    Ok(Value::Struct(fields))
}

fn read_field(schema: &Schema, f: &FieldDesc, r: &mut Reader) -> Result<Value> {
    Ok(match f.kind {
        FieldKind::Bool => Value::Bool(r.u8()? != 0),
        FieldKind::I32 => Value::I32(r.i32()?),
        FieldKind::U32 => Value::U32(r.u32()?),
        FieldKind::I64 => Value::I64(r.i64()?),
        FieldKind::U64 => Value::U64(r.u64()?),
        FieldKind::F32 => Value::F32(r.f32()?),
        FieldKind::F64 => Value::F64(r.f64()?),
        FieldKind::Struct => {
            let nested = schema.type_by_hash(f.nested_hash).ok_or(Error::BadTag(0))?;
            read_struct(schema, nested, r)?
        }
    })
}

fn write_value(w: &mut Writer, v: &Value) {
    match v {
        Value::Bool(b) => w.u8(u8::from(*b)),
        Value::I32(x) => w.i32(*x),
        Value::U32(x) => w.u32(*x),
        Value::I64(x) => w.i64(*x),
        Value::U64(x) => w.u64(*x),
        Value::F32(x) => w.f32(*x),
        Value::F64(x) => w.f64(*x),
        Value::Struct(fields) => {
            for (_, fv) in fields {
                write_value(w, fv);
            }
        }
    }
}

/// Write a length-prefixed name ([len:u16][utf8]).
fn write_name(w: &mut Writer, name: &str) {
    w.u16(name.len() as u16);
    w.bytes(name.as_bytes());
}

/// Read a length-prefixed name ([len:u16][utf8]); invalid UTF-8 is replaced, never an error.
fn read_name(r: &mut Reader) -> Result<String> {
    let len = r.u16()? as usize;
    Ok(String::from_utf8_lossy(r.take_bytes(len)?).into_owned())
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

/// An editor → engine reference to one component on one entity — the payload shared by
/// `AddComponent` and `RemoveComponent`: `[index:u32][generation:u32][hash:u64]`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ComponentRef {
    pub index: u32,
    pub generation: u32,
    pub type_hash: u64,
}

impl ComponentRef {
    /// Serialize the payload.
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u32(self.index);
        w.u32(self.generation);
        w.u64(self.type_hash);
        w.into_vec()
    }

    /// Parse the payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let index = r.u32()?;
        let generation = r.u32()?;
        let type_hash = r.u64()?;
        Ok(ComponentRef {
            index,
            generation,
            type_hash,
        })
    }
}
