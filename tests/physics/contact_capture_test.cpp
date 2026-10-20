// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <fstream>
#include <thread>

#include "../../samples/contact_bench/capture.hpp"
#include "support/temp_path.hpp"

TEST_CASE("contact recorder serializes concurrent named worlds without losing rows") {
    constexpr unsigned kWorlds = 4, kSteps = 12;
    const auto path = rime::test::temp_path("contacts-stream.txt");
    std::array<int, kWorlds> identities{};
    {
        contact_capture::Recorder writer(path.string().c_str(), 2);
        std::array<std::thread, kWorlds> threads;
        for (unsigned i = 0; i < kWorlds; ++i) {
            threads[i] = std::thread([&, i] {
                const std::string label = "physics.test" + std::to_string(i) + ".step";
                rime::physics::ShapeDesc shape;
                shape.type = rime::physics::ShapeType::Sphere;
                rime::physics::Manifold expected;
                const auto q = rime::core::quat_identity();
                for (unsigned step = 0; step < kSteps; ++step) {
                    contact_capture::Step snapshot(writer, &identities[i], label, 1.0f / 60);
                    contact_capture::record(snapshot,
                                            {1, 0},
                                            {2, 0},
                                            shape,
                                            {},
                                            q,
                                            {},
                                            shape,
                                            {4, 0, 0},
                                            q,
                                            {},
                                            false,
                                            nullptr,
                                            nullptr,
                                            false,
                                            expected);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
    }
    contact_workload::Workload loaded;
    std::ifstream file(path);
    std::string error;
    REQUIRE(contact_workload::load(file, loaded, error));
    CHECK(loaded.ledger.steps == kWorlds * kSteps);
    CHECK(loaded.ledger.selected_steps == kWorlds * kSteps / 2);
    CHECK(loaded.ledger.queries == kWorlds * kSteps);
    CHECK(loaded.ledger.selected_queries == kWorlds * kSteps / 2);
    CHECK(loaded.ledger.drops == 0);
    CHECK(loaded.pairs.size() == kWorlds * kSteps / 2);
    for (const auto& p : loaded.pairs) {
        CHECK(p.step % 2 == 0);
        rime::physics::Manifold m;
        CHECK_FALSE(contact_workload::exact(loaded, p, m));
    }
}

TEST_CASE("contact recorder counts parallel ranges within one build") {
    const auto path = rime::test::temp_path("contacts-parallel-build.txt");
    int identity = 0;
    {
        contact_capture::Recorder writer(path.string().c_str(), 1);
        contact_capture::Step snapshot(writer, &identity, "physics.parallel.step", 1.0f / 60);
        std::array<std::thread, 4> threads;
        for (unsigned worker = 0; worker < threads.size(); ++worker) {
            threads[worker] = std::thread([&, worker] {
                const rime::physics::ShapeDesc shape;
                const rime::physics::Manifold expected;
                const auto q = rime::core::quat_identity();
                for (unsigned i = 0; i < 100; ++i) {
                    contact_capture::record(snapshot,
                                            {0, 0},
                                            {1 + worker * 100 + i, 0},
                                            shape,
                                            {},
                                            q,
                                            {},
                                            shape,
                                            {4, 0, 0},
                                            q,
                                            {},
                                            false,
                                            nullptr,
                                            nullptr,
                                            false,
                                            expected);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
    }
    contact_workload::Workload loaded;
    std::ifstream file(path);
    std::string error;
    REQUIRE(contact_workload::load(file, loaded, error));
    CHECK(loaded.ledger.steps == 1);
    CHECK(loaded.ledger.queries == 400);
    CHECK(loaded.ledger.selected_queries == 400);
    CHECK(loaded.ledger.drops == 0);
    CHECK(loaded.pairs.size() == 400);
}
