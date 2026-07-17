// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the engine-side editor host (M9 / m9.1, ADR-0031). All GPU-free CPU + loopback socket
// work, so it runs on every CI OS. Three things: (1) a world snapshots and reconstructs
// bit-identically through reflection alone; (2) an edit applies to a component by its stable
// type_hash; (3) the whole thing works over a real ProtocolConnection on the s1.4 local socket —
// the editor's wire — end to end.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/platform/socket.hpp"
#include "rime/stream/protocol.hpp"

using namespace rime;

// Reflected test components (in a named namespace so RIME_REFLECT can name them).
namespace et {
struct Position {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Velocity {
    float dx = 0.0f, dy = 0.0f;
};
} // namespace et

RIME_REFLECT_BEGIN(et::Position)
RIME_REFLECT_FIELD(x)
RIME_REFLECT_FIELD(y)
RIME_REFLECT_FIELD(z)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(et::Velocity)
RIME_REFLECT_FIELD(dx)
RIME_REFLECT_FIELD(dy)
RIME_REFLECT_END()

namespace {
std::string unique_local_path() {
    std::random_device rd;
    return (std::filesystem::temp_directory_path() / ("rime-m91-" + std::to_string(rd()) + ".sock"))
        .string();
}
} // namespace

TEST_CASE("editorhost: a world snapshots and reconstructs bit-identically through reflection") {
    ecs::World src;
    (void)src.spawn_with(et::Position{1.0f, 2.0f, 3.0f});
    (void)src.spawn_with(et::Position{4.0f, 5.0f, 6.0f}, et::Velocity{7.0f, 8.0f});
    (void)src.spawn(); // a bare entity: no components, must still round-trip

    const std::vector<std::byte> blob = editorhost::serialize_world(src);
    CHECK(!blob.empty());

    // A fresh world with the SAME types registered (in a DIFFERENT order — proving type_hash
    // keying, not ComponentId ordering, is what matches components across the two worlds).
    ecs::World dst;
    (void)dst.register_component<et::Velocity>();
    (void)dst.register_component<et::Position>();
    REQUIRE(editorhost::deserialize_world(dst, blob));

    CHECK(dst.entity_count() == 3);

    std::vector<std::array<float, 3>> positions;
    dst.query<et::Position>().for_each(
        [&](ecs::Entity, et::Position& p) { positions.push_back({p.x, p.y, p.z}); });
    CHECK(positions.size() == 2);

    const auto has_pos = [&](float x, float y, float z) {
        for (const auto& p : positions) {
            if (p[0] == x && p[1] == y && p[2] == z) {
                return true;
            }
        }
        return false;
    };
    CHECK(has_pos(1.0f, 2.0f, 3.0f));
    CHECK(has_pos(4.0f, 5.0f, 6.0f));

    int vel_count = 0;
    dst.query<et::Velocity>().for_each([&](ecs::Entity, et::Velocity& v) {
        ++vel_count;
        CHECK(v.dx == 7.0f);
        CHECK(v.dy == 8.0f);
    });
    CHECK(vel_count == 1); // exactly the one entity that had a Velocity
}

TEST_CASE("editorhost: apply_set_component edits by type_hash, adding the component if absent") {
    ecs::World w;
    const ecs::Entity e = w.spawn_with(et::Position{1.0f, 1.0f, 1.0f});
    // Register Velocity too, so the host knows the type (an editor can only set a component the
    // engine has registered — the schema it sends is exactly the registry). An app using the editor
    // registers its editable component types at startup.
    (void)w.register_component<et::Velocity>();
    const std::uint64_t pos_hash = core::reflect<et::Position>().type_hash;
    const std::uint64_t vel_hash = core::reflect<et::Velocity>().type_hash;

    SUBCASE("overwrite an existing component") {
        const std::vector<std::byte> blob = core::serialize(et::Position{9.0f, 8.0f, 7.0f});
        REQUIRE(editorhost::apply_set_component(w, e, pos_hash, blob));
        const et::Position* p = w.get<et::Position>(e);
        REQUIRE(p != nullptr);
        CHECK(p->x == 9.0f);
        CHECK(p->y == 8.0f);
        CHECK(p->z == 7.0f);
    }
    SUBCASE("add a component the entity lacked") {
        CHECK(w.get<et::Velocity>(e) == nullptr);
        const std::vector<std::byte> blob = core::serialize(et::Velocity{2.0f, 3.0f});
        REQUIRE(editorhost::apply_set_component(w, e, vel_hash, blob));
        const et::Velocity* v = w.get<et::Velocity>(e);
        REQUIRE(v != nullptr);
        CHECK(v->dx == 2.0f);
        CHECK(v->dy == 3.0f);
    }
    SUBCASE("an unknown type_hash is refused") {
        const std::vector<std::byte> blob = core::serialize(et::Position{});
        CHECK_FALSE(editorhost::apply_set_component(w, e, 0xDEADBEEFULL, blob));
    }
    SUBCASE("a dead entity is refused") {
        ecs::World w2;
        const ecs::Entity dead = w2.spawn();
        REQUIRE(w2.despawn(dead));
        const std::vector<std::byte> blob = core::serialize(et::Position{});
        CHECK_FALSE(editorhost::apply_set_component(w2, dead, pos_hash, blob));
    }
}

TEST_CASE("editorhost: serves schema + snapshot and applies an edit over the local wire") {
    const std::string path = unique_local_path();
    auto listener = platform::LocalListener::bind(path);
    REQUIRE(listener.has_value());

    ecs::World world;
    const ecs::Entity e = world.spawn_with(et::Position{0.0f, 0.0f, 0.0f});

    struct ClientOutcome {
        bool handshook = false, got_schema = false, got_snapshot = false, sent = false;
    } out;

    std::thread client([&] {
        auto sock = platform::LocalSocket::connect(path);
        if (!sock) {
            return;
        }
        stream::ProtocolConnection conn(std::move(*sock));
        if (!conn.handshake()) {
            return;
        }
        out.handshook = true;

        stream::MessageType type{};
        std::vector<std::byte> payload;
        if (conn.recv_message(type, payload) &&
            type == static_cast<stream::MessageType>(editorhost::EditorMessage::Schema)) {
            out.got_schema = true;
        }
        if (conn.recv_message(type, payload) &&
            type == static_cast<stream::MessageType>(editorhost::EditorMessage::Snapshot)) {
            // The snapshot reconstructs into a mirror world — the client's view of the scene.
            ecs::World mirror;
            (void)mirror.register_component<et::Position>();
            out.got_snapshot =
                editorhost::deserialize_world(mirror, payload) && mirror.entity_count() == 1;
        }

        // Send an edit: set Position of `e` to (5, 6, 7).
        std::vector<std::byte> edit;
        core::ByteWriter w(edit);
        w.u32(e.index);
        w.u32(e.generation);
        w.u64(core::reflect<et::Position>().type_hash);
        const std::vector<std::byte> blob = core::serialize(et::Position{5.0f, 6.0f, 7.0f});
        w.u32(static_cast<std::uint32_t>(blob.size()));
        w.bytes(blob);
        out.sent = conn.send_message(
            static_cast<stream::MessageType>(editorhost::EditorMessage::SetComponent), edit);
        (void)conn.send_bye();
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server_conn(std::move(*accepted));
    REQUIRE(server_conn.handshake());
    editorhost::EditorHost host(std::move(server_conn));
    REQUIRE(host.send_hello(world));

    // Drain + apply the client's edits at this "tick boundary" until it closes (Bye).
    while (host.poll_one(world)) {
    }
    client.join();

    CHECK(out.handshook);
    CHECK(out.got_schema);
    CHECK(out.got_snapshot);
    CHECK(out.sent);

    const et::Position* p = world.get<et::Position>(e);
    REQUIRE(p != nullptr);
    CHECK(p->x == 5.0f); // the edit crossed the wire and applied
    CHECK(p->y == 6.0f);
    CHECK(p->z == 7.0f);
}
