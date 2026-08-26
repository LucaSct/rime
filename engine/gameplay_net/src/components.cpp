// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay_net/components.hpp"

#include "rime/ecs/world.hpp"

// The registration, in its own translation unit so including components.hpp does not drag
// ecs::World's guts into every consumer that only wants the component definition.
namespace rime::gameplay_net {

void register_gameplay_net_components(ecs::World& world) {
    (void)world.register_component<LastProcessedInput>();
}

} // namespace rime::gameplay_net
