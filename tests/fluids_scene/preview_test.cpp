// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

#include "rime/core/byte_cursor.hpp"
#include "rime/ecs/query.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/editorhost/thermal_preview.hpp"
#include "rime/fluids_scene/preview.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/worldkit/profile.hpp"
using namespace rime;
using namespace rime::fluids_scene;

namespace {
ThermalCell cell(double temperature = 273.15, double fraction = 0.0) {
    ThermalCell c;
    c.temperature_kelvin = temperature;
    c.liquid_mass_fraction = fraction;
    c.enabled = true;
    return c;
}

struct Graph {
    ecs::Entity hot, cold, link, source;
};

Graph graph(ecs::World& w) {
    worldkit::register_engine_components(w);
    const auto a = w.spawn_with(cell(283.15, 1.0)), b = w.spawn_with(cell());
    return {a,
            b,
            w.spawn_with(ThermalContact{a, b, 100.0, true}),
            w.spawn_with(ThermalSource{b, 1000.0, true})};
}
} // namespace

TEST_CASE(
    "thermal authoring: defaults inert, schema and runtime do not contaminate saved descriptions") {
    ecs::World w;
    worldkit::register_engine_components(w);
    (void)w.spawn_with(ThermalCell{});
    (void)w.spawn_with(ThermalContact{});
    (void)w.spawn_with(ThermalSource{});
    ThermalPreview p;
    REQUIRE(p.tick(w, 1, 0.25));
    CHECK(p.status() == PreviewStatus::Empty);
    CHECK_FALSE(p.field());
    const auto g = graph(w);
    const auto before = scene::save_scene_to_string(w);
    const auto hash = editorhost::world_content_hash(w);
    REQUIRE(p.tick(w, 2, 0.25));
    REQUIRE(p.field());
    CHECK(p.field()->tick() == 1);
    CHECK(p.field()->cells()[1].state().liquid_mass_fraction > 0.0);
    CHECK(scene::save_scene_to_string(w) == before);
    CHECK(editorhost::world_content_hash(w) == hash);
    CHECK(w.get<ThermalCell>(g.cold)->liquid_mass_fraction == 0.0);
    const auto schema = editorhost::serialize_schema(w);
    CHECK_FALSE(schema.empty());
}

TEST_CASE(
    "thermal authoring: paused reconciliation retains state until relevant description changes") {
    ecs::World w;
    const auto g = graph(w);
    ThermalPreview p;
    REQUIRE(p.tick(w, 1, 0.25));
    const auto energy = p.field()->cells()[1].energy_joules();
    const auto revision = p.revision();
    w.add_component<ecs::LocalTransform>(g.hot, {});
    p.reconcile(w);
    CHECK(p.revision() == revision);
    CHECK(p.field()->cells()[1].energy_joules() == energy);
    CHECK(p.field()->tick() == 1);
    w.get<ThermalSource>(g.source)->power_watts = 2000.;
    p.reconcile(w);
    REQUIRE(p.field());
    CHECK(p.revision() > revision);
    CHECK(p.field()->tick() == 0);
    CHECK(p.field()->cells()[1].energy_joules() == 0.0);
    CHECK(p.accepted_host_tick() == 0);
    CHECK(p.tick(w, 1, 0.25) == false); // global host clock still guards duplicates
    REQUIRE(p.tick(w, 2, 0.25));
    CHECK(p.accepted_host_tick() == 2);
    w.get<ThermalSource>(g.source)->enabled = false;
    p.reconcile(w);
    CHECK(p.field()->tick() == 0);
    const auto r = p.revision();
    w.get<ThermalSource>(g.source)->power_watts = std::numeric_limits<double>::quiet_NaN();
    p.reconcile(w);
    CHECK(p.revision() == r);
    p.reset();
    CHECK(p.status() == PreviewStatus::Empty);
    CHECK(p.entities().empty());
    CHECK_FALSE(p.field());
    REQUIRE(p.tick(w, 1, 0.25));
    CHECK(p.field()->tick() == 1);
}

TEST_CASE(
    "thermal authoring: invalid descriptions clear stale values and stable NaN does not rebuild") {
    ecs::World w;
    const auto g = graph(w);
    ThermalPreview p;
    REQUIRE(p.tick(w, 1, 0.25));
    w.get<ThermalCell>(g.hot)->mass_kg = std::numeric_limits<double>::quiet_NaN();
    p.reconcile(w);
    CHECK(p.status() == PreviewStatus::InvalidDescription);
    CHECK(p.problem_entity() == g.hot);
    CHECK_FALSE(p.field());
    CHECK(p.entities().empty());
    const auto revision = p.revision();
    p.reconcile(w);
    CHECK(p.revision() == revision);
    w.get<ThermalCell>(g.hot)->mass_kg = 1.0;
    p.reconcile(w);
    REQUIRE(p.field());
    CHECK(p.field()->tick() == 0);
    w.get<ThermalCell>(g.cold)->enabled = false;
    p.reconcile(w);
    CHECK(p.status() == PreviewStatus::InvalidDescription);
    CHECK(p.problem_entity() == g.link);
    w.get<ThermalCell>(g.cold)->enabled = true;
    w.get<ThermalContact>(g.link)->first = g.cold;
    p.reconcile(w);
    CHECK_FALSE(p.field());
    w.get<ThermalContact>(g.link)->first = g.hot;
    const auto duplicate = w.spawn_with(ThermalContact{g.cold, g.hot, 100., true});
    p.reconcile(w);
    CHECK_FALSE(p.field());
    CHECK(p.problem_entity() == duplicate);
    w.get<ThermalContact>(duplicate)->enabled = false;
    const auto source = w.spawn_with(ThermalSource{g.cold, 1.0, true});
    p.reconcile(w);
    CHECK_FALSE(p.field());
    CHECK(p.problem_entity() == source);
}

TEST_CASE("thermal authoring: stale generations never retarget recycled cells") {
    ecs::World w;
    const auto g = graph(w);
    ThermalPreview p;
    REQUIRE(p.tick(w, 1, 0.25));
    w.despawn(g.cold);
    const auto replacement = w.spawn_with(cell());
    CHECK(replacement != g.cold);
    p.reconcile(w);
    CHECK(p.status() == PreviewStatus::InvalidDescription);
    CHECK_FALSE(p.field());
}

TEST_CASE("thermal authoring: each enabled capacity refuses the entire graph") {
    for (unsigned kind = 0; kind < 3; ++kind) {
        ecs::World w;
        worldkit::register_engine_components(w);
        std::vector<ecs::Entity> nodes;
        for (unsigned i = 0; i < (kind == 0 ? 17u : 16u); ++i)
            nodes.push_back(w.spawn_with(cell()));
        if (kind == 1) {
            unsigned count = 0;
            for (unsigned i = 0; i < 16 && count < 33; ++i)
                for (unsigned j = i + 1; j < 16 && count < 33; ++j, ++count)
                    (void)w.spawn_with(ThermalContact{nodes[i], nodes[j], 1., true});
        }
        if (kind == 2)
            for (unsigned i = 0; i < 17; ++i)
                (void)w.spawn_with(ThermalSource{nodes[0], 1., true});
        ThermalPreview p;
        p.reconcile(w);
        CHECK(p.status() == PreviewStatus::CapacityExceeded);
        CHECK_FALSE(p.field());
        CHECK(p.entities().empty());
        CHECK(p.stats().capacity_refusals == 1);
        p.reconcile(w);
        CHECK(p.stats().capacity_refusals == 1);
    }
}

TEST_CASE("thermal authoring: failed numerical tick retains committed graph and diagnostics") {
    ecs::World w;
    worldkit::register_engine_components(w);
    auto c = cell(313.0, 1.0);
    const auto e = w.spawn_with(c);
    (void)w.spawn_with(ThermalSource{e, 100000.0, true});
    ThermalPreview p;
    p.reconcile(w);
    REQUIRE(p.field());
    const auto energy = p.field()->cells()[0].energy_joules();
    CHECK_FALSE(p.tick(w, 1, 0.25));
    CHECK(p.status() == PreviewStatus::StepRefused);
    CHECK(p.field()->tick() == 0);
    CHECK(p.field()->cells()[0].energy_joules() == energy);
    CHECK(p.last_result().status == fluids::ThermalFieldStatus::SourceRefused);
    CHECK(p.accepted_host_tick() == 0);
    REQUIRE(p.tick(w, 1, 0.0001));
    CHECK(p.field()->tick() == 1);
    CHECK(p.status() == PreviewStatus::Ready);
}

TEST_CASE("thermal authoring: entity references survive scene remap and Play restoration") {
    ecs::World w;
    const auto g = graph(w);
    editorhost::EditorIds ids;
    ids.assign_missing(w);
    editorhost::PlaySession session;
    session.play(w);
    ThermalPreview p;
    REQUIRE(p.tick(w, 1, 0.25));
    const auto id = w.get<editorhost::EditorId>(g.cold)->value;
    w.despawn(g.cold);
    (void)w.spawn_with(cell(280.0, 1.0));
    REQUIRE(session.stop(w));
    p.reset();
    const auto restored = ids.resolve(w, id);
    REQUIRE(restored != ecs::kNullEntity);
    CHECK(w.get<ThermalContact>(g.link)->second == restored);
    REQUIRE(p.tick(w, 1, 0.25));
    CHECK(p.status() == PreviewStatus::Ready);
    const auto text = scene::save_scene_to_string(w);
    ecs::World back;
    worldkit::register_engine_components(back);
    (void)back.spawn_with(ecs::LocalTransform{});
    const auto load = scene::load_scene_from_string(back, text);
    REQUIRE_MESSAGE(load.ok, load.error);
    bool references = false;
    back.query<ThermalContact>().for_each([&](ThermalContact& link) {
        references = back.is_alive(link.first) && back.is_alive(link.second) &&
                     back.get<ThermalCell>(link.second) != nullptr;
    });
    CHECK(references);
    ThermalPreview loaded;
    REQUIRE(loaded.tick(back, 1, 0.25));
    CHECK(loaded.field()->cells().size() == 2);
}

TEST_CASE(
    "thermal authoring: typed editor edits remap references and inverse bytes restore contact") {
    ecs::World w;
    const auto g = graph(w);
    editorhost::EditorIds ids;
    ids.assign_missing(w);
    const auto id = [&](ecs::Entity e) { return w.get<editorhost::EditorId>(e)->value; };
    const auto packed = [](std::uint64_t value) {
        return ecs::Entity{static_cast<std::uint32_t>(value),
                           static_cast<std::uint32_t>(value >> 32)};
    };
    const auto hash = core::reflect<ThermalContact>().type_hash;
    const auto original =
        core::serialize(ThermalContact{packed(id(g.hot)), packed(id(g.cold)), 100.0, true});
    const auto changed =
        core::serialize(ThermalContact{packed(id(g.cold)), packed(id(g.hot)), 200.0, true});
    for (const auto& blob : {changed, original, changed}) {
        const auto payload = editorhost::serialize_set_component({id(g.link), hash, blob});
        (void)editorhost::apply_editor_edit(
            w, ids, editorhost::EditorMessage::SetComponent, payload);
        CHECK(w.get<ThermalContact>(g.link)->first == (blob == original ? g.hot : g.cold));
        CHECK(w.get<ThermalContact>(g.link)->conductance_w_k == (blob == original ? 100.0 : 200.0));
    }
    CHECK(w.get<ThermalContact>(g.link)->first == g.cold);
    CHECK(w.get<ThermalContact>(g.link)->conductance_w_k == 200.0);
}

TEST_CASE("thermal diagnostics: bounded codec is exact-length finite and atomic on failure") {
    editorhost::ThermalPreviewMsg m;
    m.status = 1;
    m.phase = 2;
    m.session_id = 1;
    m.revision = 2;
    m.host_tick = m.accepted_host_tick = m.field_tick = 12;
    m.cells.push_back({7, 273.15, 0.25, 83375.0});
    const auto bytes = editorhost::serialize_thermal_preview(m);
    REQUIRE(bytes.size() == 95);
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "tools/rime-protocol/tests/fixtures/thermal_preview.bin";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    const std::vector<char> expected((std::istreambuf_iterator<char>(file)), {});
    REQUIRE(expected.size() == bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        CHECK(std::to_integer<unsigned char>(bytes[i]) == static_cast<unsigned char>(expected[i]));
    editorhost::ThermalPreviewMsg decoded;
    REQUIRE(editorhost::parse_thermal_preview(bytes, decoded));
    CHECK(decoded.cells[0].energy_joules == 83375.0);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CHECK_FALSE(editorhost::parse_thermal_preview(std::span(bytes).first(n), decoded));
        CHECK(decoded.cells[0].editor_id == 7);
    }
    for (const auto& [at, value] : std::array<std::pair<std::size_t, unsigned>, 7>{
             {{0, 2}, {1, 5}, {2, 3}, {3, 9}, {4, 5}, {5, 7}, {62, 17}}}) {
        auto bad = bytes;
        bad[at] = static_cast<std::byte>(value);
        CHECK_FALSE(editorhost::parse_thermal_preview(bad, decoded));
    }
    auto bad = bytes;
    bad.push_back(std::byte{0});
    CHECK_FALSE(editorhost::parse_thermal_preview(bad, decoded));
    for (std::size_t at : {71, 79, 87}) {
        auto nan = bytes;
        std::vector<std::byte> value;
        core::ByteWriter writer(value);
        writer.f64(std::numeric_limits<double>::quiet_NaN());
        std::copy(value.begin(), value.end(), nan.begin() + static_cast<std::ptrdiff_t>(at));
        CHECK_FALSE(editorhost::parse_thermal_preview(nan, decoded));
    }
    m.cells.push_back(m.cells[0]);
    CHECK(editorhost::serialize_thermal_preview(m).empty());
    CHECK_FALSE(editorhost::message_affects_frame(editorhost::EditorMessage::ThermalPreview));
}
