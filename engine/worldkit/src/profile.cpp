// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/worldkit/profile.hpp"

#include "rime/destruction/components.hpp"
#include "rime/destruction_net/components.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/components.hpp"
#include "rime/gameplay_net/components.hpp"
#include "rime/physics/components.hpp"
#include "rime/render/components.hpp"

namespace rime::worldkit {

std::size_t register_engine_components(ecs::World& world) {
    // THE LIST. One screen, one order, one place a reviewer looks. Adding an engine module that
    // defines components means adding a line here, and the module's own tests will not tell you
    // that you forgot — the failures in the header's note are what forgetting looks like.
    ecs::register_transform_components(world);

    // Two that are NOT covered by any module's own register_*_components, because they are derived
    // state rather than authored data and so are deliberately unreflected (ecs/reflect.hpp):
    //
    //   WorldTransform  — composed from the LocalTransform chain by propagate_transforms. It never
    //                     rides a scene file or a snapshot, but a renderer, a picker and a gizmo
    //                     pass all query it live, so the type has to exist in the world.
    //   RenderTransform — the previous/current pair snapshot interpolation blends between (m11.6a).
    //
    // Registering them here is what stops every consumer rediscovering that it needs them. The
    // editor host and 99-the-block had each worked it out separately, one line at a time.
    (void)world.register_component<ecs::WorldTransform>();
    (void)world.register_component<ecs::RenderTransform>();

    physics::register_physics_components(world);
    render::register_render_components(world);
    destruction::register_destruction_components(world);
    destruction_net::register_destruction_net_components(world);
    gameplay::register_gameplay_components(world);
    gameplay_net::register_gameplay_net_components(world);

    return world.registered_component_count();
}

} // namespace rime::worldkit
