// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Scene discovery uses semantic roles, not archetype/component insertion order. The icons are
//! small vector drawings, so they stay crisp at UI scale without an asset cook or font glyphs.
//!
//! Names are derived because reflection currently has no string field and ECS components must
//! remain trivially copyable. An authored Name needs a string representation threaded through
//! serialization, scene/schema wire and inspector (ADR-0038); a display-only alias here would
//! disappear on reload. Existing game roles and readable engine roles provide useful labels
//! without pretending that persistent user naming or rendered asset thumbnails already exist.
use super::{Command, EntityId};
use eframe::egui;
use rime_protocol::{Schema, SnapshotEntity};

#[derive(Default, PartialEq)]
enum Filter {
    #[default]
    All,
    Effects,
    Thermal,
}
#[derive(Default, PartialEq)]
enum View {
    #[default]
    List,
    Tiles,
}
#[derive(Default)]
pub(super) struct Options {
    filter: Filter,
    view: View,
}

#[derive(Clone, Copy)]
enum Icon {
    Mesh,
    Smoke,
    Fire,
    Water,
    Heat,
    Contact,
    Camera,
    Light,
    Entity,
}

fn role<'a>(schema: &'a Schema, entity: &SnapshotEntity) -> (&'a str, Icon) {
    let mut best = (u8::MAX, "entity", Icon::Entity);
    for c in &entity.components {
        let Some(t) = schema.type_by_hash(c.type_hash) else {
            continue;
        };
        let candidate = match t.name.as_str() {
            "rime::ecs::LocalTransform"
            | "rime::ecs::WorldTransform"
            | "rime::ecs::RenderTransform"
            | "rime::ecs::Parent" => continue,
            "rime::vfx_scene::SmokeEmitter" => (0, "Smoke emitter", Icon::Smoke),
            "rime::vfx_scene::FireEmitter" => (0, "Fire emitter", Icon::Fire),
            "rime::fluids_scene::ThermalCell" => (1, "Water / ice", Icon::Water),
            "rime::fluids_scene::ThermalSource" => (2, "Heat source", Icon::Heat),
            "rime::fluids_scene::ThermalContact" => (3, "Thermal contact", Icon::Contact),
            "rime::render::Camera" => (4, "Camera", Icon::Camera),
            "rime::render::PointLight" => (5, "Point light", Icon::Light),
            "rime::render::DirectionalLight" => (5, "Sun light", Icon::Light),
            "rime::render::MeshAsset" => (40, "Mesh asset", Icon::Mesh),
            "rime::render::MeshRef" => (50, "Mesh", Icon::Mesh),
            "rime::render::MaterialRef" | "rime::render::MaterialSet" => {
                (100, "Material", Icon::Mesh)
            }
            "rime::fluids_render::ThermalSurface" => (60, "Phase surface", Icon::Water),
            // Game-specific roles (SlabRole, Station, etc.) beat generic rendering/storage.
            _ => (
                20,
                t.name.rsplit("::").next().unwrap_or(&t.name),
                Icon::Entity,
            ),
        };
        // Tie-break by semantic name, never component order. Both smoke and fire on one entity
        // receive a stable primary label; the tooltip still lists every attached component.
        if (candidate.0, candidate.1) < (best.0, best.1) {
            best = candidate;
        }
    }
    (best.1, best.2)
}

pub(super) fn label(schema: &Schema, index: usize, entity: &SnapshotEntity) -> String {
    format!("{index}  {}", role(schema, entity).0)
}

fn icon(ui: &mut egui::Ui, kind: Icon, size: f32, selected: bool, label: &str) -> egui::Response {
    let (rect, response) = ui.allocate_exact_size(egui::vec2(size, size), egui::Sense::click());
    response.widget_info(|| {
        egui::WidgetInfo::labeled(
            egui::WidgetType::Button,
            ui.is_enabled(),
            format!("Select {label}"),
        )
    });
    let p = ui.painter();
    p.rect_filled(
        rect,
        3.0,
        if selected {
            ui.visuals().selection.bg_fill
        } else {
            egui::Color32::from_rgb(35, 44, 55)
        },
    );
    let at = |x, y| rect.min + egui::vec2(x * size, y * size);
    let cyan = egui::Color32::from_rgb(110, 208, 234);
    let amber = egui::Color32::from_rgb(255, 190, 84);
    let pale = egui::Color32::from_rgb(203, 219, 235);
    let line = |a, b, color| {
        p.line_segment([a, b], egui::Stroke::new(1.2_f32, color));
    };
    match kind {
        Icon::Smoke => {
            for (x, y, r) in [(0.35, 0.72, 0.14), (0.6, 0.48, 0.2), (0.4, 0.22, 0.13)] {
                p.circle_filled(at(x, y), size * r, pale);
            }
        }
        Icon::Fire => {
            p.add(egui::Shape::convex_polygon(
                vec![
                    at(0.2, 0.8),
                    at(0.22, 0.5),
                    at(0.5, 0.12),
                    at(0.78, 0.55),
                    at(0.82, 0.8),
                    at(0.5, 0.92),
                ],
                egui::Color32::from_rgb(242, 112, 45),
                egui::Stroke::NONE,
            ));
            p.add(egui::Shape::convex_polygon(
                vec![at(0.35, 0.8), at(0.5, 0.45), at(0.65, 0.8)],
                amber,
                egui::Stroke::NONE,
            ));
        }
        Icon::Water => {
            p.add(egui::Shape::convex_polygon(
                vec![
                    at(0.16, 0.65),
                    at(0.5, 0.12),
                    at(0.84, 0.65),
                    at(0.72, 0.88),
                    at(0.28, 0.88),
                ],
                cyan,
                egui::Stroke::NONE,
            ));
            line(at(0.5, 0.5), at(0.5, 0.83), pale);
            line(at(0.34, 0.59), at(0.66, 0.77), pale);
            line(at(0.34, 0.77), at(0.66, 0.59), pale);
        }
        Icon::Heat => {
            for x in [0.25, 0.5, 0.75] {
                p.add(egui::Shape::line(
                    vec![
                        at(x, 0.85),
                        at(x - 0.07, 0.6),
                        at(x + 0.07, 0.4),
                        at(x, 0.15),
                    ],
                    egui::Stroke::new(1.5_f32, amber),
                ));
            }
        }
        Icon::Contact => {
            p.circle_stroke(at(0.35, 0.5), size * 0.22, egui::Stroke::new(1.5_f32, cyan));
            p.circle_stroke(
                at(0.65, 0.5),
                size * 0.22,
                egui::Stroke::new(1.5_f32, amber),
            );
        }
        Icon::Camera => {
            p.rect_stroke(
                egui::Rect::from_min_max(at(0.15, 0.3), at(0.62, 0.75)),
                1.0,
                egui::Stroke::new(1.3_f32, pale),
                egui::StrokeKind::Inside,
            );
            p.add(egui::Shape::line(
                vec![
                    at(0.62, 0.45),
                    at(0.87, 0.27),
                    at(0.87, 0.78),
                    at(0.62, 0.6),
                ],
                egui::Stroke::new(1.3_f32, pale),
            ));
        }
        Icon::Light => {
            p.circle_filled(at(0.5, 0.5), size * 0.18, amber);
            for i in 0..8 {
                let angle = i as f32 * std::f32::consts::TAU / 8.0;
                let direction = egui::vec2(angle.cos(), angle.sin()) * size;
                line(
                    rect.center() + direction * 0.27,
                    rect.center() + direction * 0.4,
                    amber,
                );
            }
        }
        Icon::Mesh => {
            let points = [
                at(0.16, 0.3),
                at(0.5, 0.12),
                at(0.84, 0.3),
                at(0.84, 0.72),
                at(0.5, 0.9),
                at(0.16, 0.72),
            ];
            for i in 0..6 {
                line(points[i], points[(i + 1) % 6], cyan);
            }
            for i in [0, 2, 4] {
                line(at(0.5, 0.5), points[i], cyan);
            }
        }
        Icon::Entity => {
            p.circle_stroke(rect.center(), size * 0.28, egui::Stroke::new(1.3_f32, pale));
            line(at(0.35, 0.5), at(0.65, 0.5), pale);
            line(at(0.5, 0.35), at(0.5, 0.65), pale);
        }
    }
    response
}

pub(super) fn ui(
    ui: &mut egui::Ui,
    schema: &Schema,
    entities: &[SnapshotEntity],
    selected: &mut Option<EntityId>,
    actions: &mut Vec<Command>,
    options: &mut Options,
) {
    ui.horizontal(|ui| {
        if ui.button("+ spawn").clicked() {
            *selected = None;
            actions.push(Command::Spawn { id: 0 });
        }
        ui.menu_button("Filter", |ui| {
            ui.selectable_value(&mut options.filter, Filter::All, "All entities");
            ui.selectable_value(
                &mut options.filter,
                Filter::Effects,
                "Effects (smoke / fire)",
            );
            ui.selectable_value(
                &mut options.filter,
                Filter::Thermal,
                "Thermal (water / ice)",
            );
        });
        ui.menu_button("Display", |ui| {
            ui.selectable_value(&mut options.view, View::List, "List");
            ui.selectable_value(&mut options.view, View::Tiles, "Tiles");
        });
    });
    if options.filter != Filter::All {
        ui.weak(if options.filter == Filter::Effects {
            "Effects filter active"
        } else {
            "Thermal filter active"
        });
    }
    ui.separator();
    if entities.is_empty() {
        ui.weak("(no entities yet)");
        return;
    }
    let matches = |e: &SnapshotEntity| {
        options.filter == Filter::All
            || e.components.iter().any(|c| {
                schema.type_by_hash(c.type_hash).is_some_and(|t| {
                    if options.filter == Filter::Effects {
                        matches!(
                            t.name.as_str(),
                            "rime::vfx_scene::SmokeEmitter" | "rime::vfx_scene::FireEmitter"
                        )
                    } else {
                        t.name.starts_with("rime::fluids_scene::")
                            || t.name == "rime::fluids_render::ThermalSurface"
                    }
                })
            })
    };
    egui::ScrollArea::vertical().show(ui, |ui| {
        let mut shown = 0;
        let mut row = |ui: &mut egui::Ui, i: usize, e: &SnapshotEntity| {
            shown += 1;
            ui.push_id(e.editor_id, |ui| {
                let chosen = *selected == Some(e.editor_id);
                let (_, kind) = role(schema, e);
                let icon_response = icon(
                    ui,
                    kind,
                    if options.view == View::Tiles {
                        32.0
                    } else {
                        20.0
                    },
                    chosen,
                    &label(schema, i, e),
                );
                let text_response = ui.selectable_label(chosen, label(schema, i, e));
                if icon_response.clicked() || text_response.clicked() {
                    *selected = Some(e.editor_id);
                }
                let components = e
                    .components
                    .iter()
                    .filter_map(|c| schema.type_by_hash(c.type_hash))
                    .map(|t| t.name.as_str())
                    .collect::<Vec<_>>()
                    .join("\n");
                let tooltip = format!(
                    "Scene row {i} · handle {}:{} · editor id {}\n{components}",
                    e.index, e.generation, e.editor_id
                );
                icon_response.on_hover_text(&tooltip);
                text_response.on_hover_text(tooltip);
            });
        };
        if options.view == View::Tiles {
            ui.horizontal_wrapped(|ui| {
                for (i, e) in entities.iter().enumerate().filter(|(_, e)| matches(e)) {
                    ui.allocate_ui_with_layout(
                        egui::vec2(108.0, 64.0),
                        egui::Layout::top_down(egui::Align::Center),
                        |ui| row(ui, i, e),
                    );
                }
            });
        } else {
            for (i, e) in entities.iter().enumerate().filter(|(_, e)| matches(e)) {
                ui.horizontal(|ui| row(ui, i, e));
            }
        }
        if shown == 0 {
            ui.weak("(no entities match the filter)");
        }
    });
}
