// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `the-block-host` — the vision demo's own editor host (m15.2, ADR-0038).
//
// THIS FILE IS THE POINT OF THE BRICK. A game that wants its own components in the editor's
// inspector builds its own host binary: the engine's profile, then its own module. Nine lines, one
// CMake target, no plugin system, and nothing in `engine/` knows this game exists.
//
// The editor already takes `--engine <path>`, so pointing it here instead of at `rime-engine` is the
// whole integration. `rime-engine` still opens `block.rscene` — the loader skips `blockkit::SlabRole`
// and reports it — which is exactly the degradation m14.1 built and exactly the difference this
// binary removes.

#include "rime/app/editor_host_app.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/worldkit/profile.hpp"

int main(int argc, char** argv) {
    return rime::app::run_editor_host(
        argc,
        argv,
        [](rime::ecs::World& world) {
            (void)rime::worldkit::register_engine_components(world); // the engine's
            rime::blockkit::register_blockkit_components(world);     // this game's
        },
        "the-block-host");
}
