// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::ecs — the data-oriented world. Include this to pull in the whole ECS
// public surface; include the individual headers when you only need one. See ADR-0018 for the
// storage model and docs/design/ecs.md for the design.
//
// As of M4.1: entities (generational ids), the entity directory, and the component-type registry,
// all behind the World front door. Archetype storage, queries, and parallel systems land in
// M4.2–M4.5.

#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"
#include "rime/ecs/world.hpp"
