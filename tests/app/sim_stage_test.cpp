// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The ordered sim stage (ADR-0032 §8, landed by ADR-0033 amendment A5): Application's single
// replacing per-tick hook generalized into PreSim / PostSim / Publish around the two steps the loop
// owns. The proofs are about ORDER — that the canonical tick order of
// docs/design/simulation-tick.md is what actually runs — and about the old hook keeping its exact
// semantics, because every existing caller (the physics playground, the editor host) was written
// against them.

#include <doctest/doctest.h>

#include <string>

#include "rime/app/application.hpp"
#include "rime/ecs/schedule.hpp"

using namespace rime::app;

namespace {

AppConfig headless() {
    AppConfig config;
    config.gpu = false;
    config.worker_threads = 1; // deterministic, and this suite is about ordering, not parallelism
    return config;
}

} // namespace

TEST_CASE("the stages run in canonical tick order, bracketing the schedule") {
    Application app(headless());
    std::string trace;

    app.add_sim_stage(Application::SimStage::PreSim,
                      [&](rime::ecs::World&, double) { trace += "pre "; });
    app.schedule().add("system",
                       rime::ecs::SystemAccess{},
                       [&](rime::ecs::World&, rime::core::JobSystem&, rime::ecs::CommandBuffer&) {
                           trace += "system ";
                       });
    app.add_sim_stage(Application::SimStage::PostSim,
                      [&](rime::ecs::World&, double) { trace += "post "; });
    app.add_sim_stage(Application::SimStage::Publish,
                      [&](rime::ecs::World&, double) { trace += "publish"; });

    app.step(1.0 / 60.0 + 1e-9); // exactly one tick

    REQUIRE(app.tick_count() == 1);
    // This string IS the contract: net polls in `pre`, the sim runs, net publishes in `publish`.
    CHECK(trace == "pre system post publish");
}

TEST_CASE("steps within one stage run in registration order") {
    Application app(headless());
    std::string trace;
    for (const char* label : {"a", "b", "c"}) {
        app.add_sim_stage(Application::SimStage::PreSim,
                          [&trace, label](rime::ecs::World&, double) { trace += label; });
    }

    app.step(1.0 / 60.0 + 1e-9);

    CHECK(trace == "abc");
}

TEST_CASE("on_fixed_tick keeps its replacing semantics and its PostSim position") {
    Application app(headless());
    std::string trace;

    app.add_sim_stage(Application::SimStage::PreSim,
                      [&](rime::ecs::World&, double) { trace += "pre "; });
    app.on_fixed_tick([&](rime::ecs::World&, double) { trace += "FIRST "; });
    app.add_sim_stage(Application::SimStage::Publish,
                      [&](rime::ecs::World&, double) { trace += "publish"; });

    // Setting it again REPLACES, never appends — the documented behaviour of the old single slot,
    // which several samples rely on to re-wire their tick without doubling it up.
    app.on_fixed_tick([&](rime::ecs::World&, double) { trace += "second "; });

    app.step(1.0 / 60.0 + 1e-9);

    CHECK(trace == "pre second publish");
    CHECK(trace.find("FIRST") == std::string::npos);
}

TEST_CASE("the legacy hook holds its original position among later PostSim steps") {
    Application app(headless());
    std::string trace;

    app.on_fixed_tick([&](rime::ecs::World&, double) { trace += "legacy "; });
    app.add_sim_stage(Application::SimStage::PostSim,
                      [&](rime::ecs::World&, double) { trace += "after"; });
    // Replacing the legacy hook must not move it behind the step registered after it — replacement
    // is in place, which is the whole reason the index is remembered rather than re-appended.
    app.on_fixed_tick([&](rime::ecs::World&, double) { trace += "legacy2 "; });

    app.step(1.0 / 60.0 + 1e-9);

    CHECK(trace == "legacy2 after");
}

TEST_CASE("an app that registers no stages still ticks (the zero-cost path)") {
    Application app(headless());
    app.step(1.0 / 60.0 + 1e-9);
    CHECK(app.tick_count() == 1);
}
