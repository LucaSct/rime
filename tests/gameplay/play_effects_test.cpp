// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <vector>

#include "play_effects.hpp"

using namespace rime;

namespace {

destruction::DestructionEvent
event(destruction::DestructionEventKind kind, std::uint32_t instance, std::uint32_t part) {
    destruction::DestructionEvent e;
    e.kind = kind;
    e.instance = {instance, 0};
    e.part = part;
    e.body = {part, 0};
    e.world_bounds = {{-0.2f, 0.0f, -0.2f}, {0.2f, 0.4f, 0.2f}};
    e.magnitude = 0.0f;
    return e;
}

} // namespace

TEST_CASE(
    "play effects emit visible zero-impulse collapse feedback and dedupe same-tick delivery") {
    block_play::PlayEffects fx({0.0f, 1.0f, 0.0f});
    constexpr float dt = 1.0f / 60.0f;
    const auto died = event(destruction::DestructionEventKind::PartDied, 1, 2);
    const auto detached = event(destruction::DestructionEventKind::IslandDetached, 1, 3);
    const std::array first{died, detached};
    REQUIRE(fx.tick(10, first, dt));
    CHECK(fx.field().count() == 16);
    CHECK(fx.stats().emitted_bursts == 2);
    CHECK(fx.light().has_value());
    bool dust = false, smoke = false;
    for (const auto& particle : fx.field().particles()) {
        dust |= particle.family == vfx::Family::ImpactDust;
        smoke |= particle.family == vfx::Family::LingeringSmoke;
        CHECK(particle.age == 0.0f);
    }
    CHECK(dust);
    CHECK(smoke);

    // Delivery fragments may repeat one event but also introduce another part or instance.
    // Their arrivals share the tick's dedup set and do not age the first burst again.
    const std::array second{died,
                            event(destruction::DestructionEventKind::PartDied, 1, 4),
                            event(destruction::DestructionEventKind::PartDied, 2, 2)};
    REQUIRE(fx.tick(10, second, dt));
    CHECK(fx.stats().same_tick_deliveries == 1);
    CHECK(fx.stats().duplicate_events == 1);
    CHECK(fx.stats().emitted_bursts == 4);
    CHECK(fx.field().count() == 32);
    for (const auto& particle : fx.field().particles())
        CHECK(particle.age == 0.0f);
    CHECK_FALSE(fx.tick(9, first, dt));
    CHECK_FALSE(fx.tick(11, first, std::numeric_limits<float>::quiet_NaN()));
    CHECK(fx.stats().rejected_ticks == 2);
    CHECK(fx.field().count() == 32);
}

TEST_CASE("play effects cap both source bursts and the shared particle pool") {
    block_play::PlayEffects fx({0.0f, 0.0f, 0.0f});
    std::vector<destruction::DestructionEvent> many;
    for (std::uint32_t i = 0; i < 140; ++i)
        many.push_back(event(destruction::DestructionEventKind::IslandDetached, 7, i));
    REQUIRE(fx.tick(0, many, 1.0f / 60.0f));
    CHECK(fx.stats().emitted_bursts == block_play::PlayEffects::kBurstsPerTick);
    CHECK(fx.stats().budget_dropped_events == 140 - block_play::PlayEffects::kBurstsPerTick);
    CHECK(fx.field().count() <= block_play::PlayEffects::kParticleCapacity);
    const auto before = fx.stats().duplicate_events;
    REQUIRE(
        fx.tick(0, std::span<const destruction::DestructionEvent>{many.data(), 1}, 1.0f / 60.0f));
    CHECK(fx.stats().duplicate_events == before + 1);

    for (std::uint64_t tick = 1; tick < 8; ++tick) {
        REQUIRE(fx.tick(tick, many, 1.0f / 60.0f));
        CHECK(fx.field().count() <= block_play::PlayEffects::kParticleCapacity);
    }
    CHECK(fx.field().stats().dropped_capacity > 0);
}

TEST_CASE("play effects extinguish scripted light while old particles retire naturally") {
    block_play::PlayEffects fx({1.0f, 2.0f, 3.0f});
    for (std::uint64_t tick = 0; tick < 10; ++tick)
        REQUIRE(fx.tick(tick, {}, 1.0f / 60.0f));
    REQUIRE(fx.light().has_value());
    CHECK(fx.field().count() > 0);
    fx.set_fire_enabled(false);
    REQUIRE(fx.tick(10, {}, 1.0f / 60.0f));
    CHECK_FALSE(fx.light().has_value());
    CHECK(fx.field().count() > 0);
    fx.set_fire_enabled(true);
    REQUIRE(fx.tick(11, {}, 1.0f / 60.0f));
    CHECK(fx.light().has_value());
}
