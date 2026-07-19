// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"

// The **editor gizmo renderer** (m9.6 Part B): the engine-side half of transform gizmos. The
// architecture splits the work along the editor-as-client seam (ADR-0016/0031):
//
//   * The EDITOR owns the interaction — it knows the cursor, computes the drag math against the
//     `ViewportCamera` lens the engine ships each frame, and edits the entity through the ordinary
//     `SetComponent` path (so a drag is undoable exactly like an inspector edit).
//   * The ENGINE only *renders*: the translate/rotate/scale handles over the selected entity, and
//     a translucent tint over the selected mesh so the selection reads on the streamed frame. What
//     to draw arrives as the `GizmoState` message (selection + mode + hovered axis).
//
// Rendering choices (ratified at kickoff):
//   * **Always-on-top**: the overlay pass loads the finished LDR frame and draws with NO depth
//     test — handles are UI in the 3D view and must never sink into the geometry they manipulate
//     (the classic editor-overlay policy; the m9.6 plan's "depth-tested against nothing").
//   * **Screen-constant size**: the handle geometry is unit-sized and scaled by
//     `distance(eye, entity) * tan(fov_y/2) * kGizmoScreenFraction`, so it covers the same
//     fraction of the viewport at any camera distance — a gizmo you must walk towards to grab is
//     a broken gizmo. (docs/math/gizmos.md derives why this is the right factor.)
//   * **World-aligned axes** (v1): handles align to world X/Y/Z, not the entity's rotation — the
//     common editor default ("global" space), and it keeps the editor's drag math axis-trivial.
//   * **Tint highlight**, not stencil outline: the selected mesh is re-drawn through an
//     alpha-blending flat-colour pipeline. One extra draw, no extra targets, and it reads clearly
//     after LZ4 streaming (the outline alternative thins to nothing at stream bitrates).
namespace rime::render {

// The viewport's render lens, in one bundle: the exact clip-from-world the frame is rendered
// with, its inverse, and the eye — everything projection/unprojection needs, computed ONCE per
// frame and shared by the gizmo pass and the `ViewportCamera` message (they must agree, or the
// editor's drag math would fight the pixels it drags over; the pick pass makes the same argument
// in scene_picker.cpp).
struct CameraLens {
    bool found = false;       // false: no active camera (or degenerate extent) — render no gizmo
    core::Mat4 view_proj;     // perspective(fov, aspect, near, far) * camera view
    core::Mat4 inv_view_proj; // the true inverse (core::inverse), clip -> world
    core::Vec3 eye{0.0f, 0.0f, 0.0f}; // camera world position
    float fov_y = 0.0f;               // vertical field of view (radians) — screen-constant scaling
};

// Build the lens for `world`'s active camera at `extent` — the same formula the SceneRenderer and
// ScenePicker use, folded with the inverse the editor needs. Extraction-light but not free (it
// walks the world for the camera); call once per frame and reuse.
[[nodiscard]] CameraLens compute_camera_lens(ecs::World& world, rhi::Extent2D extent);

// Which gizmo to draw, mirroring the wire's GizmoState codes (editorhost::GizmoStateMsg). Kept as
// engine-side enums so render code never includes the protocol module (module boundaries).
enum class GizmoMode : std::uint8_t { None = 0, Translate = 1, Rotate = 2, Scale = 3 };
enum class GizmoAxis : std::uint8_t { None = 0, X = 1, Y = 2, Z = 3 };

struct GizmoSelection {
    ecs::Entity entity = ecs::kNullEntity;
    GizmoMode mode = GizmoMode::None;
    GizmoAxis axis = GizmoAxis::None; // the hovered/active axis, drawn in the highlight colour
};

// The fraction of the viewport's half-height a unit gizmo spans (see the screen-constant note
// above). Mirrored by the editor (tools/editor/src/gizmo.rs) so its 2D hover tests land exactly
// on the handles the engine draws — change one, change both.
inline constexpr float kGizmoScreenFraction = 0.25f;

class GizmoRenderer {
public:
    // Bakes the two pipelines (opaque lines for handles, alpha-blended triangles for the tint —
    // one shader pair serves both) and uploads the unit handle geometry once. `meshes` is borrowed
    // for the renderer's lifetime (the tint pass re-draws registry meshes).
    GizmoRenderer(rhi::Device& device, const MeshRegistry& meshes);
    ~GizmoRenderer();

    GizmoRenderer(const GizmoRenderer&) = delete;
    GizmoRenderer& operator=(const GizmoRenderer&) = delete;

    // Declare the overlay pass into `graph`, drawing over `target` (the frame's finished LDR:
    // LoadOp::Load, no depth attachment — always-on-top). No-op when there is nothing to draw:
    // no lens, mode None, a dead entity, or an entity with no WorldTransform. The tint draw
    // additionally needs a valid MeshRef; handles render either way (a light or camera entity is
    // still draggable).
    void declare(RenderGraph& graph,
                 ecs::World& world,
                 RGTexture target,
                 const CameraLens& lens,
                 const GizmoSelection& sel);

private:
    struct Range {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    rhi::Device& device_;
    const MeshRegistry& meshes_;

    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle line_pipeline_; // LineList, blend off — the handle geometry
    rhi::PipelineHandle tint_pipeline_; // TriangleList, alpha blend — the selection tint
    rhi::BufferHandle vertices_;        // one static buffer: all modes' unit geometry

    // Per-axis vertex ranges into `vertices_`, per mode (built once in the constructor).
    Range translate_[3];
    Range rotate_[3];
    Range scale_[3];
};

} // namespace rime::render
