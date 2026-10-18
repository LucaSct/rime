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
