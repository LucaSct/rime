// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::physics (M7, ADR-0026): the rigid-body core's public seam. Include this
// for everything, or the finer-grained headers under physics/ for smaller dependencies.
//   body.hpp       — BodyId, MotionType, BodyDesc, BodyState
//   shape.hpp      — ShapeType, ShapeDesc, mass properties
//   world.hpp      — PhysicsWorld (create/destroy bodies, step, broadphase, narrowphase)
//   contact.hpp    — ContactPoint, Manifold (the narrowphase output, the M7.4 solver input)
//   components.hpp — the RigidBody/Collider ECS components (bound to bodies by the M7.6 sync
//   system)
//   sync.hpp       — PhysicsSync, the ECS↔PhysicsWorld bridge (bind / write-back / unbind) that
//   runs the fixed tick and drives ADR-0018 §4 change detection (M7.6)
#include "rime/physics/body.hpp"
#include "rime/physics/components.hpp"
#include "rime/physics/contact.hpp"
#include "rime/physics/shape.hpp"
#include "rime/physics/sync.hpp"
#include "rime/physics/world.hpp"
