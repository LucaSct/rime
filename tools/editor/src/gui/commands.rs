// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The editor's **command layer** (m9.4) — every mutation the UI performs is a [`Command`], and the
//! undoable ones are pushed onto a [`CommandStack`] as an inverse pair. Routing edits through one
//! typed place (instead of scattering `send_editor` calls through the widgets) is what makes undo,
//! redo, and the headless proof possible: the same commands the inspector issues are what a scripted
//! test issues, and each has an exact inverse.
//!
//! Only **component** edits are undoable, because only they have an exact inverse: setting a
//! component's bytes undoes to its previous bytes; adding a component undoes to removing it; removing
//! undoes to setting its old bytes back. Entity spawn/despawn are deliberately *not* on the stack —
//! the engine assigns a fresh handle on spawn, so a naive undo can't reproduce the identity edits
//! reference (a considered v1 limitation, noted in the PR; a spawn/despawn history is a later brick).

use rime_protocol::{encode_despawn, ComponentRef, EditorMessage, SetComponent};

/// An entity's live handle (index, generation) — how an edit names the entity it targets.
pub type EntityKey = (u32, u32);

/// One mutation of the world, as the editor sees it. Turned into a wire message by [`Command::to_wire`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Set a component's serialized bytes on an entity (the inspector's field edits, and the inverse
    /// of add/remove).
    SetComponent {
        key: EntityKey,
        type_hash: u64,
        blob: Vec<u8>,
    },
    /// Add a default-constructed component to an entity.
    AddComponent { key: EntityKey, type_hash: u64 },
    /// Remove a component from an entity.
    RemoveComponent { key: EntityKey, type_hash: u64 },
    /// Spawn a fresh (empty) entity.
    Spawn,
    /// Spawn an entity with an initial component set — the asset browser's "place" (m9.5). Each
    /// component is `(type_hash, reflection-serialized bytes)`.
    SpawnEntity { components: Vec<(u64, Vec<u8>)> },
    /// Despawn an entity.
    Despawn { key: EntityKey },
    /// Ask the engine to resend the world (after a structural change, to resync the mirror).
    RequestSnapshot,
    /// Begin (from Edit) or resume (from Paused) the simulation (m9.7).
    Play,
    /// Stop ticking; the viewport keeps rendering (m9.7).
    Pause,
    /// Run exactly one fixed tick, then stay Paused (m9.7).
    Step,
    /// Restore the pre-play snapshot; back to Edit (m9.7).
    Stop,
}

impl Command {
    /// The editor-channel message + payload bytes to put on the wire.
    pub fn to_wire(&self) -> (EditorMessage, Vec<u8>) {
        match self {
            Command::SetComponent {
                key,
                type_hash,
                blob,
            } => (
                EditorMessage::SetComponent,
                SetComponent {
                    index: key.0,
                    generation: key.1,
                    type_hash: *type_hash,
                    blob: blob.clone(),
                }
                .encode(),
            ),
            Command::AddComponent { key, type_hash } => (
                EditorMessage::AddComponent,
                component_ref(*key, *type_hash).encode(),
            ),
            Command::RemoveComponent { key, type_hash } => (
                EditorMessage::RemoveComponent,
                component_ref(*key, *type_hash).encode(),
            ),
            Command::Spawn => (EditorMessage::Spawn, Vec::new()),
            Command::SpawnEntity { components } => (
                EditorMessage::SpawnEntity,
                rime_protocol::SpawnEntity {
                    components: components.clone(),
                }
                .encode(),
            ),
            Command::Despawn { key } => (EditorMessage::Despawn, encode_despawn(key.0, key.1)),
            Command::RequestSnapshot => (EditorMessage::RequestSnapshot, Vec::new()),
            Command::Play => (EditorMessage::Play, Vec::new()),
            Command::Pause => (EditorMessage::Pause, Vec::new()),
            Command::Step => (EditorMessage::Step, Vec::new()),
            Command::Stop => (EditorMessage::Stop, Vec::new()),
        }
    }

    /// True for commands that change the world's *structure* (which entities/components exist), so the
    /// caller knows to request a fresh snapshot rather than trust an optimistic value patch.
    ///
    /// `Stop` counts: it restores the pre-play world wholesale (m9.7), so the mirror's entity
    /// handles are all stale the instant it lands and must be re-fetched. `Play`/`Pause`/`Step` do
    /// NOT — a tick moves component values (WorldTransform) on entities that already exist, which
    /// is exactly what an optimistic-patch model does not chase; the state-coloured viewport border
    /// (gui.rs) is the honest v1 signal that the inspector's numbers are not live during Play,
    /// rather than polling a snapshot every tick.
    pub fn is_structural(&self) -> bool {
        matches!(
            self,
            Command::AddComponent { .. }
                | Command::RemoveComponent { .. }
                | Command::Spawn
                | Command::SpawnEntity { .. }
                | Command::Despawn { .. }
                | Command::Stop
        )
    }
}

fn component_ref(key: EntityKey, type_hash: u64) -> ComponentRef {
    ComponentRef {
        index: key.0,
        generation: key.1,
        type_hash,
    }
}

/// An undoable edit: the forward command and its exact inverse. Undo issues the inverse; redo
/// re-issues the forward.
#[derive(Debug, Clone)]
pub struct Edit {
    pub forward: Command,
    pub inverse: Command,
}

/// A linear undo/redo history. `push` records a new edit and clears the redo branch (the usual
/// "editing after undo forks the timeline" rule). `undo`/`redo` return the command to apply.
#[derive(Default)]
pub struct CommandStack {
    undo: Vec<Edit>,
    redo: Vec<Edit>,
}

impl CommandStack {
    /// Record an applied edit. Anything previously undone is now unreachable.
    pub fn push(&mut self, edit: Edit) {
        self.undo.push(edit);
        self.redo.clear();
    }

    /// Move the last edit to the redo branch and return the command that undoes it (its inverse).
    pub fn undo(&mut self) -> Option<Command> {
        let edit = self.undo.pop()?;
        let inverse = edit.inverse.clone();
        self.redo.push(edit);
        Some(inverse)
    }

    /// Move the last undone edit back and return the command that re-applies it (its forward).
    pub fn redo(&mut self) -> Option<Command> {
        let edit = self.redo.pop()?;
        let forward = edit.forward.clone();
        self.undo.push(edit);
        Some(forward)
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(key: EntityKey, hash: u64, blob: &[u8]) -> Command {
        Command::SetComponent {
            key,
            type_hash: hash,
            blob: blob.to_vec(),
        }
    }

    #[test]
    fn undo_returns_the_inverse_and_redo_returns_the_forward() {
        let mut stack = CommandStack::default();
        let key = (1, 0);
        // Edit a component from bytes [1,2] to [3,4]; the inverse restores [1,2].
        stack.push(Edit {
            forward: set(key, 0xAA, &[3, 4]),
            inverse: set(key, 0xAA, &[1, 2]),
        });
        assert!(stack.can_undo() && !stack.can_redo());

        // Undo yields the inverse (restore [1,2]) — the exact previous bytes.
        assert_eq!(stack.undo(), Some(set(key, 0xAA, &[1, 2])));
        assert!(!stack.can_undo() && stack.can_redo());

        // Redo yields the forward again (re-apply [3,4]).
        assert_eq!(stack.redo(), Some(set(key, 0xAA, &[3, 4])));
        assert!(stack.can_undo() && !stack.can_redo());
    }

    #[test]
    fn a_new_edit_clears_the_redo_branch() {
        let mut stack = CommandStack::default();
        let key = (2, 1);
        stack.push(Edit {
            forward: set(key, 1, &[9]),
            inverse: set(key, 1, &[0]),
        });
        assert_eq!(stack.undo(), Some(set(key, 1, &[0])));
        assert!(stack.can_redo());
        // A fresh edit forks the timeline — redo is gone.
        stack.push(Edit {
            forward: set(key, 1, &[5]),
            inverse: set(key, 1, &[0]),
        });
        assert!(!stack.can_redo());
    }

    #[test]
    fn add_and_remove_map_to_the_right_wire_messages() {
        let (msg, _) = (Command::AddComponent {
            key: (3, 0),
            type_hash: 7,
        })
        .to_wire();
        assert_eq!(msg, EditorMessage::AddComponent);
        let (msg, payload) = (Command::RemoveComponent {
            key: (3, 0),
            type_hash: 7,
        })
        .to_wire();
        assert_eq!(msg, EditorMessage::RemoveComponent);
        // The payload is a ComponentRef the engine can parse back.
        let cr = ComponentRef::decode(&payload).expect("decode ref");
        assert_eq!((cr.index, cr.generation, cr.type_hash), (3, 0, 7));
    }
}
