// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Semantic references over the existing EditorId wire: the engine still resolves live handles.
use super::{exact_int_field, outliner, Interaction};
use eframe::egui;
use rime_protocol::{FieldKind, Schema, SnapshotEntity, Value, ENTITY_TYPE_NAME};

const CELL: &str = "rime::fluids_scene::ThermalCell";

fn is_cell(schema: &Schema, entity: &SnapshotEntity) -> bool {
    entity.components.iter().any(|component| {
        schema
            .type_by_hash(component.type_hash)
            .is_some_and(|entry| entry.name == CELL)
    })
}

fn label(schema: &Schema, row: usize, entity: &SnapshotEntity) -> String {
    format!(
        "{} · id {}",
        outliner::label(schema, row, entity),
        entity.editor_id
    )
}

fn current_label(schema: &Schema, entities: &[SnapshotEntity], id: u64) -> String {
    if id == 0 {
        return "None · id 0".into();
    }
    match entities.iter().enumerate().find(|(_, e)| e.editor_id == id) {
        Some((row, entity)) if is_cell(schema, entity) => label(schema, row, entity),
        Some((row, entity)) => format!("{} (not a ThermalCell)", label(schema, row, entity)),
        None => format!("Missing / external · id {id}"),
    }
}

/// Intercept only the known thermal fields *and* their real reflected Entity representation.
/// Integers such as mesh hashes or future game-specific references must keep their ordinary
/// editor. The shape check makes an unfamiliar schema fall back to that editor without guessing.
fn reference_id<'a>(
    schema: &Schema,
    component: u64,
    field: &str,
    value: &'a mut Value,
) -> Option<&'a mut u64> {
    let entry = schema.type_by_hash(component)?;
    if !matches!(
        (entry.name.as_str(), field),
        ("rime::fluids_scene::ThermalSource", "cell")
            | ("rime::fluids_scene::ThermalContact", "first" | "second")
    ) {
        return None;
    }
    let desc = entry.fields.iter().find(|desc| desc.name == field)?;
    if desc.kind != FieldKind::Struct {
        return None;
    }
    let nested = schema.type_by_hash(desc.nested_hash)?;
    if nested.name != ENTITY_TYPE_NAME
        || nested.fields.len() != 1
        || nested.fields[0].kind != FieldKind::U64
    {
        return None;
    }
    match value {
        Value::Struct(fields) if fields.len() == 1 => match &mut fields[0].1 {
            Value::U64(id) => Some(id),
            _ => None,
        },
        _ => None,
    }
}

#[allow(clippy::too_many_arguments)] // field context plus snapshot, without owning edit state
pub(super) fn field_ui(
    ui: &mut egui::Ui,
    salt: &str,
    schema: &Schema,
    component: u64,
    field: &str,
    value: &mut Value,
    entities: &[SnapshotEntity],
    interaction: &mut Interaction,
) -> bool {
    let Some(id) = reference_id(schema, component, field, value) else {
        return false;
    };
    ui.label(field);
    let before = *id;
    let response = egui::ComboBox::from_id_salt((salt, "entity-reference"))
        .selected_text(current_label(schema, entities, *id))
        .show_ui(ui, |ui| {
            ui.selectable_value(id, 0, "None · id 0");
            let mut any = false;
            for (row, entity) in entities.iter().enumerate() {
                if entity.editor_id != 0 && is_cell(schema, entity) {
                    any = true;
                    ui.selectable_value(id, entity.editor_id, label(schema, row, entity));
                }
            }
            if !any {
                ui.weak("No ThermalCell entities in this snapshot");
            }
        })
        .response
        .on_hover_text("Choose a water / ice cell. References use stable editor ids; the engine resolves their live handles.");
    response.widget_info(|| {
        egui::WidgetInfo::labeled(
            egui::WidgetType::ComboBox,
            ui.is_enabled(),
            format!("{field}: {}", current_label(schema, entities, *id)),
        )
    });
    if *id != before {
        // A selection is one discrete gesture. Let the usual component encoder and command stack
        // capture its exact forward/inverse bytes, just like a checkbox, instead of a second wire.
        interaction.changed = true;
        interaction.committed = true;
    }
    ui.horizontal(|ui| {
        ui.label("Editor id");
        exact_int_field(ui, &format!("{salt}/reference-id"), id, interaction);
    })
    .response
    .on_hover_text("Exact numeric fallback for missing or external references. 0 clears this reference. No automatic retargeting.");
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gui::{component_fields_ui, ActiveEdit, Command, CommandStack};
    use egui::accesskit::Role;
    use egui_kittest::{kittest::Queryable, Harness};
    use rime_protocol::{encode_value, FieldDesc, SchemaEntry, SetComponent, SnapshotComponent};

    const ENTITY: u64 = 1;
    const SOURCE: u64 = 2;
    const CELL_HASH: u64 = 3;
    const CONTACT: u64 = 4;
    const OTHER: u64 = 5;
    const FIRST: u64 = 0x2000_0000_0000_0001;
    const SECOND: u64 = FIRST + 2;

    fn field(name: &str, kind: FieldKind, nested_hash: u64) -> FieldDesc {
        FieldDesc {
            name: name.into(),
            kind,
            nested_hash,
        }
    }

    fn schema() -> Schema {
        let entry = |type_hash, name: &str, fields| SchemaEntry {
            type_hash,
            name: name.into(),
            is_component: type_hash != ENTITY,
            fields,
        };
        Schema {
            types: vec![
                entry(
                    ENTITY,
                    ENTITY_TYPE_NAME,
                    vec![field("editor_id", FieldKind::U64, 0)],
                ),
                entry(
                    SOURCE,
                    "rime::fluids_scene::ThermalSource",
                    vec![
                        field("cell", FieldKind::Struct, ENTITY),
                        field("power_watts", FieldKind::F64, 0),
                        field("enabled", FieldKind::Bool, 0),
                    ],
                ),
                entry(CELL_HASH, CELL, vec![field("enabled", FieldKind::Bool, 0)]),
                entry(
                    CONTACT,
                    "rime::fluids_scene::ThermalContact",
                    vec![
                        field("first", FieldKind::Struct, ENTITY),
                        field("second", FieldKind::Struct, ENTITY),
                    ],
                ),
                entry(OTHER, "game::Other", vec![field("cell", FieldKind::U64, 0)]),
            ],
        }
    }

    fn reference(id: u64) -> Value {
        Value::Struct(vec![("editor_id".into(), Value::U64(id))])
    }

    fn blob(id: u64) -> Vec<u8> {
        encode_value(&Value::Struct(vec![
            ("cell".into(), reference(id)),
            ("power_watts".into(), Value::F64(-1250.5)),
            ("enabled".into(), Value::Bool(true)),
        ]))
    }

    fn entities() -> Vec<SnapshotEntity> {
        [FIRST, SECOND, 77]
            .into_iter()
            .enumerate()
            .map(|(i, editor_id)| SnapshotEntity {
                index: 90 + i as u32,
                generation: 7,
                editor_id,
                components: vec![SnapshotComponent {
                    type_hash: if i < 2 { CELL_HASH } else { OTHER },
                    data: vec![0],
                }],
            })
            .collect()
    }

    #[derive(Default)]
    struct State {
        component: u64,
        data: Vec<u8>,
        active: Option<ActiveEdit>,
        stack: CommandStack,
        sent: Vec<Command>,
    }

    fn harness(id: u64) -> Harness<'static, State> {
        Harness::new_ui_state(
            |ui, state: &mut State| {
                let component = SnapshotComponent {
                    type_hash: state.component,
                    data: state.data.clone(),
                };
                let mut actions = vec![];
                component_fields_ui(
                    ui,
                    &component,
                    55,
                    &schema(),
                    &entities(),
                    &mut actions,
                    &mut state.active,
                    &mut state.stack,
                );
                for command in actions {
                    if let Command::SetComponent { blob, .. } = &command {
                        state.data = blob.clone();
                    }
                    state.sent.push(command);
                }
            },
            State {
                component: SOURCE,
                data: blob(id),
                ..Default::default()
            },
        )
    }

    fn click(harness: &mut Harness<'_, State>, text: &str) {
        harness.get_by_label(text).click();
        harness.run();
    }

    #[test]
    fn thermal_reference_selection_sends_exact_component_bytes_and_one_undo() {
        let mut h = harness(FIRST);
        click(
            &mut h,
            &format!("cell: {}", current_label(&schema(), &entities(), FIRST)),
        );
        // A source cannot target arbitrary entities just because they have an editor id.
        assert!(h.query_by_label("2  Other · id 77").is_none());
        click(&mut h, &current_label(&schema(), &entities(), SECOND));
        assert_eq!(h.state().data, blob(SECOND));
        assert_eq!(h.state().sent.len(), 1);
        let (_, wire) = h.state().sent[0].to_wire();
        let edit = SetComponent::decode(&wire).unwrap();
        assert_eq!(
            (edit.editor_id, edit.type_hash, edit.blob),
            (55, SOURCE, blob(SECOND))
        );
        assert_eq!(
            h.state_mut().stack.undo(),
            Some(vec![Command::SetComponent {
                id: 55,
                type_hash: SOURCE,
                blob: blob(FIRST),
            }])
        );
        assert!(!h.state().stack.can_undo());
        assert_eq!(
            h.state_mut().stack.redo(),
            Some(vec![Command::SetComponent {
                id: 55,
                type_hash: SOURCE,
                blob: blob(SECOND),
            }])
        );
    }

    #[test]
    fn missing_reference_is_retained_until_explicitly_cleared() {
        let mut h = harness(u64::MAX);
        let shown = "cell: Missing / external · id 18446744073709551615";
        assert!(h.query_by_label(shown).is_some());
        h.run();
        assert_eq!(h.state().data, blob(u64::MAX));
        assert!(h.state().sent.is_empty());
        click(&mut h, shown);
        click(&mut h, "None · id 0");
        assert_eq!(h.state().data, blob(0));
        assert_eq!(
            h.state_mut().stack.undo(),
            Some(vec![Command::SetComponent {
                id: 55,
                type_hash: SOURCE,
                blob: blob(u64::MAX),
            }])
        );
    }

    #[test]
    fn numeric_reference_fallback_keeps_all_64_bits() {
        let mut h = harness(0);
        h.get_by_role(Role::TextInput).focus();
        h.run();
        h.press_key(egui::Key::End);
        h.press_key(egui::Key::Backspace);
        h.run();
        h.get_by_role(Role::TextInput)
            .type_text(&SECOND.to_string());
        h.run();
        h.press_key(egui::Key::Enter);
        h.run();
        assert_eq!(h.state().data, blob(SECOND));
        assert_eq!(h.state().sent.len(), 1);
        assert!(h.state().stack.can_undo());
    }

    #[test]
    fn only_exact_thermal_entity_fields_get_a_picker() {
        let schema = schema();
        for (component, field) in [(SOURCE, "cell"), (CONTACT, "first"), (CONTACT, "second")] {
            assert!(reference_id(&schema, component, field, &mut reference(SECOND)).is_some());
        }
        for (component, field) in [(SOURCE, "power_watts"), (SOURCE, "first"), (OTHER, "cell")] {
            assert!(reference_id(&schema, component, field, &mut reference(SECOND)).is_none());
        }
        assert!(reference_id(&schema, SOURCE, "cell", &mut Value::U64(SECOND)).is_none());
        let mut malformed = schema;
        malformed.types[0]
            .fields
            .push(field("generation", FieldKind::U32, 0));
        assert!(reference_id(&malformed, SOURCE, "cell", &mut reference(SECOND)).is_none());
    }

    #[test]
    fn incompatible_current_target_stays_visible_and_candidates_are_cells_only() {
        let schema = schema();
        let entities = entities();
        assert_eq!(
            entities
                .iter()
                .filter(|e| is_cell(&schema, e))
                .map(|e| e.editor_id)
                .collect::<Vec<_>>(),
            vec![FIRST, SECOND]
        );
        assert_eq!(
            current_label(&schema, &entities, 77),
            "2  Other · id 77 (not a ThermalCell)"
        );
        let mut h = harness(77);
        h.run();
        assert_eq!(h.state().data, blob(77));
        assert!(h.state().sent.is_empty());
    }

    #[test]
    fn thermal_contact_endpoints_are_independent_undoable_references() {
        let contact_blob = |first, second| {
            encode_value(&Value::Struct(vec![
                ("first".into(), reference(first)),
                ("second".into(), reference(second)),
            ]))
        };
        let mut h = harness(0);
        h.state_mut().component = CONTACT;
        h.state_mut().data = contact_blob(0, 0);
        h.run();
        click(&mut h, "first: None · id 0");
        click(&mut h, &current_label(&schema(), &entities(), FIRST));
        assert_eq!(h.state().data, contact_blob(FIRST, 0));
        click(&mut h, "second: None · id 0");
        click(&mut h, &current_label(&schema(), &entities(), SECOND));
        assert_eq!(h.state().data, contact_blob(FIRST, SECOND));
        assert_eq!(
            h.state_mut().stack.undo(),
            Some(vec![Command::SetComponent {
                id: 55,
                type_hash: CONTACT,
                blob: contact_blob(FIRST, 0),
            }])
        );
        assert_eq!(
            h.state_mut().stack.undo(),
            Some(vec![Command::SetComponent {
                id: 55,
                type_hash: CONTACT,
                blob: contact_blob(0, 0),
            }])
        );
    }

    #[test]
    fn reference_picker_is_keyboard_accessible_and_cancel_does_not_edit() {
        let mut h = harness(FIRST);
        h.get_by_role(Role::ComboBox).focus();
        h.run();
        h.press_key(egui::Key::Enter);
        h.run();
        assert!(h.query_by_label("None · id 0").is_some());
        h.press_key(egui::Key::Escape);
        h.run();
        assert!(h.query_by_label("None · id 0").is_none());
        assert!(h.state().sent.is_empty());
        assert!(!h.state().stack.can_undo());
    }
}
