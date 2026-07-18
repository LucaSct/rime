// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! **Gizmo math** (m9.6 Part B) — the editor-side half of transform gizmos, as pure functions.
//! The engine renders the handles and ships the exact render lens each frame (the
//! `ViewportCamera` message: `view_proj`, its engine-computed inverse, the eye); this module turns
//! cursor pixels into world-space rays against that lens, decides which handle the cursor is over,
//! and — during a drag — computes the constrained new transform. No egui, no sockets, no I/O:
//! everything here is bit-exactly unit-testable, which is the fence the m9.6 plan demands for
//! "gizmo math edge cases".
//!
//! The math (derived in `docs/math/gizmos.md`):
//!   * **Pixel → ray**: unproject the pixel at the near and far clip planes through
//!     `inv_view_proj` (Vulkan clip z ∈ [0,1]), perspective-divide, and the ray is eye → far.
//!     Because the engine ships the inverse, this module never inverts a matrix.
//!   * **Translate/scale**: the drag is constrained to a world axis line. The cursor ray rarely
//!     *intersects* that line, so we take the parameter `t` of the **closest point on the axis to
//!     the ray** (line/line closest approach). The dragged value is `t_now − t_grab` — which makes
//!     the handle stick to the cursor without ever leaving its rail. Degenerate when the ray is
//!     (near-)parallel to the axis: the closest-approach system loses rank, and we hold the last
//!     good value rather than jump.
//!   * **Rotate**: intersect the cursor ray with the ring's plane, and the drag angle is the
//!     signed angle between the grab vector and the current vector about the ring's axis
//!     (`atan2(dot(cross(a,b), axis), dot(a,b))` — quadrant-correct). Degenerate when the ray is
//!     (near-)parallel to the plane (ring seen edge-on).
//!   * **Screen-constant size**: the engine draws the gizmo at
//!     `dist(eye, entity) · tan(fov/2) · GIZMO_SCREEN_FRACTION` world units; hover math must use
//!     the SAME size or the clickable area drifts off the drawn pixels. `tan(fov/2)` is recovered
//!     from `view_proj` itself (the length of its second row — see `tan_half_fov`), so the wire
//!     needs no extra field.

// ── Minimal linear algebra ──────────────────────────────────────────────────────────────────
// Hand-rolled on purpose (no linalg crate): the editor needs exactly these few operations, the
// wire layout is the engine's column-major [f32; 16], and owning the code means owning the
// conventions (docs/adr/0004: column vectors, v' = M·v, Vulkan clip space).

/// A 3-component vector (the engine's `core::Vec3` twin).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    pub fn add(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }

    pub fn sub(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }

    pub fn scale(self, s: f32) -> Vec3 {
        Vec3::new(self.x * s, self.y * s, self.z * s)
    }

    pub fn dot(self, o: Vec3) -> f32 {
        self.x * o.x + self.y * o.y + self.z * o.z
    }

    pub fn cross(self, o: Vec3) -> Vec3 {
        Vec3::new(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x,
        )
    }

    pub fn length(self) -> f32 {
        self.dot(self).sqrt()
    }

    /// Zero-safe normalize: the zero vector stays zero (callers guard the degenerate uses).
    pub fn normalized(self) -> Vec3 {
        let len = self.length();
        if len > 0.0 {
            self.scale(1.0 / len)
        } else {
            self
        }
    }
}

/// A unit quaternion (x, y, z, w) — the engine's `core::Quat` twin, same component order as the
/// reflection wire (`rotation.x, .y, .z, .w`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Quat {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

impl Quat {
    pub const IDENTITY: Quat = Quat {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    };

    /// Rotation of `angle` radians about the (unit) `axis` — the axis-angle → quaternion map
    /// `q = (sin(θ/2)·axis, cos(θ/2))`.
    pub fn from_axis_angle(axis: Vec3, angle: f32) -> Quat {
        let half = angle * 0.5;
        let s = half.sin();
        Quat {
            x: axis.x * s,
            y: axis.y * s,
            z: axis.z * s,
            w: half.cos(),
        }
    }

    /// Hamilton product `self · rhs` — applying `rhs` FIRST, then `self` (matching the engine's
    /// `parent.rotation * child.rotation` composition order).
    pub fn mul(self, rhs: Quat) -> Quat {
        Quat {
            x: self.w * rhs.x + self.x * rhs.w + self.y * rhs.z - self.z * rhs.y,
            y: self.w * rhs.y - self.x * rhs.z + self.y * rhs.w + self.z * rhs.x,
            z: self.w * rhs.z + self.x * rhs.y - self.y * rhs.x + self.z * rhs.w,
            w: self.w * rhs.w - self.x * rhs.x - self.y * rhs.y - self.z * rhs.z,
        }
    }

    /// Renormalize — a drag composes many small rotations; this keeps the quaternion unit-length
    /// (the drift discipline every quaternion pipeline needs).
    pub fn normalized(self) -> Quat {
        let len = (self.x * self.x + self.y * self.y + self.z * self.z + self.w * self.w).sqrt();
        if len > 0.0 {
            Quat {
                x: self.x / len,
                y: self.y / len,
                z: self.z / len,
                w: self.w / len,
            }
        } else {
            Quat::IDENTITY
        }
    }
}

/// A column-major 4×4 matrix as its 16 wire floats (`core::Mat4::m`): element (row r, col c) is
/// `m[c*4 + r]`.
pub type Mat4 = [f32; 16];

/// `M · v` for a homogeneous 4-vector (column-vector convention).
pub fn mul_vec4(m: &Mat4, v: [f32; 4]) -> [f32; 4] {
    let mut out = [0.0f32; 4];
    for (row, out_row) in out.iter_mut().enumerate() {
        *out_row = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    }
    out
}

/// Recover `tan(fov_y / 2)` from a `perspective · view` product. The engine's perspective has one
/// non-zero entry in row 1 — `at(1,1) = −1/tan(fov/2)` (the Vulkan y-flip) — and a rigid view
/// matrix's rows are unit-length, so row 1 of the product has length `1/tan(fov/2)` exactly.
/// This is how the editor sizes its hover targets to the engine's screen-constant gizmo without
/// the wire carrying the fov separately.
pub fn tan_half_fov(view_proj: &Mat4) -> f32 {
    let row1 = Vec3::new(view_proj[1], view_proj[5], view_proj[9]);
    let len = row1.length();
    if len > 0.0 {
        1.0 / len
    } else {
        0.0
    }
}

// ── Projection / unprojection ───────────────────────────────────────────────────────────────

/// A world-space ray (origin + unit direction).
#[derive(Debug, Clone, Copy)]
pub struct Ray {
    pub origin: Vec3,
    pub dir: Vec3,
}

/// Unproject a viewport pixel (continuous engine-frame coordinates) into a world ray, using the
/// engine-shipped `inv_view_proj` and `eye`. The pixel maps to NDC by the exact inverse of
/// `project_point`; unprojecting at clip z = 0 (near) and z = 1 (far — Vulkan's [0,1] depth
/// range) and perspective-dividing gives two world points on the pixel's line of sight. For a
/// perspective camera that line passes through the eye, so the eye is the origin and far−near the
/// direction.
pub fn ray_from_pixel(inv_view_proj: &Mat4, eye: Vec3, px: (f32, f32), extent: (f32, f32)) -> Ray {
    let ndc_x = px.0 / extent.0 * 2.0 - 1.0;
    let ndc_y = px.1 / extent.1 * 2.0 - 1.0;
    let near = mul_vec4(inv_view_proj, [ndc_x, ndc_y, 0.0, 1.0]);
    let far = mul_vec4(inv_view_proj, [ndc_x, ndc_y, 1.0, 1.0]);
    let near = Vec3::new(near[0] / near[3], near[1] / near[3], near[2] / near[3]);
    let far = Vec3::new(far[0] / far[3], far[1] / far[3], far[2] / far[3]);
    Ray {
        origin: eye,
        dir: far.sub(near).normalized(),
    }
}

/// Project a world point to continuous viewport-pixel coordinates. `None` when the point is at or
/// behind the eye plane (clip w ≤ ε) — such a point has no meaningful screen position, and hover
/// tests must skip it rather than divide by (nearly) zero.
pub fn project_point(view_proj: &Mat4, extent: (f32, f32), world: Vec3) -> Option<(f32, f32)> {
    let clip = mul_vec4(view_proj, [world.x, world.y, world.z, 1.0]);
    if clip[3] <= 1.0e-6 {
        return None;
    }
    let x = (clip[0] / clip[3] * 0.5 + 0.5) * extent.0;
    let y = (clip[1] / clip[3] * 0.5 + 0.5) * extent.1;
    Some((x, y))
}

// ── The constrained-drag primitives ─────────────────────────────────────────────────────────

/// The parameter `t` of the point on the axis line `axis_origin + t·axis_dir` closest to `ray`
/// (line/line closest approach — docs/math/gizmos.md derives the 2×2 system). `None` when the ray
/// and axis are (near-)parallel: the system's determinant `1 − (d̂·v̂)² → 0`, the closest point
/// races to infinity, and a drag must HOLD rather than jump. `axis_dir` must be unit-length.
pub fn closest_t_on_axis(ray: &Ray, axis_origin: Vec3, axis_dir: Vec3) -> Option<f32> {
    let b = axis_dir.dot(ray.dir); // cos of the angle between the lines (both unit)
    let denom = 1.0 - b * b;
    if denom < 1.0e-6 {
        return None; // near-parallel: ill-conditioned, refuse
    }
    // With w = axis_origin − ray_origin, minimising |axis(t) − ray(s)|² over (t, s) gives
    //   t = (b·e − d) / (1 − b²),   d = axis_dir·w,   e = ray_dir·w
    // (set both partial derivatives to zero and eliminate s — the derivation, including why the
    // sign of w matters, is worked in docs/math/gizmos.md).
    let w = axis_origin.sub(ray.origin);
    let d = axis_dir.dot(w);
    let e = ray.dir.dot(w);
    Some((b * e - d) / denom)
}

/// Intersect `ray` with the plane through `point` with (unit) `normal`. `None` when the ray is
/// (near-)parallel to the plane, or the hit is behind the ray's origin (the ring's plane behind
/// the camera — nothing draggable there).
pub fn ray_plane_intersect(ray: &Ray, point: Vec3, normal: Vec3) -> Option<Vec3> {
    let denom = ray.dir.dot(normal);
    if denom.abs() < 1.0e-6 {
        return None;
    }
    let t = point.sub(ray.origin).dot(normal) / denom;
    if t < 0.0 {
        return None;
    }
    Some(ray.origin.add(ray.dir.scale(t)))
}

/// The signed angle from `from` to `to` about `axis` (all in the plane ⊥ `axis`; `axis` unit).
/// `atan2(dot(cross(from, to), axis), dot(from, to))` — quadrant-correct over (−π, π], positive
/// counter-clockwise when viewed from the axis tip (the right-hand rule).
pub fn signed_angle_on_plane(from: Vec3, to: Vec3, axis: Vec3) -> f32 {
    from.cross(to).dot(axis).atan2(from.dot(to))
}

/// Quantize `value` to multiples of `step` (`None` = no snapping). Round-to-nearest, so snapping
/// engages symmetrically in both drag directions.
pub fn snap(value: f32, step: Option<f32>) -> f32 {
    match step {
        Some(s) if s > 0.0 => (value / s).round() * s,
        _ => value,
    }
}

// ── Gizmo vocabulary ────────────────────────────────────────────────────────────────────────

/// The gizmo modes (the wire's `GizmoState.mode` semantics, minus "none" — a hidden gizmo has no
/// math to do).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Translate,
    Rotate,
    Scale,
}

/// A world axis. Handles are world-aligned in v1 ("global" space — the engine renders them the
/// same way; gizmo_renderer.hpp documents the pairing).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Axis {
    X,
    Y,
    Z,
}

impl Axis {
    pub fn dir(self) -> Vec3 {
        match self {
            Axis::X => Vec3::new(1.0, 0.0, 0.0),
            Axis::Y => Vec3::new(0.0, 1.0, 0.0),
            Axis::Z => Vec3::new(0.0, 0.0, 1.0),
        }
    }

    /// The two axes spanning this axis' normal plane — the ring's plane, ordered to match the
    /// engine's ring parameterisation (gizmo_renderer.cpp's kAxisPerp1/2).
    pub fn perp(self) -> (Vec3, Vec3) {
        match self {
            Axis::X => (Vec3::new(0.0, 1.0, 0.0), Vec3::new(0.0, 0.0, 1.0)),
            Axis::Y => (Vec3::new(0.0, 0.0, 1.0), Vec3::new(1.0, 0.0, 0.0)),
            Axis::Z => (Vec3::new(1.0, 0.0, 0.0), Vec3::new(0.0, 1.0, 0.0)),
        }
    }
}

/// The fraction of the viewport half-height the gizmo spans — MUST equal the engine's
/// `render::kGizmoScreenFraction` (gizmo_renderer.hpp) or hover targets drift off the drawn
/// handles. Change one, change both.
pub const GIZMO_SCREEN_FRACTION: f32 = 0.25;

/// The engine gizmo's world size at `center`: `dist(eye, center) · tan(fov/2) · fraction` — the
/// screen-constant scaling, mirrored exactly (tan recovered from the lens itself).
pub fn gizmo_world_size(view_proj: &Mat4, eye: Vec3, center: Vec3) -> f32 {
    let dist = center.sub(eye).length();
    (dist * tan_half_fov(view_proj) * GIZMO_SCREEN_FRACTION).max(1.0e-4)
}

// ── Hover: which handle is the cursor over? ─────────────────────────────────────────────────

/// Distance from point `p` to the 2D segment `[a, b]` — the shaft/tip hit test in pixels.
fn dist_point_segment_2d(p: (f32, f32), a: (f32, f32), b: (f32, f32)) -> f32 {
    let ab = (b.0 - a.0, b.1 - a.1);
    let ap = (p.0 - a.0, p.1 - a.1);
    let len2 = ab.0 * ab.0 + ab.1 * ab.1;
    let t = if len2 > 0.0 {
        ((ap.0 * ab.0 + ap.1 * ab.1) / len2).clamp(0.0, 1.0)
    } else {
        0.0
    };
    let cx = a.0 + ab.0 * t - p.0;
    let cy = a.1 + ab.1 * t - p.1;
    (cx * cx + cy * cy).sqrt()
}

/// The pixel radius inside which a handle is grabbable. Generous on purpose: the handles are
/// 1-px lines on the stream, and a gizmo you must pixel-hunt is a broken gizmo.
pub const HOVER_RADIUS_PX: f32 = 10.0;

/// Which axis handle (if any) is under the cursor. `cursor` is in continuous engine-frame pixels
/// (the ViewportCamera extent), `center` the entity's world position, `size` the gizmo's world
/// size (`gizmo_world_size`). Translate/scale test the projected axis SEGMENTS (shaft + tip);
/// rotate tests each ring as a projected polyline (rings project to ellipses — sampling beats
/// solving a conic for a hover test). Nearest handle within `HOVER_RADIUS_PX` wins.
pub fn hover_axis(
    mode: Mode,
    view_proj: &Mat4,
    extent: (f32, f32),
    center: Vec3,
    size: f32,
    cursor: (f32, f32),
) -> Option<Axis> {
    let mut best: Option<(Axis, f32)> = None;
    let mut consider = |axis: Axis, d: f32| {
        if d <= HOVER_RADIUS_PX && best.is_none_or(|(_, bd)| d < bd) {
            best = Some((axis, d));
        }
    };
    for axis in [Axis::X, Axis::Y, Axis::Z] {
        match mode {
            Mode::Translate | Mode::Scale => {
                let (Some(a), Some(b)) = (
                    project_point(view_proj, extent, center),
                    project_point(view_proj, extent, center.add(axis.dir().scale(size))),
                ) else {
                    continue; // an endpoint behind the eye: no meaningful screen segment
                };
                consider(axis, dist_point_segment_2d(cursor, a, b));
            }
            Mode::Rotate => {
                // Sample the ring (32 chords approximate the projected ellipse to well under a
                // pixel at gizmo scale) and take the min point-to-polyline distance.
                const SAMPLES: usize = 32;
                let (p1, p2) = axis.perp();
                let mut prev: Option<(f32, f32)> = None;
                let mut min_d = f32::INFINITY;
                for i in 0..=SAMPLES {
                    let ang = (i % SAMPLES) as f32 / SAMPLES as f32 * std::f32::consts::TAU;
                    let world = center
                        .add(p1.scale(ang.cos() * size))
                        .add(p2.scale(ang.sin() * size));
                    let Some(pt) = project_point(view_proj, extent, world) else {
                        prev = None; // the ring dips behind the eye: break the polyline there
                        continue;
                    };
                    if let Some(pr) = prev {
                        min_d = min_d.min(dist_point_segment_2d(cursor, pr, pt));
                    }
                    prev = Some(pt);
                }
                consider(axis, min_d);
            }
        }
    }
    best.map(|(axis, _)| axis)
}

// ── The transform blob (LocalTransform over the wire) ───────────────────────────────────────

/// A decoded TRS transform — the engine's `core::Transform` (and the payload of a
/// `LocalTransform` component: one nested `value` field).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Trs {
    pub translation: Vec3,
    pub rotation: Quat,
    pub scale: Vec3,
}

/// The reflection-serialized `LocalTransform` blob: 10 packed little-endian f32s in declared
/// field order — translation x,y,z · rotation x,y,z,w · scale x,y,z (core/math/reflect.hpp).
/// The GUI validates this layout against the live schema once before enabling the gizmo (names
/// and kinds), so this direct codec can never silently disagree with the engine.
pub const TRS_BLOB_LEN: usize = 40;

/// Decode a `LocalTransform` blob. `None` if it is not exactly the 10-float layout.
pub fn decode_trs_blob(blob: &[u8]) -> Option<Trs> {
    if blob.len() != TRS_BLOB_LEN {
        return None;
    }
    let f = |i: usize| f32::from_le_bytes(blob[i * 4..i * 4 + 4].try_into().unwrap());
    Some(Trs {
        translation: Vec3::new(f(0), f(1), f(2)),
        rotation: Quat {
            x: f(3),
            y: f(4),
            z: f(5),
            w: f(6),
        },
        scale: Vec3::new(f(7), f(8), f(9)),
    })
}

/// Encode a `Trs` back into the wire blob — the exact inverse of [`decode_trs_blob`] (a decode →
/// encode round-trip is bit-identical).
pub fn encode_trs_blob(t: &Trs) -> Vec<u8> {
    let mut out = Vec::with_capacity(TRS_BLOB_LEN);
    for v in [
        t.translation.x,
        t.translation.y,
        t.translation.z,
        t.rotation.x,
        t.rotation.y,
        t.rotation.z,
        t.rotation.w,
        t.scale.x,
        t.scale.y,
        t.scale.z,
    ] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

// ── The drag session ────────────────────────────────────────────────────────────────────────

/// Snapping configuration for a drag (Ctrl held, or a toolbar toggle). `linear` quantizes
/// translate deltas and scale values (world units); `angular` quantizes rotate deltas (radians).
#[derive(Debug, Clone, Copy, Default)]
pub struct Snapping {
    pub linear: Option<f32>,
    pub angular: Option<f32>,
}

/// One gizmo drag from grab to release, as pure state: everything is anchored at the grab
/// (`begin`), and every update computes the new transform FROM THE ANCHOR — never incrementally —
/// so a drag is stateless per frame, immune to event-rate jitter, and trivially replayable in a
/// test. The session works on component BLOBS at the edges (grab bytes in, new bytes out): the
/// GUI wraps it with `SetComponent` sends and folds the whole session into ONE undo `Edit`
/// (`old_blob` → the last update's bytes) at release.
#[derive(Debug, Clone)]
pub struct DragSession {
    mode: Mode,
    axis: Axis,
    start: Trs,
    /// The grab-time component bytes, verbatim — the undo `Edit`'s bit-exact inverse.
    old_blob: Vec<u8>,
    center: Vec3,
    /// Translate/scale: the axis parameter at grab. Rotate: unused (0).
    anchor_t: f32,
    /// Rotate: the grab vector in the ring plane. Translate/scale: unused (zero).
    anchor_vec: Vec3,
}

impl DragSession {
    /// Begin a drag: decode the grab-time blob and anchor the cursor ray on the constraint
    /// (axis parameter for translate/scale, ring-plane vector for rotate). `None` when the blob
    /// is not a TRS, or the grab geometry is degenerate — ray parallel to the axis / plane, a
    /// zero grab vector, or a scale grab at the axis origin (t ≈ 0 would make every later factor
    /// t/t₀ explode). Refusing the grab is the right UX: a drag that cannot compute must not
    /// start, rather than start and teleport the object.
    pub fn begin(
        mode: Mode,
        axis: Axis,
        old_blob: &[u8],
        center: Vec3,
        ray: &Ray,
    ) -> Option<DragSession> {
        let start = decode_trs_blob(old_blob)?;
        let mut anchor_t = 0.0f32;
        let mut anchor_vec = Vec3::default();
        match mode {
            Mode::Translate | Mode::Scale => {
                anchor_t = closest_t_on_axis(ray, center, axis.dir())?;
                if mode == Mode::Scale && anchor_t.abs() < 1.0e-4 {
                    return None; // grabbed the origin: no meaningful scale reference
                }
            }
            Mode::Rotate => {
                let hit = ray_plane_intersect(ray, center, axis.dir())?;
                anchor_vec = hit.sub(center);
                if anchor_vec.length() < 1.0e-4 {
                    return None; // grabbed the exact ring centre: no reference direction
                }
            }
        }
        Some(DragSession {
            mode,
            axis,
            start,
            old_blob: old_blob.to_vec(),
            center,
            anchor_t,
            anchor_vec,
        })
    }

    /// The grab-time component bytes — the bit-exact inverse for the drag's single undo step.
    pub fn old_blob(&self) -> &[u8] {
        &self.old_blob
    }

    pub fn axis(&self) -> Axis {
        self.axis
    }

    /// Compute the transform for the current cursor ray, as wire bytes. Anchor-relative (see the
    /// type docs); a degenerate current ray (parallel to the constraint) holds the grab-time
    /// transform instead of jumping — the drag simply stops following until the cursor returns to
    /// a computable region.
    pub fn update(&self, ray: &Ray, snapping: Snapping) -> Vec<u8> {
        let mut now = self.start;
        match self.mode {
            Mode::Translate => {
                if let Some(t) = closest_t_on_axis(ray, self.center, self.axis.dir()) {
                    let delta = snap(t - self.anchor_t, snapping.linear);
                    now.translation = self.start.translation.add(self.axis.dir().scale(delta));
                }
            }
            Mode::Scale => {
                if let Some(t) = closest_t_on_axis(ray, self.center, self.axis.dir()) {
                    // Multiplicative, like every editor: dragging the cube to twice its grab
                    // distance doubles that axis' scale. The grab guarded anchor_t ≠ 0.
                    let factor = t / self.anchor_t;
                    let scaled = match self.axis {
                        Axis::X => self.start.scale.x * factor,
                        Axis::Y => self.start.scale.y * factor,
                        Axis::Z => self.start.scale.z * factor,
                    };
                    let snapped = snap(scaled, snapping.linear);
                    match self.axis {
                        Axis::X => now.scale.x = snapped,
                        Axis::Y => now.scale.y = snapped,
                        Axis::Z => now.scale.z = snapped,
                    }
                }
            }
            Mode::Rotate => {
                if let Some(hit) = ray_plane_intersect(ray, self.center, self.axis.dir()) {
                    let v = hit.sub(self.center);
                    if v.length() >= 1.0e-4 {
                        let angle = snap(
                            signed_angle_on_plane(self.anchor_vec, v, self.axis.dir()),
                            snapping.angular,
                        );
                        // A WORLD-axis rotation pre-multiplies (world = delta · start); the
                        // engine composes parent·child the same way. Renormalized against drift.
                        now.rotation = Quat::from_axis_angle(self.axis.dir(), angle)
                            .mul(self.start.rotation)
                            .normalized();
                    }
                }
            }
        }
        encode_trs_blob(&now)
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // A test-local general 4×4 inverse (Gauss-Jordan). PRODUCTION code never inverts — the engine
    // ships inv_view_proj — but tests must build both halves of a lens from scratch.
    fn invert4x4(m: &Mat4) -> Mat4 {
        let mut a = *m;
        let mut inv: Mat4 = [
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
        ];
        for col in 0..4 {
            // Partial pivot: the largest |entry| in this column keeps the elimination stable.
            let mut pivot = col;
            for r in col + 1..4 {
                if a[col * 4 + r].abs() > a[col * 4 + pivot].abs() {
                    pivot = r;
                }
            }
            if pivot != col {
                for c in 0..4 {
                    a.swap(c * 4 + col, c * 4 + pivot);
                    inv.swap(c * 4 + col, c * 4 + pivot);
                }
            }
            let d = a[col * 4 + col];
            assert!(d.abs() > 1.0e-12, "singular matrix in test");
            for c in 0..4 {
                a[c * 4 + col] /= d;
                inv[c * 4 + col] /= d;
            }
            for r in 0..4 {
                if r != col {
                    let f = a[col * 4 + r];
                    for c in 0..4 {
                        a[c * 4 + r] -= f * a[c * 4 + col];
                        inv[c * 4 + r] -= f * inv[c * 4 + col];
                    }
                }
            }
        }
        inv
    }

    // The engine's perspective (mat.cpp): right-handed, Vulkan clip space, y-flip in (1,1).
    fn perspective(fov_y: f32, aspect: f32, z_near: f32, z_far: f32) -> Mat4 {
        let tan_half = (fov_y * 0.5).tan();
        let mut m = [0.0f32; 16];
        m[0] = 1.0 / (aspect * tan_half); // at(0,0)
        m[5] = -1.0 / tan_half; // at(1,1)
        m[10] = z_far / (z_near - z_far); // at(2,2)
        m[11] = -1.0; // at(3,2)
        m[14] = (z_far * z_near) / (z_near - z_far); // at(2,3)
        m
    }

    fn mat_mul(a: &Mat4, b: &Mat4) -> Mat4 {
        let mut out = [0.0f32; 16];
        for col in 0..4 {
            for row in 0..4 {
                let mut sum = 0.0;
                for k in 0..4 {
                    sum += a[k * 4 + row] * b[col * 4 + k];
                }
                out[col * 4 + row] = sum;
            }
        }
        out
    }

    fn translation(t: Vec3) -> Mat4 {
        let mut m: Mat4 = [
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
        ];
        m[12] = t.x;
        m[13] = t.y;
        m[14] = t.z;
        m
    }

    /// The standard test lens: camera at `eye`, identity rotation (looking down −z), fov 0.9,
    /// square viewport — view = translate(−eye), the engine's convention.
    fn lens(eye: Vec3) -> (Mat4, Mat4) {
        let vp = mat_mul(
            &perspective(0.9, 1.0, 0.1, 1000.0),
            &translation(Vec3::new(-eye.x, -eye.y, -eye.z)),
        );
        let inv = invert4x4(&vp);
        (vp, inv)
    }

    const EXTENT: (f32, f32) = (960.0, 540.0);

    #[test]
    fn project_then_unproject_round_trips() {
        let eye = Vec3::new(2.0, 1.5, 8.0);
        let (vp, inv) = lens(eye);
        for world in [
            Vec3::new(0.0, 0.0, 0.0),
            Vec3::new(1.5, -2.0, 3.0),
            Vec3::new(-3.0, 1.0, -4.0),
        ] {
            let px = project_point(&vp, EXTENT, world).expect("in front of the eye");
            let ray = ray_from_pixel(&inv, eye, px, EXTENT);
            // The unprojected ray must pass through the point that projected to the pixel:
            // distance from `world` to the ray < 1e-3 (f32 through two matrix trips).
            let to_point = world.sub(ray.origin);
            let along = to_point.dot(ray.dir);
            let closest = ray.origin.add(ray.dir.scale(along));
            assert!(
                world.sub(closest).length() < 1.0e-3,
                "round-trip missed: {world:?}"
            );
        }
    }

    #[test]
    fn project_point_rejects_points_behind_the_eye() {
        let eye = Vec3::new(0.0, 0.0, 8.0);
        let (vp, _) = lens(eye);
        assert!(project_point(&vp, EXTENT, Vec3::new(0.0, 0.0, 20.0)).is_none());
    }

    #[test]
    fn tan_half_fov_recovers_the_lens_fov() {
        let (vp, _) = lens(Vec3::new(3.0, -1.0, 5.0));
        assert!((tan_half_fov(&vp) - (0.45f32).tan()).abs() < 1.0e-5);
    }

    #[test]
    fn closest_t_on_axis_known_cases_and_degeneracy() {
        // A ray dropping straight down onto the X axis at x = 5.
        let ray = Ray {
            origin: Vec3::new(5.0, 4.0, 0.0),
            dir: Vec3::new(0.0, -1.0, 0.0),
        };
        let t = closest_t_on_axis(&ray, Vec3::default(), Vec3::new(1.0, 0.0, 0.0)).unwrap();
        assert!((t - 5.0).abs() < 1.0e-6);

        // A skew ray: origin (0, 1, 3), direction −z — closest X-axis point is x = 0.
        let ray = Ray {
            origin: Vec3::new(0.0, 1.0, 3.0),
            dir: Vec3::new(0.0, 0.0, -1.0),
        };
        let t = closest_t_on_axis(&ray, Vec3::default(), Vec3::new(1.0, 0.0, 0.0)).unwrap();
        assert!(t.abs() < 1.0e-6);

        // Near-parallel: a ray ALONG the axis has no closest parameter — must refuse, not jump.
        let ray = Ray {
            origin: Vec3::new(0.0, 1.0, 0.0),
            dir: Vec3::new(1.0, 0.0, 0.0),
        };
        assert!(closest_t_on_axis(&ray, Vec3::default(), Vec3::new(1.0, 0.0, 0.0)).is_none());
    }

    #[test]
    fn ray_plane_intersect_hits_misses_and_degenerates() {
        let ray = Ray {
            origin: Vec3::new(1.0, 2.0, 8.0),
            dir: Vec3::new(0.0, 0.0, -1.0),
        };
        let hit = ray_plane_intersect(&ray, Vec3::default(), Vec3::new(0.0, 0.0, 1.0)).unwrap();
        assert!(hit.sub(Vec3::new(1.0, 2.0, 0.0)).length() < 1.0e-6);

        // Parallel to the plane: refuse.
        let ray = Ray {
            origin: Vec3::new(0.0, 0.0, 8.0),
            dir: Vec3::new(1.0, 0.0, 0.0),
        };
        assert!(ray_plane_intersect(&ray, Vec3::default(), Vec3::new(0.0, 0.0, 1.0)).is_none());

        // The plane behind the ray: refuse (t < 0).
        let ray = Ray {
            origin: Vec3::new(0.0, 0.0, -1.0),
            dir: Vec3::new(0.0, 0.0, -1.0),
        };
        assert!(ray_plane_intersect(&ray, Vec3::default(), Vec3::new(0.0, 0.0, 1.0)).is_none());
    }

    #[test]
    fn signed_angle_is_quadrant_correct() {
        let x = Vec3::new(1.0, 0.0, 0.0);
        let y = Vec3::new(0.0, 1.0, 0.0);
        let z = Vec3::new(0.0, 0.0, 1.0);
        assert!((signed_angle_on_plane(x, y, z) - std::f32::consts::FRAC_PI_2).abs() < 1.0e-6);
        assert!((signed_angle_on_plane(y, x, z) + std::f32::consts::FRAC_PI_2).abs() < 1.0e-6);
        // Just past a half turn lands near ±π (the atan2 seam) without blowing up.
        let almost_back = Vec3::new(-1.0, -1.0e-3, 0.0);
        assert!(signed_angle_on_plane(x, almost_back, z).abs() > 3.0);
    }

    #[test]
    fn snapping_quantizes_symmetrically() {
        assert_eq!(snap(1.06, Some(0.25)), 1.0);
        assert_eq!(snap(1.13, Some(0.25)), 1.25);
        assert_eq!(snap(-0.9, Some(0.25)), -1.0);
        assert_eq!(snap(0.37, None), 0.37);
    }

    #[test]
    fn trs_blob_round_trips_bit_exact() {
        let t = Trs {
            translation: Vec3::new(1.5, -2.25, 3.125),
            rotation: Quat {
                x: 0.1,
                y: 0.2,
                z: 0.3,
                w: 0.9,
            },
            scale: Vec3::new(1.0, 2.0, 0.5),
        };
        let blob = encode_trs_blob(&t);
        assert_eq!(blob.len(), TRS_BLOB_LEN);
        let back = decode_trs_blob(&blob).unwrap();
        assert_eq!(encode_trs_blob(&back), blob); // bit-exact round trip
        assert!(decode_trs_blob(&blob[..39]).is_none()); // wrong length refuses
    }

    #[test]
    fn hover_finds_the_handle_under_the_cursor() {
        let eye = Vec3::new(0.0, 0.0, 8.0);
        let (vp, _) = lens(eye);
        let center = Vec3::default();
        let size = gizmo_world_size(&vp, eye, center);

        // The +X tip projects somewhere right of centre; the cursor exactly there hovers X.
        let tip = project_point(&vp, EXTENT, center.add(Vec3::new(size, 0.0, 0.0))).unwrap();
        assert_eq!(
            hover_axis(Mode::Translate, &vp, EXTENT, center, size, tip),
            Some(Axis::X)
        );
        // Mid-shaft of +Y hovers Y (the whole shaft is grabbable, not just the tip).
        let mid = project_point(&vp, EXTENT, center.add(Vec3::new(0.0, size * 0.5, 0.0))).unwrap();
        assert_eq!(
            hover_axis(Mode::Translate, &vp, EXTENT, center, size, mid),
            Some(Axis::Y)
        );
        // Far from every handle: nothing.
        assert_eq!(
            hover_axis(Mode::Translate, &vp, EXTENT, center, size, (30.0, 30.0)),
            None
        );
        // Rotate: a point on the Z ring (radius `size` in the XY plane) hovers Z.
        let on_ring = project_point(&vp, EXTENT, center.add(Vec3::new(0.0, size, 0.0))).unwrap();
        // (0, size, 0) lies on BOTH the Z ring and the X ring — the nearest wins; accept either
        // of the two coincident rings, but never Y (whose ring is edge-on through this point).
        let hovered = hover_axis(Mode::Rotate, &vp, EXTENT, center, size, on_ring);
        assert!(hovered == Some(Axis::Z) || hovered == Some(Axis::X));
    }

    // ── The drag sessions: scripted grabs → bit-exact deltas, exact inverses ──────────────

    /// Drive one drag: grab at the pixel of `grab_world`, move to the pixel of `move_world`,
    /// return (session, new_blob).
    #[allow(clippy::too_many_arguments)] // a scripted drag genuinely takes the whole scene
    fn scripted_drag(
        mode: Mode,
        axis: Axis,
        start: &Trs,
        center: Vec3,
        eye: Vec3,
        grab_world: Vec3,
        move_world: Vec3,
        snapping: Snapping,
    ) -> (DragSession, Vec<u8>) {
        let (vp, inv) = lens(eye);
        let old_blob = encode_trs_blob(start);
        let grab_px = project_point(&vp, EXTENT, grab_world).unwrap();
        let grab_ray = ray_from_pixel(&inv, eye, grab_px, EXTENT);
        let session =
            DragSession::begin(mode, axis, &old_blob, center, &grab_ray).expect("grab must anchor");
        let move_px = project_point(&vp, EXTENT, move_world).unwrap();
        let move_ray = ray_from_pixel(&inv, eye, move_px, EXTENT);
        let blob = session.update(&move_ray, snapping);
        (session, blob)
    }

    #[test]
    fn translate_drag_moves_by_the_expected_delta_and_keeps_the_inverse_bit_exact() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        let eye = Vec3::new(0.0, 0.0, 8.0);
        // Grab the X handle at 0.5 world units out, drag the cursor to where 1.5 projects: the
        // constrained delta must be +1 X — the "scripted drag moves the entity by the expected
        // delta" proof of the m9.6 plan.
        let (session, blob) = scripted_drag(
            Mode::Translate,
            Axis::X,
            &start,
            Vec3::default(),
            eye,
            Vec3::new(0.5, 0.0, 0.0),
            Vec3::new(1.5, 0.0, 0.0),
            Snapping::default(),
        );
        let moved = decode_trs_blob(&blob).unwrap();
        assert!((moved.translation.x - 1.0).abs() < 1.0e-3);
        assert!(moved.translation.y.abs() < 1.0e-4 && moved.translation.z.abs() < 1.0e-4);
        // Rotation/scale ride through UNTOUCHED — bit-exact (a translate must never disturb them).
        assert_eq!(moved.rotation, start.rotation);
        assert_eq!(moved.scale, start.scale);
        // The session's inverse is the grab-time bytes, verbatim.
        assert_eq!(session.old_blob(), encode_trs_blob(&start));
    }

    #[test]
    fn translate_drag_snaps_to_the_linear_grid() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        let (_, blob) = scripted_drag(
            Mode::Translate,
            Axis::X,
            &start,
            Vec3::default(),
            Vec3::new(0.0, 0.0, 8.0),
            Vec3::new(0.5, 0.0, 0.0),
            Vec3::new(1.56, 0.0, 0.0), // a raw delta of ~1.06
            Snapping {
                linear: Some(0.25),
                angular: None,
            },
        );
        let moved = decode_trs_blob(&blob).unwrap();
        // snap(≈1.06, 0.25) = 1.0 exactly (0.25 · 4 is exact in binary floating point).
        assert_eq!(moved.translation.x, 1.0);
    }

    #[test]
    fn rotate_drag_turns_about_the_axis_by_the_swept_angle() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        let eye = Vec3::new(0.0, 0.0, 8.0);
        // Grab the Z ring where it crosses +X, drag to +Y: a quarter turn about Z, so the
        // quaternion must be (0, 0, sin 45°, cos 45°).
        let (_, blob) = scripted_drag(
            Mode::Rotate,
            Axis::Z,
            &start,
            Vec3::default(),
            eye,
            Vec3::new(1.0, 0.0, 0.0),
            Vec3::new(0.0, 1.0, 0.0),
            Snapping::default(),
        );
        let moved = decode_trs_blob(&blob).unwrap();
        let s45 = std::f32::consts::FRAC_PI_4.sin();
        assert!((moved.rotation.z - s45).abs() < 1.0e-3);
        assert!((moved.rotation.w - s45).abs() < 1.0e-3);
        assert!(moved.rotation.x.abs() < 1.0e-4 && moved.rotation.y.abs() < 1.0e-4);
        assert_eq!(moved.translation, start.translation);
    }

    #[test]
    fn rotate_drag_snaps_to_the_angular_grid() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        // Sweep ~83° with a 15° snap: 83/15 = 5.53 → 6 steps = 90°.
        let a = 83.0f32.to_radians();
        let (_, blob) = scripted_drag(
            Mode::Rotate,
            Axis::Z,
            &start,
            Vec3::default(),
            Vec3::new(0.0, 0.0, 8.0),
            Vec3::new(1.0, 0.0, 0.0),
            Vec3::new(a.cos(), a.sin(), 0.0),
            Snapping {
                linear: None,
                angular: Some(15.0f32.to_radians()),
            },
        );
        let moved = decode_trs_blob(&blob).unwrap();
        let s45 = std::f32::consts::FRAC_PI_4.sin();
        assert!((moved.rotation.z - s45).abs() < 1.0e-3); // exactly the 90° quaternion
    }

    #[test]
    fn scale_drag_multiplies_the_grabbed_axis() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        // Grab the X cube at 0.9 units, drag to where 1.8 projects: factor 2 on X only.
        let (_, blob) = scripted_drag(
            Mode::Scale,
            Axis::X,
            &start,
            Vec3::default(),
            Vec3::new(0.0, 0.0, 8.0),
            Vec3::new(0.9, 0.0, 0.0),
            Vec3::new(1.8, 0.0, 0.0),
            Snapping::default(),
        );
        let moved = decode_trs_blob(&blob).unwrap();
        assert!((moved.scale.x - 2.0).abs() < 1.0e-3);
        assert_eq!(moved.scale.y, 1.0);
        assert_eq!(moved.scale.z, 1.0);
    }

    #[test]
    fn degenerate_grabs_refuse_and_degenerate_updates_hold() {
        let start = Trs {
            translation: Vec3::new(0.0, 0.0, 0.0),
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        };
        let old = encode_trs_blob(&start);
        // A grab ray ALONG the drag axis cannot anchor (the near-parallel degeneracy).
        let along = Ray {
            origin: Vec3::new(-5.0, 0.001, 0.0),
            dir: Vec3::new(1.0, 0.0, 0.0),
        };
        assert!(
            DragSession::begin(Mode::Translate, Axis::X, &old, Vec3::default(), &along).is_none()
        );
        // A rotate grab with the ring edge-on (ray in the ring plane) cannot anchor either.
        let in_plane = Ray {
            origin: Vec3::new(0.0, -5.0, 0.0),
            dir: Vec3::new(0.0, 1.0, 0.0),
        };
        assert!(
            DragSession::begin(Mode::Rotate, Axis::Y, &old, Vec3::default(), &in_plane).is_none()
        );
        // A good grab followed by a degenerate MOVE holds the start transform (no NaN, no jump).
        let good = Ray {
            origin: Vec3::new(0.5, 0.0, 8.0),
            dir: Vec3::new(0.0, 0.0, -1.0),
        };
        let session =
            DragSession::begin(Mode::Translate, Axis::X, &old, Vec3::default(), &good).unwrap();
        assert_eq!(session.update(&along, Snapping::default()), old);
        // A truncated blob refuses to even begin.
        assert!(
            DragSession::begin(Mode::Translate, Axis::X, &old[..12], Vec3::default(), &good)
                .is_none()
        );
    }
}
