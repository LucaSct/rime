// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `rime-engine` — the ENGINE's editor host (m15.2, ADR-0038).
//
// It registers the engine's own component profile and nothing else. It used to also register
// `blockkit`, the vision demo's content module, which made one game's types a permanent part of the
// engine binary and meant a second game could not see its own components in the inspector without
// editing engine C++.
//
// A game builds its own host from the same library and adds its module on top — see
// `samples/99-the-block/block_host_main.cpp`, which is nine lines. This binary opening a game's
// scene still works: the load skips component types it does not know and reports the count (m14.1),
// so the difference between the two hosts is visible rather than silent.

#include "rime/app/editor_host_app.hpp"
#include "rime/worldkit/profile.hpp"

int main(int argc, char** argv) {
    return rime::app::run_editor_host(
        argc,
        argv,
        [](rime::ecs::World& world) { (void)rime::worldkit::register_engine_components(world); },
        "rime-engine");
}
