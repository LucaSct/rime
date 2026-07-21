// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

// The render-facing ECS components (M5.5): how an entity says "draw me". All trivially-copyable
// PODs (the ADR-0018 storage contract) carrying dense ids into the mesh/material registries —
// never handles or pointers. Positions/orientations deliberately do NOT live here: they ride the
// M4.5 transform components (WorldTransform is what the renderer consumes), so "where" and "what
// it looks like" stay orthogonal. Every component is registered through reflection
// (register_render_components), which is the ADR-0016 editor-enabler bet paying forward:
// described once ⇒ serializable now, inspectable at M9, replicable at M11.
//
// Color fields are flat floats (not a math vector) on purpose: reflection describes primitive
// fields today, and r/g/b sliders are exactly what an inspector wants anyway.
namespace rime::render {

// Draw this entity with that registry mesh.
struct MeshRef {
    MeshId mesh = kInvalidMeshId;
};

// An **authoring** reference to a cooked mesh by its content id (assets::AssetId's u64) — what the
// editor's asset browser places. It is deliberately distinct from MeshRef: "which asset" (a stable
// content hash that survives a rename and keys the cook cache, ADR-0024/0025) must never be
// confused with "which loaded mesh" (a dense registry MeshId). A later mesh-loading brick resolves
// a MeshAsset into a MeshRef + a GPU upload (the GpuAssetBridge is textures-only today). Stored as
// a bare u64, not assets::AssetId, so this header keeps its light dependency set and the field
// reflects as a plain UInt64 the inspector shows.
struct MeshAsset {
    std::uint64_t asset = 0; // == assets::AssetId::value; 0 = unset
};

// Shade it with that registry material.
struct MaterialRef {
    MaterialId material = kInvalidMaterialId;
};

// A perspective camera. Position + orientation come from the entity's WorldTransform; this
// component holds only the lens. `active` marks the camera a frame renders through (the scene
// renderer takes the first active one it finds).
struct Camera {
    float fov_y = 0.87266f; // vertical field of view [rad] (~50°, the orbit camera's default)
    float z_near = 0.1f;
    float z_far = 1000.0f;
    bool active = true;
};

// A light arriving from one direction (the sun): infinitely far away, so only direction matters.
// The light shines along the entity's forward axis — its WorldTransform rotation applied to −z,
// the same convention a camera looks down — so aiming a light is rotating its entity, and a
// gizmo that orients cameras orients lights for free. Color is linear RGB; intensity scales it.
struct DirectionalLight {
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
};

// A point light at the entity's WorldTransform translation, falling off with distance and cut to
// zero at `radius` (physical inverse-square inside — the exact falloff is M5.6's business; the
// radius bounds the light's reach so a scene of many lights stays cullable).
struct PointLight {
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
    float radius = 10.0f;
};

// A spot light (m10.2): a point light with a cone. It sits at the entity's WorldTransform
// translation and shines along its forward axis (WorldTransform rotation applied to −z — the same
// "aim it like a camera" convention as DirectionalLight and Camera). Distance falls off to zero at
// `range`; angular falloff runs from full inside the `inner_angle` cone to zero at the
// `outer_angle` cone edge (angles are the half-angle from the axis, in radians). Spot lights are
// the local shadow casters M10 introduces — with lighting shadows on, a spot casts a real shadow
// through its own perspective shadow map (lighting/local_shadows.hpp); with lighting off it does
// not exist (spots ride the shadowed forward path), keeping the M5.6 baseline byte-identical.
struct SpotLight {
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
    float range = 20.0f;
    float inner_angle = 0.4363323f; // 25° — full brightness inside this half-angle cone
    float outer_angle = 0.6108652f; // 35° — falls to zero by here; the shadow map's FOV = 2×this
};

// Register every render component with a world — id + size + reflection TypeInfo in one shot
// (World::register_component is idempotent, so calling this after spawning is harmless; calling
// it FIRST keeps component ids stable across worlds, which serialization will eventually thank
// us for).
inline void register_render_components(ecs::World& world) {
    (void)world.register_component<MeshRef>();
    (void)world.register_component<MeshAsset>();
    (void)world.register_component<MaterialRef>();
    (void)world.register_component<Camera>();
    (void)world.register_component<DirectionalLight>();
    (void)world.register_component<PointLight>();
    (void)world.register_component<SpotLight>();
}

} // namespace rime::render

// Reflection (outside the namespace — the macros open rime::core themselves). Field lists mirror
// the structs above; a mismatch shows up as a wrong offset in the serializer tests.
RIME_REFLECT_BEGIN(rime::render::MeshRef)
RIME_REFLECT_FIELD(mesh)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::MeshAsset)
RIME_REFLECT_FIELD(asset)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::MaterialRef)
RIME_REFLECT_FIELD(material)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::Camera)
RIME_REFLECT_FIELD(fov_y)
RIME_REFLECT_FIELD(z_near)
RIME_REFLECT_FIELD(z_far)
RIME_REFLECT_FIELD(active)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::DirectionalLight)
RIME_REFLECT_FIELD(color_r)
RIME_REFLECT_FIELD(color_g)
RIME_REFLECT_FIELD(color_b)
RIME_REFLECT_FIELD(intensity)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::PointLight)
RIME_REFLECT_FIELD(color_r)
RIME_REFLECT_FIELD(color_g)
RIME_REFLECT_FIELD(color_b)
RIME_REFLECT_FIELD(intensity)
RIME_REFLECT_FIELD(radius)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::render::SpotLight)
RIME_REFLECT_FIELD(color_r)
RIME_REFLECT_FIELD(color_g)
RIME_REFLECT_FIELD(color_b)
RIME_REFLECT_FIELD(intensity)
RIME_REFLECT_FIELD(range)
RIME_REFLECT_FIELD(inner_angle)
RIME_REFLECT_FIELD(outer_angle)
RIME_REFLECT_END()
