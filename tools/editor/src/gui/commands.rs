// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The editor's **command layer** (m9.4) — every mutation the UI performs is a [`Command`], and the
//! undoable ones are pushed onto a [`CommandStack`] as an inverse pair. Routing edits through one
//! typed place (instead of scattering `send_editor` calls through the widgets) is what makes undo,
//! redo, and the headless proof possible: the same commands the inspector issues are what a scripted
//! test issues, and each has an exact inverse.
//!
//! **Entities are named by [`EntityId`] — the engine host's `EditorId` (ADR-0075) — never by the
//! engine's `(index, generation)` handle.** A handle dies with a despawn and may be replaced by a
//! Play→Stop; an EditorId is stamped once and never reused, and the engine gives an undone despawn
//! its old id back. That is what lets every structural edit be undoable:
//!
//! | edit | inverse |
//! |---|---|
//! | set a component's bytes | set the previous bytes |
//! | add a component | remove it |
//! | remove a component | set its old bytes back (which re-adds it) |
//! | spawn / place | despawn (redo respawns under the same id) |
//! | despawn | an exact respawn under the same id, then every reference other entities held to it |
//!
//! Structural edits are committed to the history only once the engine has **acknowledged** them
//! (its `EditResult`, one per structural command, in send order) — a refused edit leaves no step,
//! and a spawn's step needs the id the engine assigned. [`CommandStack::expect`] queues what to do
//! with each answer; [`CommandStack::acknowledge`] applies it.
//!
//! **The history is frozen while the simulation runs** ([`CommandStack::set_frozen`]): nothing is
//! recorded and undo/redo are refused, because Stop throws away everything play did — an undo step
//! recorded against the played world would undo something that no longer exists.

use std::collections::VecDeque;

use rime_protocol::{
    encode_entity_ref, ComponentRef, EditResult, EditorMessage, SaveScene, SetComponent,
};

/// An entity's stable name: the `EditorId` the engine host stamped on it (ADR-0075). `0` names
/// nothing — it is the wire's null reference, and a [`Command::Spawn`]'s "give me a fresh one".
pub type EntityId = u64;

/// One mutation of the world, as the editor sees it. Turned into a wire message by [`Command::to_wire`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Set a component's serialized bytes on an entity (the inspector's field edits, and the inverse
    /// of add/remove). Entity fields inside `blob` are EntityIds too.
    SetComponent {
        id: EntityId,
        type_hash: u64,
        blob: Vec<u8>,
    },
    /// Add a default-constructed component to an entity.
    AddComponent { id: EntityId, type_hash: u64 },
    /// Remove a component from an entity.
    RemoveComponent { id: EntityId, type_hash: u64 },
    /// Spawn an entity with the engine's default placement. `id: 0` asks for a fresh id; a
    /// previously issued id respawns under it (the redo of a spawn).
    Spawn { id: EntityId },
    /// Spawn an entity with an initial component set — the asset browser's "place" (m9.5), or with
    /// `exact` the undo of a despawn. Each component is `(type_hash, reflection-serialized bytes)`.
    SpawnEntity {
        id: EntityId,
        exact: bool,
        components: Vec<(u64, Vec<u8>)>,
    },
    /// Despawn an entity. The engine nulls every reference other entities hold to it.
    Despawn { id: EntityId },
    /// Ask the engine to resend the world (after a structural change, to resync the mirror).
    RequestSnapshot,
    /// Begin (from Edit) or resume (from Paused) the simulation (m9.7).
    Play,
    /// Stop ticking; the viewport keeps rendering (m9.7).
    Pause,
    /// Run exactly one fixed tick, then stay Paused (m9.7).
    Step,
    /// Restore the pre-play world; back to Edit (m9.7). Entities keep their ids (ADR-0075).
    Stop,
    /// Write the world back to a `.rscene` (m15.3). An empty path means "the file it was opened
    /// from"; the engine refuses rather than guesses when there is no such file.
    ///
    /// **The engine writes it, not the editor.** `scene_format.hpp` makes the C++ writer the
    /// reference implementation of the format, and this crate reuses it through files rather than
    /// reimplementing the byte layout — so "save" is a request, and the answer comes back as a
    /// `SaveResult` (which may be a refusal, with its reason).
    ///
    /// Deliberately NOT on the undo stack: it mutates a file, not the world, and there is nothing
    /// for an inverse to restore.
    SaveScene { path: String },
}

impl Command {
    /// The editor-channel message + payload bytes to put on the wire.
    pub fn to_wire(&self) -> (EditorMessage, Vec<u8>) {
        match self {
            Command::SetComponent {
                id,
                type_hash,
                blob,
            } => (
                EditorMessage::SetComponent,
                SetComponent {
                    editor_id: *id,
                    type_hash: *type_hash,
                    blob: blob.clone(),
                }
                .encode(),
            ),
            Command::AddComponent { id, type_hash } => (
                EditorMessage::AddComponent,
                component_ref(*id, *type_hash).encode(),
            ),
            Command::RemoveComponent { id, type_hash } => (
                EditorMessage::RemoveComponent,
                component_ref(*id, *type_hash).encode(),
            ),
            Command::Spawn { id } => (EditorMessage::Spawn, encode_entity_ref(*id)),
            Command::SpawnEntity {
                id,
                exact,
                components,
            } => (
                EditorMessage::SpawnEntity,
                rime_protocol::SpawnEntity {
                    editor_id: *id,
                    exact: *exact,
                    components: components.clone(),
                }
                .encode(),
            ),
            Command::Despawn { id } => (EditorMessage::Despawn, encode_entity_ref(*id)),
            Command::RequestSnapshot => (EditorMessage::RequestSnapshot, Vec::new()),
            Command::Play => (EditorMessage::Play, Vec::new()),
            Command::Pause => (EditorMessage::Pause, Vec::new()),
            Command::Step => (EditorMessage::Step, Vec::new()),
            Command::Stop => (EditorMessage::Stop, Vec::new()),
            Command::SaveScene { path } => (
                EditorMessage::SaveScene,
                SaveScene { path: path.clone() }.encode(),
            ),
        }
    }

    /// True for commands that change the world's *structure* (which entities/components exist), so the
    /// caller knows to request a fresh snapshot rather than trust an optimistic value patch.
    ///
    /// `Stop` counts: it restores the pre-play world (m9.7) — the same entities under the same ids
    /// (ADR-0075), but with their pre-play values and component sets, which the mirror must re-fetch.
    /// `Play`/`Pause`/`Step` do NOT — a tick moves component values (WorldTransform) on entities that
    /// already exist, which is exactly what an optimistic-patch model does not chase; the
    /// state-coloured viewport border (gui.rs) is the honest v1 signal that the inspector's numbers
    /// are not live during Play, rather than polling a snapshot every tick.
    pub fn is_structural(&self) -> bool {
        matches!(
            self,
            Command::AddComponent { .. }
                | Command::RemoveComponent { .. }
                | Command::Spawn { .. }
                | Command::SpawnEntity { .. }
                | Command::Despawn { .. }
                | Command::Stop
        )
    }

    /// True for the commands the engine answers with an `EditResult` (ADR-0075) — exactly one each,
    /// in send order. Every one of these sent must have a matching [`CommandStack::expect`].
    pub fn awaits_result(&self) -> bool {
        matches!(
            self,
            Command::AddComponent { .. }
                | Command::RemoveComponent { .. }
                | Command::Spawn { .. }
                | Command::SpawnEntity { .. }
                | Command::Despawn { .. }
        )
    }
}

fn component_ref(id: EntityId, type_hash: u64) -> ComponentRef {
    ComponentRef {
        editor_id: id,
        type_hash,
    }
}

/// An undoable edit: the forward commands and their exact inverse. Undo issues the inverse; redo
/// re-issues the forward. Most edits are one command each way; a despawn's inverse is several (the
/// respawn, then one `SetComponent` per reference the despawn nulled), sent in order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Edit {
    pub forward: Vec<Command>,
    pub inverse: Vec<Command>,
}

impl Edit {
    /// The common case: one command forward, one back.
    pub fn single(forward: Command, inverse: Command) -> Self {
        Edit {
            forward: vec![forward],
            inverse: vec![inverse],
        }
    }
}

/// What a structural user edit becomes in the history once the engine accepts it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PendingStep {
    /// The inverse was known when the edit was sent (add, remove, despawn).
    Ready(Edit),
    /// A spawn (`components: None`) or a placement: the step needs the id the engine assigns.
    Spawned {
        components: Option<Vec<(u64, Vec<u8>)>>,
    },
}

/// What to do with one `EditResult`, queued in send order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Awaiting {
    /// A new user edit: commit it to the history if the engine accepted it.
    Step(PendingStep),
    /// A command whose history is already settled — an undo/redo replaying a step, or an edit made
    /// while the history is frozen. A refusal is still reported.
    Replay,
}

/// A linear undo/redo history. `push` records a new edit and clears the redo branch (the usual
/// "editing after undo forks the timeline" rule). `undo`/`redo` return the commands to apply.
#[derive(Default)]
pub struct CommandStack {
    undo: Vec<Edit>,
    redo: Vec<Edit>,
    frozen: bool,
    awaiting: VecDeque<Awaiting>,
}

impl CommandStack {
    /// Record an applied edit. Anything previously undone is now unreachable. Ignored while frozen.
    pub fn push(&mut self, edit: Edit) {
        if self.frozen {
            return;
        }
        self.undo.push(edit);
        self.redo.clear();
    }

    /// Move the last edit to the redo branch and return the commands that undo it (its inverse).
    /// `None` while frozen.
    pub fn undo(&mut self) -> Option<Vec<Command>> {
        if self.frozen {
            return None;
        }
        let edit = self.undo.pop()?;
        let inverse = edit.inverse.clone();
        self.redo.push(edit);
        Some(inverse)
    }

    /// Move the last undone edit back and return the commands that re-apply it (its forward).
    /// `None` while frozen.
    pub fn redo(&mut self) -> Option<Vec<Command>> {
        if self.frozen {
            return None;
        }
        let edit = self.redo.pop()?;
        let forward = edit.forward.clone();
        self.undo.push(edit);
        Some(forward)
    }

    pub fn can_undo(&self) -> bool {
        !self.frozen && !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.frozen && !self.redo.is_empty()
    }

    /// Freeze (while the simulation runs) or thaw (back in Edit) the history. The steps already
    /// recorded are kept untouched: they name entities by id, and Stop gives every entity its id
    /// back, so they are as valid after Play→Stop as before it.
    pub fn set_frozen(&mut self, frozen: bool) {
        self.frozen = frozen;
    }

    /// Queue what to do with the engine's answer to a command just sent (one that
    /// [`Command::awaits_result`]). While frozen a new step is downgraded to a replay: it applies,
    /// and Stop will discard it, so there is nothing for the history to hold.
    pub fn expect(&mut self, awaiting: Awaiting) {
        let awaiting = match awaiting {
            Awaiting::Step(_) if self.frozen => Awaiting::Replay,
            other => other,
        };
        self.awaiting.push_back(awaiting);
    }

    /// Apply the engine's next answer. Returns a message for the user when the engine refused
    /// something (nothing changed in the world, and no step was recorded).
    pub fn acknowledge(&mut self, result: EditResult) -> Option<String> {
        let Some(awaiting) = self.awaiting.pop_front() else {
            return Some("the engine answered an edit the editor did not send".to_owned());
        };
        if !result.ok {
            return Some(match awaiting {
                Awaiting::Step(_) => {
                    format!("the engine refused that edit (entity {})", result.editor_id)
                }
                Awaiting::Replay => format!(
                    "the engine refused to undo/redo (entity {})",
                    result.editor_id
                ),
            });
        }
        if let Awaiting::Step(step) = awaiting {
            let edit = match step {
                PendingStep::Ready(edit) => edit,
                PendingStep::Spawned { components } => {
                    let id = result.editor_id;
                    let forward = match components {
                        None => Command::Spawn { id },
                        Some(components) => Command::SpawnEntity {
                            id,
                            exact: false,
                            components,
                        },
                    };
                    Edit::single(forward, Command::Despawn { id })
                }
            };
            // Committed even if the history froze meanwhile: the engine applied it while editing.
            self.undo.push(edit);
            self.redo.clear();
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(id: EntityId, hash: u64, blob: &[u8]) -> Command {
        Command::SetComponent {
            id,
            type_hash: hash,
            blob: blob.to_vec(),
        }
    }

    #[test]
    fn undo_returns_the_inverse_and_redo_returns_the_forward() {
        let mut stack = CommandStack::default();
        let id = 1;
        // Edit a component from bytes [1,2] to [3,4]; the inverse restores [1,2].
        stack.push(Edit::single(set(id, 0xAA, &[3, 4]), set(id, 0xAA, &[1, 2])));
        assert!(stack.can_undo() && !stack.can_redo());

        // Undo yields the inverse (restore [1,2]) — the exact previous bytes.
        assert_eq!(stack.undo(), Some(vec![set(id, 0xAA, &[1, 2])]));
        assert!(!stack.can_undo() && stack.can_redo());

        // Redo yields the forward again (re-apply [3,4]).
        assert_eq!(stack.redo(), Some(vec![set(id, 0xAA, &[3, 4])]));
        assert!(stack.can_undo() && !stack.can_redo());
    }

    #[test]
    fn a_new_edit_clears_the_redo_branch() {
        let mut stack = CommandStack::default();
        let id = 2;
        stack.push(Edit::single(set(id, 1, &[9]), set(id, 1, &[0])));
        assert_eq!(stack.undo(), Some(vec![set(id, 1, &[0])]));
        assert!(stack.can_redo());
        // A fresh edit forks the timeline — redo is gone.
        stack.push(Edit::single(set(id, 1, &[5]), set(id, 1, &[0])));
        assert!(!stack.can_redo());
    }

    #[test]
    fn add_and_remove_map_to_the_right_wire_messages() {
        let (msg, _) = (Command::AddComponent {
            id: 3,
            type_hash: 7,
        })
        .to_wire();
        assert_eq!(msg, EditorMessage::AddComponent);
        let (msg, payload) = (Command::RemoveComponent {
            id: 3,
            type_hash: 7,
        })
        .to_wire();
        assert_eq!(msg, EditorMessage::RemoveComponent);
        // The payload is a ComponentRef the engine can parse back.
        let cr = ComponentRef::decode(&payload).expect("decode ref");
        assert_eq!((cr.editor_id, cr.type_hash), (3, 7));
    }

    #[test]
    fn a_frozen_history_records_nothing_and_refuses_undo_and_redo() {
        let mut stack = CommandStack::default();
        stack.push(Edit::single(set(1, 1, &[1]), set(1, 1, &[0])));
        stack.push(Edit::single(set(1, 1, &[2]), set(1, 1, &[1])));
        assert_eq!(stack.undo(), Some(vec![set(1, 1, &[1])]));
        stack.set_frozen(true);
        assert!(!stack.can_undo() && !stack.can_redo());
        assert_eq!(stack.undo(), None);
        assert_eq!(stack.redo(), None);
        stack.push(Edit::single(set(1, 1, &[7]), set(1, 1, &[2])));
        // A new step while frozen is downgraded to a replay: accepted by the engine, never recorded.
        stack.expect(Awaiting::Step(PendingStep::Spawned { components: None }));
        assert_eq!(
            stack.acknowledge(EditResult {
                ok: true,
                editor_id: 9
            }),
            None
        );
        stack.set_frozen(false);
        // Thawed, the history is exactly what it was before the freeze — redo branch included.
        assert_eq!(stack.redo(), Some(vec![set(1, 1, &[2])]));
        assert_eq!(stack.undo(), Some(vec![set(1, 1, &[1])]));
        assert_eq!(stack.undo(), Some(vec![set(1, 1, &[0])]));
        assert!(!stack.can_undo());
    }

    #[test]
    fn a_spawn_step_is_committed_with_the_id_the_engine_assigned() {
        let mut stack = CommandStack::default();
        stack.expect(Awaiting::Step(PendingStep::Spawned { components: None }));
        assert!(
            !stack.can_undo(),
            "nothing to undo until the engine answers"
        );
        assert_eq!(
            stack.acknowledge(EditResult {
                ok: true,
                editor_id: 12
            }),
            None
        );
        assert_eq!(stack.undo(), Some(vec![Command::Despawn { id: 12 }]));
        assert_eq!(stack.redo(), Some(vec![Command::Spawn { id: 12 }]));
    }

    #[test]
    fn a_refused_edit_leaves_no_step_and_says_so() {
        let mut stack = CommandStack::default();
        stack.expect(Awaiting::Step(PendingStep::Ready(Edit::single(
            Command::AddComponent {
                id: 4,
                type_hash: 1,
            },
            Command::RemoveComponent {
                id: 4,
                type_hash: 1,
            },
        ))));
        let refusal = stack.acknowledge(EditResult {
            ok: false,
            editor_id: 4,
        });
        assert!(refusal.is_some());
        assert!(!stack.can_undo());
    }
}

#[cfg(test)]
mod save_tests {
    use super::*;
    use rime_protocol::SaveScene as WireSaveScene;

    // m15.3. `SaveScene` was implemented end to end on the wire at m14.3 and reachable only from
    // the headless smoke — `File` in the menu bar was a dead `ui.label`, so a person editing in the
    // GUI lost their work on close. These pin the command layer the menu now goes through, which is
    // the link that was missing rather than the message that always worked.
    #[test]
    fn save_becomes_the_wire_message_with_its_path() {
        let (msg, payload) = Command::SaveScene {
            path: "/tmp/a b/scene.rscene".to_string(),
        }
        .to_wire();
        assert_eq!(msg, EditorMessage::SaveScene);
        let decoded = WireSaveScene::decode(&payload).expect("decode");
        assert_eq!(decoded.path, "/tmp/a b/scene.rscene");
    }

    #[test]
    fn an_empty_path_survives_and_means_write_it_back() {
        // Distinct from a missing field: it is how the editor says "wherever you opened it from",
        // a decision the engine owns because only it knows the path it was handed.
        let (_, payload) = Command::SaveScene {
            path: String::new(),
        }
        .to_wire();
        assert!(WireSaveScene::decode(&payload)
            .expect("decode")
            .path
            .is_empty());
    }

    #[test]
    fn saving_is_not_a_structural_change() {
        // It mutates a FILE, not the world — so it must not trigger a resync, and there is nothing
        // for an undo inverse to restore either.
        assert!(!Command::SaveScene {
            path: String::new()
        }
        .is_structural());
    }
}
