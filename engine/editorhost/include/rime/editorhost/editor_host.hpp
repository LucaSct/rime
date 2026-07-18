// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/stream/protocol.hpp"

namespace rime::ecs {
class World;
}

// The **engine-side editor host** — Milestone 9 / m9.1 (ADR-0031). The editor is a *client of a
// live engine* (ADR-0016): this module serves the editor channel against a running `World` over the
// S1 `ProtocolConnection`. It walks the world entirely through **reflection** — the
// `ComponentRegistry`'s `TypeInfo` per component — so a component registered once (`RIME_REFLECT`)
// is snapshot, streamed, and editable with **zero code here** (the "register once ⇒ inspectable at
// M9" bet of ADR-0018 §4, finally cashed). No UI: every behaviour is provable headless
// (tests/editorhost).
//
// Two layers, one reusable and one over the wire:
//   1. Reflection-driven world (de)serialization + edits — the reusable core (m9.2's scene format
//   and
//      m9.7's play snapshot/restore build on this). A component is keyed by its stable
//      **type_hash**, not its registration-order `ComponentId`, so a blob survives a different
//      registration order.
//   2. `EditorHost` — that core spoken over a `ProtocolConnection`: send schema + snapshot, then
//   drain
//      and apply the client's edits at a tick boundary.
namespace rime::editorhost {

// Editor channel message types — values in the reserved [0x0200, 0x02FF] band (protocol.hpp). Cast
// to `stream::MessageType` to send/receive over a `ProtocolConnection`; the reservation's
// forward-compat rule (an old peer drops an unknown type) is what let M6.9 carve this range out
// early. The engine->editor set is even-ish (read); editor->engine (edits) start at 0x0210.
enum class EditorMessage : std::uint16_t {
    Schema = 0x0200,       // engine -> editor: the reflected type registry (layout per type; m9.4)
    Snapshot = 0x0201,     // engine -> editor: the whole world (entities + components)
    SetComponent = 0x0210, // editor -> engine: set a component's bytes on an entity
    Spawn = 0x0211,        // editor -> engine: spawn an empty entity
    Despawn = 0x0212,      // editor -> engine: despawn an entity
    AddComponent = 0x0213, // editor -> engine: add a DEFAULT-constructed component to an entity
    RemoveComponent = 0x0214, // editor -> engine: remove a component from an entity
    RequestSnapshot = 0x0215, // editor -> engine: resend the world (engine replies with Snapshot)
};

// True if `type` (as received by recv_message) is an editor-channel message (the reserved band).
[[nodiscard]] bool is_editor_message(stream::MessageType type) noexcept;

// ── The reusable reflection core (no wire) ──────────────────────────────────────────────

// Serialize every live entity and its **reflected** components into a self-describing blob: each
// component tagged by its `type_hash` + a reflection-serialized payload. Unreflected components
// (tags, structs with no `RIME_REFLECT`) carry no inspectable state and are skipped. Entities are
// named by their raw (index, generation) handle so an edit can address one.
[[nodiscard]] std::vector<std::byte> serialize_world(const ecs::World& world);

// Reconstruct a world from `serialize_world`'s blob into `dst`, which must have the **same
// component types registered** (so `type_hash` resolves to a `ComponentId` and its `TypeInfo` is
// available). Spawns a fresh entity per record; component values come back bit-identical. Returns
// false (logged) on a truncated blob or a `type_hash` the destination does not know. This is the
// machinery m9.7's play snapshot/restore and m9.2's scene load reuse.
[[nodiscard]] bool deserialize_world(ecs::World& dst, std::span<const std::byte> data);

// The schema: the reflected-type dictionary the editor's inspectors are *generated* from (m9.4). It
// is not just component names any more — it is every reflected type reachable from a registered
// component (the components **and** the nested value types they contain: Transform, Vec3, Quat,
// Entity…), each with its full field layout: per field a name, a primitive kind, and — for a nested
// struct — the `type_hash` of the type to recurse into. A flag marks which entries are top-level
// components (the "add component" menu). Keyed by `type_hash` throughout, so the editor decodes a
// snapshot's opaque component blob into typed, editable fields (and re-encodes an edit) with no
// per-type code. See docs/design/editor-inspectors.md.
[[nodiscard]] std::vector<std::byte> serialize_schema(const ecs::World& world);

// Apply one component edit: deserialize `blob` (a reflection-serialized component of type
// `type_hash`) onto entity `e`, adding the component if the entity lacks it, and stamp it changed.
// Returns false if `e` is not alive, the `type_hash` is unknown/unreflected, or the blob is
// malformed. Call at a tick boundary (the ECS structural-change rule); `EditorHost::poll_one` does.
[[nodiscard]] bool apply_set_component(ecs::World& world,
                                       ecs::Entity e,
                                       std::uint64_t type_hash,
                                       std::span<const std::byte> blob);

// Add a DEFAULT-constructed component of type `type_hash` to `e` (the honest engine defaults, not a
// zeroed blob) and stamp it changed. No-op returning false if `e` is dead, the type is
// unknown/unreflected, or the entity already has it. Call at a tick boundary. The editor's "add
// component" action; shared by the channel host and the viewport host so both stay identical.
[[nodiscard]] bool add_default_component(ecs::World& world, ecs::Entity e, std::uint64_t type_hash);

// Remove the component of type `type_hash` from `e`. Returns false if the type is unknown or the
// entity did not have it. Call at a tick boundary. The editor's "remove component" action.
[[nodiscard]] bool remove_component(ecs::World& world, ecs::Entity e, std::uint64_t type_hash);

// ── The host over the wire ──────────────────────────────────────────────────────────────

// Serves the editor channel against a live `World` over one `ProtocolConnection` (one editor client
// v1). Not thread-safe: drive it from the app's tick thread so edits land at a tick boundary.
class EditorHost {
public:
    explicit EditorHost(stream::ProtocolConnection conn) noexcept;

    // Send the schema then a full world snapshot — call once, right after the connection handshake.
    [[nodiscard]] bool send_hello(const ecs::World& world);

    // Block for the next client message and apply it to `world` (set-component / spawn / despawn)
    // at this call site (= the caller's tick boundary). Returns false when the connection closes or
    // the peer says Bye — the caller's drain loop then stops. A non-edit or malformed message is
    // ignored (returns true, still connected). The app runs this on its editor-serving thread, like
    // samples/04-remote-view's input drain.
    [[nodiscard]] bool poll_one(ecs::World& world);

    [[nodiscard]] bool is_open() const noexcept { return conn_.is_open(); }

    [[nodiscard]] stream::ProtocolConnection& connection() noexcept { return conn_; }

private:
    stream::ProtocolConnection conn_;
};

} // namespace rime::editorhost
