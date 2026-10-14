// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/render/terrain_pass.hpp"

namespace rime::render {

// One resident tile to draw in SceneRenderer's opaque stage. The pass and this list are borrowed
// through the next synchronous render() graph declaration. The caller keeps tile GPU resources
// alive until every frame that reads them retires.
struct TerrainSceneDraw {
    TerrainTileId tile;
    TerrainLodDraw lod{};
};

} // namespace rime::render
