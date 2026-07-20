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
#include <span>
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

// A nested reflected value + a component that contains it — the m9.4 schema must describe BOTH
// (Spin as a component, Inner as a nested-only type) and mark the Struct field's link.
struct Inner {
    float a = 0.0f;
    std::int32_t b = 0;
    bool flag = false;
};

struct Spin {
    Inner inner;
    float rate = 1.0f;
};

// An authoring asset reference (mirrors render::MeshAsset): a single u64 content id, so a placement
// test can assert "the placed entity carries the expected AssetId".
struct AssetRef {
    std::uint64_t asset = 0;
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

RIME_REFLECT_BEGIN(et::Inner)
RIME_REFLECT_FIELD(a)
RIME_REFLECT_FIELD(b)
RIME_REFLECT_FIELD(flag)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(et::Spin)
RIME_REFLECT_FIELD(inner)
RIME_REFLECT_FIELD(rate)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(et::AssetRef)
RIME_REFLECT_FIELD(asset)
RIME_REFLECT_END()

namespace {
std::string unique_local_path() {
    std::random_device rd;
    return (std::filesystem::temp_directory_path() / ("rime-m91-" + std::to_string(rd()) + ".sock"))
        .string();
}

// A parsed schema entry — enough to assert the m9.4 wire layout (RSM2) in a test.
struct ParsedField {
    std::string name;
    std::uint8_t kind = 0;
    std::uint64_t nested_hash = 0;
};

struct ParsedType {
    std::uint64_t hash = 0;
    std::string name;
    bool is_component = false;
    std::vector<ParsedField> fields;
};

// Parse a serialize_schema (RSM2) blob back into structs, mirroring the Rust decoder — so the C++
// test guards the exact bytes the editor will read.
std::vector<ParsedType> parse_schema(std::span<const std::byte> data) {
    core::ByteReader r(data);
    std::vector<ParsedType> types;
    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    REQUIRE(r.u32(magic));
    REQUIRE(magic == 0x52534D32u); // 'RSM2'
    REQUIRE(r.u32(count));
    const auto read_name = [&r]() {
        std::uint16_t len = 0;
        REQUIRE(r.u16(len));
        std::span<const std::byte> bytes;
        REQUIRE(r.bytes(bytes, len));
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        ParsedType t;
        REQUIRE(r.u64(t.hash));
        t.name = read_name();
        std::uint8_t is_comp = 0;
        REQUIRE(r.u8(is_comp));
        t.is_component = is_comp != 0;
        std::uint16_t field_count = 0;
        REQUIRE(r.u16(field_count));
        for (std::uint16_t f = 0; f < field_count; ++f) {
            ParsedField pf;
            pf.name = read_name();
            REQUIRE(r.u8(pf.kind));
            REQUIRE(r.u64(pf.nested_hash));
            t.fields.push_back(std::move(pf));
        }
        types.push_back(std::move(t));
    }
    return types;
}

const ParsedType* find_type(const std::vector<ParsedType>& types, std::uint64_t hash) {
    for (const ParsedType& t : types) {
        if (t.hash == hash) {
            return &t;
        }
    }
    return nullptr;
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

TEST_CASE("editorhost: schema (RSM2) describes each type's fields, recursing into nested structs") {
    ecs::World world;
    (void)world.register_component<et::Position>();
    (void)world.register_component<et::Velocity>();
    (void)world.register_component<et::Spin>();

    const std::vector<ParsedType> types = parse_schema(editorhost::serialize_schema(world));

    const std::uint64_t pos_h = core::reflect<et::Position>().type_hash;
    const std::uint64_t spin_h = core::reflect<et::Spin>().type_hash;
    const std::uint64_t inner_h = core::reflect<et::Inner>().type_hash;

    // The three registered components AND the nested-only Inner (reached through Spin) are
    // described.
    CHECK(types.size() == 4);
    const ParsedType* pos = find_type(types, pos_h);
    const ParsedType* spin = find_type(types, spin_h);
    const ParsedType* inner = find_type(types, inner_h);
    REQUIRE(pos != nullptr);
    REQUIRE(spin != nullptr);
    REQUIRE(inner != nullptr);

    // is_component: the registered types, yes; Inner (a nested value only), no — so it never shows
    // up in the editor's "add component" menu.
    CHECK(pos->is_component);
    CHECK(spin->is_component);
    CHECK_FALSE(inner->is_component);

    // Position: three Float fields x/y/z, none nested. (FieldType::Float == 5.)
    REQUIRE(pos->fields.size() == 3);
    CHECK(pos->fields[0].name == "x");
    CHECK(pos->fields[0].kind == 5);
    CHECK(pos->fields[0].nested_hash == 0);

    // Spin.inner is a Struct field linking to Inner's hash; Spin.rate is a plain Float.
    REQUIRE(spin->fields.size() == 2);
    CHECK(spin->fields[0].name == "inner");
    CHECK(spin->fields[0].kind == 7); // FieldType::Struct
    CHECK(spin->fields[0].nested_hash == inner_h);
    CHECK(spin->fields[1].name == "rate");
    CHECK(spin->fields[1].kind == 5);
    CHECK(spin->fields[1].nested_hash == 0);

    // Inner's primitives carry their kinds: a:Float(5), b:Int32(1), flag:Bool(0).
    REQUIRE(inner->fields.size() == 3);
    CHECK(inner->fields[0].kind == 5);
    CHECK(inner->fields[1].kind == 1);
    CHECK(inner->fields[2].kind == 0);
}

TEST_CASE("editorhost: add/remove component and request-snapshot over the local wire") {
    const std::string path = unique_local_path();
    auto listener = platform::LocalListener::bind(path);
    REQUIRE(listener.has_value());

    ecs::World world;
    const ecs::Entity e = world.spawn_with(et::Position{1.0f, 2.0f, 3.0f});
    (void)world.register_component<et::Velocity>(); // a known type, initially absent on `e`
    const std::uint64_t vel_h = core::reflect<et::Velocity>().type_hash;

    struct Outcome {
        bool velocity_after_add = false;
        bool velocity_gone_after_remove = false;
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
        stream::MessageType type{};
        std::vector<std::byte> payload;
        (void)conn.recv_message(type, payload); // the hello schema
        (void)conn.recv_message(type, payload); // the hello snapshot

        // An [index][generation][hash] editor→engine message (Add/Remove share this shape).
        const auto entity_msg = [&](std::uint64_t hash) {
            std::vector<std::byte> m;
            core::ByteWriter w(m);
            w.u32(e.index);
            w.u32(e.generation);
            w.u64(hash);
            return m;
        };
        // Ask for a fresh snapshot and count how many entities carry a Velocity in it.
        const auto velocity_count_after_request = [&]() -> int {
            const std::vector<std::byte> empty;
            if (!conn.send_message(
                    static_cast<stream::MessageType>(editorhost::EditorMessage::RequestSnapshot),
                    empty)) {
                return -1;
            }
            if (!conn.recv_message(type, payload) ||
                type != static_cast<stream::MessageType>(editorhost::EditorMessage::Snapshot)) {
                return -1;
            }
            ecs::World mirror;
            (void)mirror.register_component<et::Position>();
            (void)mirror.register_component<et::Velocity>();
            if (!editorhost::deserialize_world(mirror, payload)) {
                return -1;
            }
            int n = 0;
            mirror.query<et::Velocity>().for_each([&](ecs::Entity, et::Velocity&) { ++n; });
            return n;
        };

        (void)conn.send_message(
            static_cast<stream::MessageType>(editorhost::EditorMessage::AddComponent),
            entity_msg(vel_h));
        out.velocity_after_add = velocity_count_after_request() == 1;

        (void)conn.send_message(
            static_cast<stream::MessageType>(editorhost::EditorMessage::RemoveComponent),
            entity_msg(vel_h));
        out.velocity_gone_after_remove = velocity_count_after_request() == 0;

        (void)conn.send_bye();
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server_conn(std::move(*accepted));
    REQUIRE(server_conn.handshake());
    editorhost::EditorHost host(std::move(server_conn));
    REQUIRE(host.send_hello(world));
    while (host.poll_one(world)) {
    }
    client.join();

    CHECK(out.velocity_after_add);         // AddComponent added it; RequestSnapshot showed it
    CHECK(out.velocity_gone_after_remove); // RemoveComponent took it away
    CHECK(world.get<et::Position>(e) != nullptr); // and never touched the entity's other components
}

TEST_CASE("editorhost: a pick on the GPU-free channel answers the nothing sentinel (m9.6)") {
    // The channel host has no renderer, so it cannot hit-test — but the protocol still owes every
    // PickRequest exactly one PickResult, or a client's click would wait forever. The honest
    // answer is the miss sentinel: index 0xFFFFFFFF (ecs::kNullEntity's invalid slot), gen 0.
    const std::string path = unique_local_path();
    auto listener = platform::LocalListener::bind(path);
    REQUIRE(listener.has_value());

    ecs::World world;
    (void)world.spawn_with(et::Position{});

    struct Outcome {
        bool got_result = false;
        std::uint32_t index = 0;
        std::uint32_t generation = 1;
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
        stream::MessageType type{};
        std::vector<std::byte> payload;
        (void)conn.recv_message(type, payload); // the hello schema
        (void)conn.recv_message(type, payload); // the hello snapshot

        std::vector<std::byte> req; // [x:i32][y:i32] — any pixel; there is nothing to hit
        core::ByteWriter w(req);
        w.u32(120);
        w.u32(64);
        (void)conn.send_message(
            static_cast<stream::MessageType>(editorhost::EditorMessage::PickRequest), req);
        if (conn.recv_message(type, payload) &&
            type == static_cast<stream::MessageType>(editorhost::EditorMessage::PickResult)) {
            core::ByteReader r(payload);
            out.got_result = r.u32(out.index) && r.u32(out.generation);
        }
        (void)conn.send_bye();
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server_conn(std::move(*accepted));
    REQUIRE(server_conn.handshake());
    editorhost::EditorHost host(std::move(server_conn));
    REQUIRE(host.send_hello(world));
    while (host.poll_one(world)) {
    }
    client.join();

    CHECK(out.got_result);
    CHECK(out.index == 0xFFFFFFFFu); // "nothing" — no viewport, nothing pickable
    CHECK(out.generation == 0);
}

TEST_CASE("editorhost: serialize_asset_list round-trips a manifest to the browser wire (RAL1)") {
    const std::array<editorhost::AssetListEntry, 2> assets{{
        {static_cast<std::uint16_t>(1), 0xABCDEF01u, "meshes/barrel.gltf", "barrel.rmesh"},
        {static_cast<std::uint16_t>(2), 0x1234u, "textures/rust.png", "rust.rtex"},
    }};
    const std::vector<std::byte> blob = editorhost::serialize_asset_list(assets);

    // Parse it back (mirroring the Rust AssetList decoder) and check every field survives.
    core::ByteReader r(blob);
    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    REQUIRE(r.u32(magic));
    CHECK(magic == 0x52414C31u); // 'RAL1'
    REQUIRE(r.u32(count));
    REQUIRE(count == 2);
    const auto read_name = [&r]() {
        std::uint16_t len = 0;
        REQUIRE(r.u16(len));
        std::span<const std::byte> bytes;
        REQUIRE(r.bytes(bytes, len));
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    std::uint16_t kind = 0;
    std::uint64_t id = 0;
    REQUIRE(r.u16(kind));
    REQUIRE(r.u64(id));
    CHECK(kind == 1);
    CHECK(id == 0xABCDEF01u);
    CHECK(read_name() == "meshes/barrel.gltf");
    CHECK(read_name() == "barrel.rmesh");
    REQUIRE(r.u16(kind));
    REQUIRE(r.u64(id));
    CHECK(kind == 2);
    CHECK(read_name() == "textures/rust.png");
    CHECK(read_name() == "rust.rtex");
}

TEST_CASE("editorhost: spawn_entity_from_payload places an entity with the expected components") {
    ecs::World world;
    (void)world.register_component<et::AssetRef>();
    (void)world.register_component<et::Position>();

    // Build the SpawnEntity payload the browser's "place" sends: an AssetRef carrying a content id
    // plus a Position — [comp_count:u16] then per component [hash:u64][blob_len:u32][blob].
    const std::vector<std::byte> asset_blob = core::serialize(et::AssetRef{0xDEADBEEFCAFEULL});
    const std::vector<std::byte> pos_blob = core::serialize(et::Position{3.0f, 4.0f, 5.0f});
    std::vector<std::byte> payload;
    core::ByteWriter w(payload);
    w.u16(2);
    w.u64(core::reflect<et::AssetRef>().type_hash);
    w.u32(static_cast<std::uint32_t>(asset_blob.size()));
    w.bytes(asset_blob);
    w.u64(core::reflect<et::Position>().type_hash);
    w.u32(static_cast<std::uint32_t>(pos_blob.size()));
    w.bytes(pos_blob);

    REQUIRE(editorhost::spawn_entity_from_payload(world, payload));
    CHECK(world.entity_count() == 1);

    int placed = 0;
    world.query<et::AssetRef, et::Position>().for_each(
        [&](ecs::Entity, et::AssetRef& a, et::Position& p) {
            ++placed;
            CHECK(a.asset == 0xDEADBEEFCAFEULL); // the expected AssetId rode across intact
            CHECK(p.x == 3.0f);
            CHECK(p.z == 5.0f);
        });
    CHECK(placed == 1);
}

TEST_CASE("editorhost: ViewportCamera and GizmoState round-trip their wire payloads (m9.6b)") {
    // The gizmo channel's two payloads, serialized and parsed back bit-exactly. GPU-free — the
    // messages are pure state; their live behaviour (the engine rendering a gizmo, the editor
    // unprojecting through the lens) is proven by tests/render/gizmo_test.cpp and the editor's
    // gizmo-math tests. Floats compare bitwise: the wire carries IEEE bit patterns, so equality is
    // the honest assertion (no epsilon needed for a byte round-trip).
    editorhost::ViewportCameraMsg cam{};
    for (int i = 0; i < 16; ++i) {
        cam.view_proj[i] = static_cast<float>(i) * 0.25f;    // distinct per slot: an element
        cam.inv_view_proj[i] = 8.0f - static_cast<float>(i); // swap/skew shows immediately
    }
    cam.eye[0] = 1.0f;
    cam.eye[1] = -2.0f;
    cam.eye[2] = 3.5f;
    cam.width = 960;
    cam.height = 540;

    const std::vector<std::byte> cam_bytes = editorhost::serialize_viewport_camera(cam);
    CHECK(cam_bytes.size() == 16 * 4 + 16 * 4 + 3 * 4 + 4 + 4); // the documented 148-byte payload
    editorhost::ViewportCameraMsg cam_back{};
    REQUIRE(editorhost::parse_viewport_camera(cam_bytes, cam_back));
    for (int i = 0; i < 16; ++i) {
        CHECK(cam_back.view_proj[i] == cam.view_proj[i]);
        CHECK(cam_back.inv_view_proj[i] == cam.inv_view_proj[i]);
    }
    CHECK(cam_back.eye[2] == 3.5f);
    CHECK(cam_back.width == 960);
    CHECK(cam_back.height == 540);
    // A truncated payload is rejected (the bounds-checked reader), never mis-parsed.
    editorhost::ViewportCameraMsg trunc{};
    CHECK_FALSE(editorhost::parse_viewport_camera(
        std::span<const std::byte>(cam_bytes.data(), cam_bytes.size() - 1), trunc));

    editorhost::GizmoStateMsg gs{};
    gs.index = 42;
    gs.generation = 7;
    gs.mode = 2; // rotate
    gs.axis = 1; // X highlighted
    const std::vector<std::byte> gs_bytes = editorhost::serialize_gizmo_state(gs);
    CHECK(gs_bytes.size() == 4 + 4 + 1 + 1);
    editorhost::GizmoStateMsg gs_back{};
    REQUIRE(editorhost::parse_gizmo_state(gs_bytes, gs_back));
    CHECK(gs_back.index == 42);
    CHECK(gs_back.generation == 7);
    CHECK(gs_back.mode == 2);
    CHECK(gs_back.axis == 1);
    // The default-constructed state is the hidden sentinel (index == u32 max, mode none).
    CHECK(editorhost::GizmoStateMsg{}.index == 0xFFFFFFFFu);
    CHECK(editorhost::GizmoStateMsg{}.mode == 0);
}

TEST_CASE("editorhost: PlayState round-trips its wire payload (m9.7)") {
    editorhost::PlayStateMsg msg{};
    msg.phase = editorhost::PlayPhase::Paused;
    msg.tick_count = 123456789ull;
    const std::vector<std::byte> bytes = editorhost::serialize_play_state(msg);
    CHECK(bytes.size() == 1 + 8); // [phase:u8][tick_count:u64]

    editorhost::PlayStateMsg back{};
    REQUIRE(editorhost::parse_play_state(bytes, back));
    CHECK(back.phase == editorhost::PlayPhase::Paused);
    CHECK(back.tick_count == 123456789ull);

    // A truncated payload is rejected, never mis-parsed.
    editorhost::PlayStateMsg trunc{};
    CHECK_FALSE(editorhost::parse_play_state(
        std::span<const std::byte>(bytes.data(), bytes.size() - 1), trunc));

    // The default state is Edit / tick 0 — what a fresh session reports before any Play.
    CHECK(editorhost::PlayStateMsg{}.phase == editorhost::PlayPhase::Edit);
    CHECK(editorhost::PlayStateMsg{}.tick_count == 0);
}

TEST_CASE("editorhost: PlaySession — play/pause/stop is a pure ECS state machine (m9.7)") {
    // Pure ECS proof, deliberately physics-free (physics + the restore-fidelity/content-hash/
    // accumulator proofs live in tests/app/editor_play_test.cpp, which can link rime::physics;
    // this module cannot — see engine/editorhost/CMakeLists.txt). What belongs here is the
    // state-machine contract PlaySession itself owns: phase transitions, snapshot capture on the
    // Edit->Playing/Paused edge only, and a bit-exact ECS restore.
    ecs::World w;
    const ecs::Entity e = w.spawn_with(et::Position{1.0f, 2.0f, 3.0f});
    editorhost::PlaySession session;
    CHECK(session.phase() == editorhost::PlayPhase::Edit);

    SUBCASE("play then stop restores the entity's pre-play component values") {
        session.play(w);
        CHECK(session.phase() == editorhost::PlayPhase::Playing);

        // A live "edit" while playing (m9.0 §4 allows edits during Play; they must not survive
        // Stop) — mutate Position directly, exactly what a SetComponent apply would do.
        w.get<et::Position>(e)->x = 99.0f;
        (void)w.spawn(); // and a structural edit: a bare extra entity
        CHECK(w.entity_count() == 2);

        REQUIRE(session.stop(w));
        CHECK(session.phase() == editorhost::PlayPhase::Edit);
        CHECK(session.tick_count() == 0);
        CHECK(w.entity_count() == 1); // the extra spawn did not survive

        // The original entity's data came back — a DIFFERENT handle (deserialize_world always
        // mints fresh ids), so re-find it by its restored Position rather than by `e`.
        bool found = false;
        w.query<et::Position>().for_each([&](ecs::Entity, et::Position& p) {
            found = true;
            CHECK(p.x == 1.0f); // NOT 99 — the mid-play edit was discarded
            CHECK(p.y == 2.0f);
            CHECK(p.z == 3.0f);
        });
        CHECK(found);
    }

    SUBCASE(
        "resume after pause keeps the ORIGINAL pre-play snapshot, not the paused mid-play state") {
        session.play(w);
        w.get<et::Position>(e)->x = 42.0f; // moved while Playing
        session.pause(w);                  // Playing -> Paused: must NOT re-snapshot here
        CHECK(session.phase() == editorhost::PlayPhase::Paused);
        session.play(w); // resume: Paused -> Playing, still the ORIGINAL snapshot
        CHECK(session.phase() == editorhost::PlayPhase::Playing);

        REQUIRE(session.stop(w));
        w.query<et::Position>().for_each(
            [&](ecs::Entity, et::Position& p) { CHECK(p.x == 1.0f); }); // the pre-play value
    }

    SUBCASE("a Step straight from Edit still snapshots, so Stop has something to restore") {
        session.pause(w); // mirrors editor_host_main.cpp's Step handler: Edit -> Paused directly
        CHECK(session.phase() == editorhost::PlayPhase::Paused);
        w.get<et::Position>(e)->x = 7.0f;
        REQUIRE(session.stop(w));
        w.query<et::Position>().for_each([&](ecs::Entity, et::Position& p) { CHECK(p.x == 1.0f); });
    }

    SUBCASE("stop is a no-op from Edit — nothing was playing") {
        CHECK_FALSE(session.stop(w));
        CHECK(session.phase() == editorhost::PlayPhase::Edit);
        CHECK(w.entity_count() == 1); // untouched
    }

    SUBCASE("record_tick accumulates and resets with the session") {
        session.play(w);
        session.record_tick();
        session.record_tick();
        CHECK(session.tick_count() == 2);
        REQUIRE(session.stop(w));
        CHECK(session.tick_count() == 0);
    }
}

TEST_CASE("editorhost: world_content_hash ignores entity identity but not component data (m9.7)") {
    // The property world_content_hash exists for: two worlds holding the SAME component data but
    // DIFFERENT entity handles (exactly what a despawn-everything-then-respawn restore produces,
    // per ecs::EntityDirectory's LIFO-recycling + unconditional generation bump) still hash equal;
    // any actual data difference still hashes different.
    ecs::World a;
    (void)a.spawn(); // burn index 0 so `b`'s matching entity lands on a DIFFERENT index/generation
    const ecs::Entity ea = a.spawn_with(et::Position{1.0f, 2.0f, 3.0f});
    (void)ea;

    ecs::World b;
    (void)b.spawn_with(et::Position{1.0f, 2.0f, 3.0f}); // same data, index 0 (no entity burned)

    CHECK(editorhost::world_content_hash(a) == editorhost::world_content_hash(b));

    ecs::World c;
    (void)c.spawn_with(et::Position{1.0f, 2.0f, 3.5f}); // one field differs
    CHECK(editorhost::world_content_hash(a) != editorhost::world_content_hash(c));
}
