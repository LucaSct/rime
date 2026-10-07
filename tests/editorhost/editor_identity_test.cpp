// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the editor's entity identity and the in-place Play→Stop restore (ADR-0075).
//
// The defect this brick fixes, stated as the tests below assert its absence: Stop used to despawn
// every entity and rebuild the world from a snapshot, so every entity came back under a new handle,
// every `ecs::Parent` was left pointing at a dead one (and saved as `null`), and the editor's undo
// history — which named entities by handle — was dead after one Play. Four claims, each its own
// test case: (1) Stop restores values AND component membership in place, keeping survivors'
// handles, removing newcomers and recreating casualties under fresh handles with their old
// EditorId; (2) a Parent survives Stop — forward, backward and self references, a parent that was
// itself destroyed, and a scene save afterwards; (3) an id or reference the host cannot resolve is
// refused and counted, never guessed at or nulled; (4) a handle that died is still dead after Stop.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "rime/core/reflect.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/editorhost/editor_identity.hpp"
#include "rime/scene/scene_format.hpp"

using namespace rime;

namespace eid {
struct Health {
    float hp = 100.0f;
};

struct Armor {
    float rating = 0.0f;
};

// A reference buried one struct deep — the remap must recurse, not just look at top-level fields.
struct Target {
    ecs::Entity who = ecs::kNullEntity;
    float weight = 0.0f;
};

struct Aim {
    Target target;
};

// Unreflected, the way physics' RigidBodyHandle is: play may add one, and Stop must take it away.
struct Scratch {
    int n = 0;
};
} // namespace eid

RIME_REFLECT_BEGIN(eid::Health)
RIME_REFLECT_FIELD(hp)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(eid::Armor)
RIME_REFLECT_FIELD(rating)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(eid::Target)
RIME_REFLECT_FIELD(who)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(eid::Aim)
RIME_REFLECT_FIELD(target)
RIME_REFLECT_END()

namespace {

void register_all(ecs::World& w) {
    ecs::register_transform_components(w);
    (void)w.register_component<eid::Health>();
    (void)w.register_component<eid::Armor>();
    (void)w.register_component<eid::Aim>();
    (void)w.register_component<eid::Scratch>();
    editorhost::register_editor_components(w);
}

// The live handle holding `id`, by a scan of the components (independent of EditorIds' cache).
ecs::Entity by_id(const ecs::World& w, std::uint64_t id) {
    ecs::Entity found = ecs::kNullEntity;
    for (std::size_t ai = 0; ai < w.archetype_count(); ++ai) {
        const ecs::Archetype& arch = w.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t r = 0; r < chunk.size(); ++r) {
                const ecs::Entity e = chunk.entity_at(r);
                if (editorhost::editor_id_of(w, e) == id) {
                    found = e;
                }
            }
        }
    }
    return found;
}

ecs::Entity parent_of(const ecs::World& w, ecs::Entity e) {
    const ecs::Parent* p = w.get<ecs::Parent>(e);
    return p != nullptr ? p->value : ecs::kNullEntity;
}

// How an Entity field travels on the editor wire: the referent's EditorId in the field's 8 bytes.
ecs::Entity wire_ref(std::uint64_t id) {
    return ecs::Entity{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(id >> 32)};
}

editorhost::EditOutcome send(ecs::World& w,
                             editorhost::EditorIds& ids,
                             editorhost::EditorMessage type,
                             const std::vector<std::byte>& payload) {
    return editorhost::apply_editor_edit(w, ids, type, payload);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("E1 (1): Stop restores values and membership in place — survivors keep their handles, "
          "newcomers go, casualties return under fresh handles with their old EditorId") {
    ecs::World w;
    register_all(w);
    const ecs::Entity a = w.spawn_with(eid::Health{10.0f});
    const ecs::Entity b = w.spawn_with(eid::Health{20.0f}, eid::Armor{5.0f});
    const ecs::Entity c = w.spawn_with(eid::Health{30.0f});
    editorhost::EditorIds ids;
    REQUIRE(ids.assign_missing(w) == 3);
    const std::uint64_t id_a = editorhost::editor_id_of(w, a);
    const std::uint64_t id_b = editorhost::editor_id_of(w, b);
    const std::uint64_t id_c = editorhost::editor_id_of(w, c);
    CHECK((id_a == 1 && id_b == 2 && id_c == 3)); // slot order = spawn order
    const std::uint64_t before = editorhost::world_content_hash(w);
    const auto b_signature = w.signature_of(b).ids();

    editorhost::PlaySession session;
    session.play(w);
    // Play does what a simulation does: change a value, drop a component, bind an unreflected one,
    // destroy an entity, create one.
    w.get<eid::Health>(a)->hp = -1.0f;
    REQUIRE(w.remove_component<eid::Armor>(b));
    (void)w.add_component(b, eid::Scratch{7});
    REQUIRE(w.despawn(c));
    const ecs::Entity newcomer = w.spawn_with(eid::Health{99.0f});
    REQUIRE(session.stop(w));

    // Survivors: the same handles, the pre-play values, and exactly the pre-play component set.
    REQUIRE(w.is_alive(a));
    REQUIRE(w.is_alive(b));
    CHECK(w.get<eid::Health>(a)->hp == 10.0f);
    REQUIRE(w.get<eid::Armor>(b) != nullptr);
    CHECK(w.get<eid::Armor>(b)->rating == 5.0f);
    CHECK_FALSE(w.has<eid::Scratch>(b)); // what play bound is gone
    CHECK(w.signature_of(b).ids() == b_signature);

    // The newcomer is gone; the casualty is back — not under its old handle (that one is dead for
    // good), but under its old NAME.
    CHECK_FALSE(w.is_alive(newcomer));
    CHECK_FALSE(w.is_alive(c));
    const ecs::Entity c2 = by_id(w, id_c);
    REQUIRE(c2 != ecs::kNullEntity);
    CHECK(c2 != c);
    CHECK(w.get<eid::Health>(c2)->hp == 30.0f);
    CHECK(ids.resolve(w, id_c) == c2); // the host's registry finds it by the same id

    CHECK(w.entity_count() == 3);
    CHECK(editorhost::world_content_hash(w) == before);
    const editorhost::StopReport& rep = session.last_stop();
    CHECK(rep.survivors == 2);
    CHECK(rep.removed_newcomers == 1);
    CHECK(rep.respawned == 1);
    CHECK(rep.refs_unresolved == 0);
}

TEST_CASE("E1 (2): a Parent survives Stop — through a respawned parent, forward and self "
          "references, and a scene save afterwards") {
    ecs::World w;
    register_all(w);
    // Iteration order puts the CHILD before its parent, so the child's Parent is a forward
    // reference on restore: it must already resolve when the child is filled.
    const ecs::Entity child = w.spawn_with(ecs::LocalTransform{});
    const ecs::Entity parent = w.spawn_with(ecs::LocalTransform{});
    (void)w.add_component(child, ecs::Parent{parent});
    // A self-reference, one struct deep.
    const ecs::Entity turret = w.spawn_with(eid::Aim{});
    w.get<eid::Aim>(turret)->target = eid::Target{turret, 0.5f};
    editorhost::EditorIds ids;
    (void)ids.assign_missing(w);
    const std::uint64_t id_child = editorhost::editor_id_of(w, child);
    const std::uint64_t id_parent = editorhost::editor_id_of(w, parent);
    const std::uint64_t id_turret = editorhost::editor_id_of(w, turret);
    const std::uint64_t before = editorhost::world_content_hash(w);

    editorhost::PlaySession session;

    SUBCASE("the parent is destroyed during play; the child survives") {
        session.play(w);
        REQUIRE(w.despawn(parent));
        REQUIRE(session.stop(w));
        const ecs::Entity parent2 = by_id(w, id_parent);
        REQUIRE(parent2 != ecs::kNullEntity);
        CHECK(parent2 != parent);
        REQUIRE(w.is_alive(child));
        CHECK(parent_of(w, child) == parent2); // NOT the dead handle, NOT null
        CHECK(session.last_stop().refs_remapped == 1);
    }

    SUBCASE("parent, child and the self-referencing turret are all destroyed during play") {
        session.play(w);
        REQUIRE(w.despawn(child));
        REQUIRE(w.despawn(parent));
        REQUIRE(w.despawn(turret));
        // Fill the freed slots with newcomers, so a lazy restore that rewound a generation or
        // reused a handle would land on the wrong entity rather than fail loudly.
        (void)w.spawn();
        (void)w.spawn();
        REQUIRE(session.stop(w));
        const ecs::Entity child2 = by_id(w, id_child);
        const ecs::Entity parent2 = by_id(w, id_parent);
        const ecs::Entity turret2 = by_id(w, id_turret);
        REQUIRE(child2 != ecs::kNullEntity);
        REQUIRE(parent2 != ecs::kNullEntity);
        REQUIRE(turret2 != ecs::kNullEntity);
        CHECK(parent_of(w, child2) == parent2);                 // the forward reference
        CHECK(w.get<eid::Aim>(turret2)->target.who == turret2); // the self-reference, nested
        CHECK(w.get<eid::Aim>(turret2)->target.weight == 0.5f);
        CHECK(session.last_stop().refs_remapped == 2);
        CHECK(session.last_stop().removed_newcomers == 2);
    }

    // Whichever way play went, the restored world carries the same data, the editor sees the
    // reference by NAME, and a save writes it as a reference rather than `null`.
    CHECK(editorhost::world_content_hash(w) == before);

    const std::vector<std::byte> snap = editorhost::serialize_editor_snapshot(w, ids);
    CHECK(!snap.empty());

    const ecs::Entity child_now = by_id(w, id_child);
    std::uint64_t parent_field_on_wire = 0;
    {
        // What the editor would receive for the child's Parent: the parent's EditorId.
        std::vector<std::byte> blob = core::serialize(*w.get<ecs::Parent>(child_now));
        const ecs::ComponentInfo& info = w.components().info(w.components().id_of<ecs::Parent>());
        REQUIRE(editorhost::rewrite_blob_entity_refs(info, blob, [&](ecs::Entity& ref) {
            ref = wire_ref(editorhost::editor_id_of(w, ref));
            return true;
        }));
        ecs::Parent decoded{};
        REQUIRE(core::deserialize(decoded, blob));
        parent_field_on_wire = static_cast<std::uint64_t>(decoded.value.index) |
                               (static_cast<std::uint64_t>(decoded.value.generation) << 32);
    }
    CHECK(parent_field_on_wire == id_parent);

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "rime_e1_parent_after_stop.rscene";
    const editorhost::SceneSaveOutcome saved =
        editorhost::save_hosted_scene(w, editorhost::HostedScene{}, file.string());
    REQUIRE(saved.ok);
    const std::string text = read_file(file);
    CHECK(text.find("value null") == std::string::npos);
    CHECK(text.find("EditorId") == std::string::npos); // a session component: never written

    ecs::World reloaded;
    register_all(reloaded);
    REQUIRE(scene::load_scene_from_string(reloaded, text).ok);
    int parented = 0;
    reloaded.query<ecs::Parent>().for_each([&](ecs::Entity e, ecs::Parent& p) {
        ++parented;
        CHECK(reloaded.is_alive(p.value));
        CHECK(p.value != e);
    });
    CHECK(parented == 1);
    std::filesystem::remove(file);
}

TEST_CASE("E1 (3): an id or reference the host cannot resolve is refused and counted") {
    ecs::World w;
    register_all(w);
    const ecs::Entity a = w.spawn_with(ecs::LocalTransform{}, eid::Health{1.0f});
    editorhost::EditorIds ids;
    (void)ids.assign_missing(w); // a = 1
    const std::uint64_t health_hash = core::reflect<eid::Health>().type_hash;
    const std::uint64_t parent_hash = core::reflect<ecs::Parent>().type_hash;
    using editorhost::EditorMessage;

    SUBCASE("a command naming an id no live entity holds") {
        editorhost::SetComponentMsg set{.editor_id = 77, .type_hash = health_hash, .blob = {}};
        set.blob = core::serialize(eid::Health{5.0f});
        CHECK_FALSE(send(w, ids, EditorMessage::SetComponent, serialize_set_component(set)).reply);
        CHECK(w.get<eid::Health>(a)->hp == 1.0f);
        const editorhost::EditOutcome d =
            send(w, ids, EditorMessage::Despawn, editorhost::serialize_entity_ref(77));
        CHECK(d.reply);
        CHECK_FALSE(d.ok);
        const editorhost::EditOutcome add =
            send(w,
                 ids,
                 EditorMessage::AddComponent,
                 editorhost::serialize_component_ref({.editor_id = 77, .type_hash = health_hash}));
        CHECK_FALSE(add.ok);
        CHECK(ids.counters().unresolved_ids == 3);
        CHECK(w.entity_count() == 1);
    }

    SUBCASE("a component whose Entity field names an unknown id is refused, not nulled") {
        (void)w.add_component(a, ecs::Parent{ecs::kNullEntity});
        editorhost::SetComponentMsg set{.editor_id = 1, .type_hash = parent_hash, .blob = {}};
        set.blob = core::serialize(ecs::Parent{wire_ref(999)});
        (void)send(w, ids, EditorMessage::SetComponent, serialize_set_component(set));
        CHECK(ids.counters().unresolved_refs == 1);
        CHECK(parent_of(w, a) == ecs::kNullEntity); // unchanged — the edit never applied

        // The same refusal inside a placement: the whole spawn is refused, nothing left behind.
        editorhost::SpawnEntityMsg spawn;
        spawn.components.emplace_back(parent_hash, core::serialize(ecs::Parent{wire_ref(999)}));
        const editorhost::EditOutcome out =
            send(w, ids, EditorMessage::SpawnEntity, serialize_spawn_entity(spawn));
        CHECK(out.reply);
        CHECK_FALSE(out.ok);
        CHECK(ids.counters().unresolved_refs == 2);
        CHECK(w.entity_count() == 1);
    }

    SUBCASE("a respawn under an id this session never issued, or one still live") {
        editorhost::SpawnEntityMsg restore;
        restore.exact = true;
        restore.editor_id = 50; // never issued
        CHECK_FALSE(send(w, ids, EditorMessage::SpawnEntity, serialize_spawn_entity(restore)).ok);
        restore.editor_id = 1; // a's, and a is alive
        CHECK_FALSE(send(w, ids, EditorMessage::SpawnEntity, serialize_spawn_entity(restore)).ok);
        CHECK(ids.counters().refused_adoptions == 2);
        CHECK(w.entity_count() == 1);
    }

    SUBCASE("Stop meets a reference that was dangling before play: kept, logged, counted") {
        const ecs::Entity gone = w.spawn();
        (void)w.add_component(a, ecs::Parent{gone});
        REQUIRE(w.despawn(gone)); // dangling before play even starts
        editorhost::PlaySession session;
        session.play(w);
        REQUIRE(session.stop(w));
        CHECK(session.last_stop().refs_unresolved == 1);
        CHECK(parent_of(w, a) == gone); // restored as it was — never quietly nulled
        // And the editor sees it as dangling, not as "no parent".
        (void)editorhost::serialize_editor_snapshot(w, ids);
        CHECK(ids.counters().unresolved_refs == 1);
    }
}

TEST_CASE("E1 (4): a handle that died before or during play is still dead after Stop") {
    ecs::World w;
    register_all(w);
    const ecs::Entity keep = w.spawn_with(eid::Health{});
    const ecs::Entity doomed = w.spawn_with(eid::Health{});
    editorhost::EditorIds ids;
    (void)ids.assign_missing(w);

    editorhost::PlaySession session;
    session.play(w);
    REQUIRE(w.despawn(doomed));
    const ecs::Entity newcomer = w.spawn(); // LIFO recycling: it takes doomed's slot
    CHECK(newcomer.index == doomed.index);
    REQUIRE(session.stop(w));

    // Generations never rewind: the respawned entity reuses no dead handle, and both handles that
    // died — the casualty's old one and the newcomer's — fail is_alive and resolve to nothing.
    CHECK(w.is_alive(keep));
    CHECK_FALSE(w.is_alive(doomed));
    CHECK_FALSE(w.is_alive(newcomer));
    CHECK(w.get<eid::Health>(doomed) == nullptr);
    const ecs::Entity reborn = ids.resolve(w, 2);
    REQUIRE(reborn != ecs::kNullEntity);
    CHECK(reborn != doomed);
    CHECK(reborn != newcomer);
}

TEST_CASE("E1: despawn nulls inbound references, and the editor's undo restores them exactly") {
    // The despawn inverse the editor records (ADR-0075): an exact SpawnEntity under the old id,
    // then a SetComponent per captured inbound reference — all by EditorId, all through the one
    // dispatcher. Here the "editor" is the test.
    ecs::World w;
    register_all(w);
    const ecs::Entity parent = w.spawn_with(ecs::LocalTransform{}, eid::Health{42.0f});
    const ecs::Entity child = w.spawn_with(ecs::LocalTransform{});
    (void)w.add_component(child, ecs::Parent{parent});
    editorhost::EditorIds ids;
    (void)ids.assign_missing(w); // parent = 1, child = 2
    using editorhost::EditorMessage;

    // Capture what the editor would, from its snapshot: the parent's components, on the wire.
    const std::uint64_t health_hash = core::reflect<eid::Health>().type_hash;
    const std::uint64_t local_hash = core::reflect<ecs::LocalTransform>().type_hash;
    const std::uint64_t parent_hash = core::reflect<ecs::Parent>().type_hash;
    editorhost::SpawnEntityMsg restore;
    restore.editor_id = 1;
    restore.exact = true;
    restore.components.emplace_back(local_hash, core::serialize(ecs::LocalTransform{}));
    restore.components.emplace_back(health_hash, core::serialize(eid::Health{42.0f}));
    editorhost::SetComponentMsg relink{.editor_id = 2, .type_hash = parent_hash, .blob = {}};
    relink.blob = core::serialize(ecs::Parent{wire_ref(1)});

    const editorhost::EditOutcome gone =
        send(w, ids, EditorMessage::Despawn, editorhost::serialize_entity_ref(1));
    REQUIRE(gone.ok);
    CHECK(parent_of(w, child) == ecs::kNullEntity); // nulled, not left dangling
    CHECK(ids.counters().nulled_refs == 1);

    // A spawn in between takes the freed slot — the undo must not care.
    const editorhost::EditOutcome other =
        send(w, ids, EditorMessage::Spawn, editorhost::serialize_entity_ref(0));
    REQUIRE(other.ok);
    CHECK(other.editor_id == 3); // never 1 again: ids are not reused

    REQUIRE(send(w, ids, EditorMessage::SpawnEntity, serialize_spawn_entity(restore)).ok);
    (void)send(w, ids, EditorMessage::SetComponent, serialize_set_component(relink));
    const ecs::Entity parent2 = ids.resolve(w, 1);
    REQUIRE(parent2 != ecs::kNullEntity);
    CHECK(parent2 != parent);
    CHECK(parent_of(w, child) == parent2);
    CHECK(w.get<eid::Health>(parent2)->hp == 42.0f);
    CHECK_FALSE(w.has<ecs::Parent>(parent2)); // exact: nothing it did not have
    CHECK(ids.counters().unresolved_ids == 0);
    CHECK(ids.counters().unresolved_refs == 0);
}

TEST_CASE("E1: a host-saved scene is written in editor order, without ids, and an untouched "
          "load → save is byte-identical") {
    ecs::World w;
    register_all(w);
    // Build a scene through the writer itself so the text is canonical.
    ecs::World author;
    register_all(author);
    const ecs::Entity e0 = author.spawn_with(eid::Health{1.0f});
    const ecs::Entity e1 = author.spawn_with(eid::Health{2.0f}, eid::Armor{3.0f});
    (void)author.add_component(e0, ecs::Parent{e1});
    const std::string canonical = scene::save_scene_to_string(author);

    REQUIRE(scene::load_scene_from_string(w, canonical).ok);
    editorhost::EditorIds ids;
    (void)ids.assign_missing(w);
    CHECK(scene::save_scene_to_string(w, editorhost::entities_in_editor_order(w)) == canonical);

    // Churn the archetype layout the way Play→Stop can (component off and back on): editor order is
    // unmoved, so the file is too — while the storage-order save would renumber.
    const ecs::Entity first = ids.resolve(w, 1);
    const eid::Health saved_hp = *w.get<eid::Health>(first);
    REQUIRE(w.remove_component<eid::Health>(first));
    (void)w.add_component(first, saved_hp);
    CHECK(scene::save_scene_to_string(w, editorhost::entities_in_editor_order(w)) == canonical);
}
