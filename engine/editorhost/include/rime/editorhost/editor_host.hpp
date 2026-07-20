// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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
    Schema = 0x0200,     // engine -> editor: the reflected type registry (layout per type; m9.4)
    Snapshot = 0x0201,   // engine -> editor: the whole world (entities + components)
    AssetList = 0x0203,  // engine -> editor: the cook manifest (browsable assets; m9.5)
    PickResult = 0x0204, // engine -> editor: the entity under a picked viewport pixel (m9.6)
    ViewportCamera = 0x0205,  // engine -> editor: the viewport's exact render lens (m9.6 gizmos)
    PlayState = 0x0206,       // engine -> editor: the play/edit phase + tick count (m9.7)
    SetComponent = 0x0210,    // editor -> engine: set a component's bytes on an entity
    Spawn = 0x0211,           // editor -> engine: spawn an empty entity
    Despawn = 0x0212,         // editor -> engine: despawn an entity
    AddComponent = 0x0213,    // editor -> engine: add a DEFAULT-constructed component to an entity
    RemoveComponent = 0x0214, // editor -> engine: remove a component from an entity
    RequestSnapshot = 0x0215, // editor -> engine: resend the world (engine replies with Snapshot)
    SpawnEntity = 0x0216, // editor -> engine: spawn an entity WITH an initial component set (m9.5)
    PickRequest = 0x0217, // editor -> engine: pick the entity at a viewport pixel (m9.6)
    GizmoState = 0x0218,  // editor -> engine: selection + gizmo mode/axis to render (m9.6 gizmos)
    Play = 0x0219,  // editor -> engine: begin (from Edit) or resume (from Paused) the sim (m9.7)
    Pause = 0x021A, // editor -> engine: stop ticking; the viewport keeps rendering (m9.7)
    Step = 0x021B,  // editor -> engine: run exactly one fixed tick, then stay Paused (m9.7)
    Stop = 0x021C,  // editor -> engine: restore the pre-play snapshot; back to Edit (m9.7)
};

// True if `type` (as received by recv_message) is an editor-channel message (the reserved band).
[[nodiscard]] bool is_editor_message(stream::MessageType type) noexcept;

// True if receiving `msg` (editor -> engine) changes what the viewport *renders*, so the frame must
// be re-rendered and re-streamed. This is the classifier the editor host's idle-skip is keyed off
// (m10.0-perf, ADR-0032 §11 — "idle work is a bug"): an idle editor renders nothing until an edit,
// a gizmo change, or a play-state transition arrives. Deliberately NOT frame-affecting: a snapshot
// request (answered with world bytes, not a frame) and a pick request (served by the independent
// 1×1 pick pass, which never touches the streamed frame). Camera moves count — the camera is a
// world entity, so they arrive as `SetComponent`. A total switch (no default) makes any future
// message type a compile error until it is consciously classified.
[[nodiscard]] bool message_affects_frame(EditorMessage msg) noexcept;

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

// A content fingerprint of every reflected component `world` holds — FNV-1a, in the same
// archetype/chunk/row order serialize_world walks, over each entity's (type_hash, blob) pairs but
// deliberately EXCLUDING the entity's own (index, generation). m9.7's play/stop restore despawns
// every entity and respawns fresh ones (deserialize_world always mints new handles, never reuses
// the source ids — see its comment above), and ecs::EntityDirectory unconditionally bumps a slot's
// generation on every despawn (the safety invariant that makes a stale Entity handle detectable) —
// so even a flawless restore never reproduces the exact pre-play (index, generation) pairs. This is
// therefore the identity-independent "did the DATA come back exactly" witness the play/stop restore
// proof wants (tests/app/editor_play_test.cpp), the same FNV-1a-over-flat-serialized-bytes
// discipline as PhysicsWorld::world_hash / DestructionWorld::state_hash.
[[nodiscard]] std::uint64_t world_content_hash(const ecs::World& world);

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

// One cooked asset as the editor's browser sees it (m9.5). A flattened view of a cook-manifest line
// (assets::ManifestEntry), kept as plain fields + views so editorhost need not depend on
// engine/assets — the app fills these from its parsed Manifest. `kind` is the wire value of
// assets::AssetKind; `id` is the AssetId's u64 content hash.
struct AssetListEntry {
    std::uint16_t kind = 0;
    std::uint64_t id = 0;
    std::string_view source_path;
    std::string_view cooked_file;
};

// Serialize the asset list (the `AssetList` message payload) — the manifest the browser lists,
// searches, and places from. Format:
//   [magic 'RAL1':u32][count:u32] then per entry
//   [kind:u16][id:u64][source_len:u16][source...][cooked_len:u16][cooked...]
[[nodiscard]] std::vector<std::byte> serialize_asset_list(std::span<const AssetListEntry> assets);

// Spawn one entity carrying an initial component set — the editor's "place asset" primitive (m9.5).
// The payload is `[comp_count:u16]` then per component `[type_hash:u64][blob_len:u32][blob...]`
// (each a reflection-serialized component, as SetComponent uses). Atomic spawn-with-components
// avoids the editor having to spawn, learn the new handle, then set each component.
// Unknown/unreflected/ malformed components are skipped (the entity still spawns). Returns false
// only on a truncated header. Call at a tick boundary.
[[nodiscard]] bool spawn_entity_from_payload(ecs::World& world, std::span<const std::byte> payload);

// ── The gizmo channel (m9.6 Part B) ─────────────────────────────────────────────────────

// The viewport's authoritative render lens for one frame — the `ViewportCamera` message the engine
// sends alongside every streamed frame. The editor's gizmo math must project/unproject through the
// EXACT matrices the pixels were rendered with (the same "clicks would land beside their pixels"
// argument as the pick pass, scene_picker.cpp), so the engine ships both the clip-from-world and
// its inverse: the editor never has to invert a matrix, and the two can never disagree. ~148 bytes
// per frame — noise next to the LZ4 frame it rides with.
struct ViewportCameraMsg {
    float view_proj[16];               // clip-from-world, column-major (core::Mat4::m layout)
    float inv_view_proj[16];           // world-from-clip: the engine-computed inverse of view_proj
    float eye[3] = {0.0f, 0.0f, 0.0f}; // camera world position (the perspective ray origin)
    std::uint32_t width = 0;           // the render extent the matrices target — pixel-space
    std::uint32_t height = 0;          // coordinates in gizmo math are relative to THIS size
};

// Serialize / parse the `ViewportCamera` payload:
//   [view_proj:16xf32][inv_view_proj:16xf32][eye:3xf32][width:u32][height:u32]  (little-endian)
[[nodiscard]] std::vector<std::byte> serialize_viewport_camera(const ViewportCameraMsg& msg);
[[nodiscard]] bool parse_viewport_camera(std::span<const std::byte> payload,
                                         ViewportCameraMsg& out);

// The editor's gizmo state — the `GizmoState` message: which entity is selected and which gizmo
// (and hovered/active axis) the engine should render over the viewport. ENGINE STATE, not a world
// edit: like PickRequest it is consumed by the viewport host's frame loop, never applied to the
// World. `index == 0xFFFFFFFF` (the PickResult miss sentinel) means "no selection — hide the
// gizmo". Mode/axis are plain u8 codes so the wire stays trivially forward-compatible (an unknown
// mode renders nothing).
struct GizmoStateMsg {
    std::uint32_t index = 0xFFFFFFFFu; // selected entity handle (index, generation)
    std::uint32_t generation = 0;
    std::uint8_t mode = 0; // 0 none/hidden, 1 translate, 2 rotate, 3 scale
    std::uint8_t axis = 0; // 0 none, 1 X, 2 Y, 3 Z — the axis to draw highlighted
};

// Serialize / parse the `GizmoState` payload: [index:u32][generation:u32][mode:u8][axis:u8].
[[nodiscard]] std::vector<std::byte> serialize_gizmo_state(const GizmoStateMsg& msg);
[[nodiscard]] bool parse_gizmo_state(std::span<const std::byte> payload, GizmoStateMsg& out);

// ── Play/edit state machine (m9.7, ADR-0031 §4) ─────────────────────────────────────────

// Edit -> Playing/Paused -> Edit, driven by the Play/Pause/Step/Stop editor messages. `PlaySession`
// (below) owns only a phase, a memory snapshot (serialize_world's bytes — m9.2's reflection
// machinery, to MEMORY not disk), and a tick counter, so it is provable in isolation exactly like
// serialize_world/deserialize_world (tests/editorhost). It is deliberately silent on HOW a tick is
// produced: the caller (editor_host_main.cpp's serve_viewport) decides, each loop iteration,
// whether to advance the sim — phase() == Playing, or a one-shot armed Step — and reports back
// afterward via record_tick(), which keeps this class free of an rime::app or rime::physics
// dependency (module boundaries) while remaining the single source of truth the PlayState status
// message reports from.
//
// THE ENGINEERING RISK this brick exists to resolve (ADR-0031 §4): engine SIDE-TABLES — physics
// bodies, the M8 destruction SoA — must be "reconstructible from components" on restore.
// PlaySession::stop() only touches the ECS (despawn everything, reconstruct from the snapshot); a
// side-table itself is the CALLER's job to rebuild from the now-restored components. The two
// side-tables this ADR names, and what was found/decided at kickoff:
//   * PHYSICS already satisfies the rule BY CONSTRUCTION (physics::PhysicsSync::reconcile, M7.6):
//     it binds every RigidBody+Collider+WorldTransform entity that lacks a body and destroys any
//     body whose entity is gone, so recreating a fresh PhysicsWorld/PhysicsSync and reconciling
//     against the restored world (editor_host_main.cpp does exactly this) rebuilds physics state
//     from nothing but components — no new engine code needed, just wiring the existing seam in.
//   * DESTRUCTION has no such path today, and none was built here: `destruction::DestructionWorld`
//     takes no `ecs::World` anywhere in its API (ADR-0029 §5/§6 — destructible parts are not
//     simulated entities, and destruction bypasses PhysicsSync on purpose, owning its bodies
//     directly), and the editor host never constructs a DestructionWorld — the M9 viewport scenes
//     carry no destructibles. There is therefore no component-level representation to reconstruct
//     FROM yet; inventing one here, with no consumer to exercise it, would be exactly the kind of
//     unasked-for infrastructure the project's "measure, don't guess" discipline warns against.
//     Wiring destructibles into the editor (an ECS-authored destructible-instance component plus a
//     reconcile step over it) is ADR-0029 §6's own named graduation trigger — left for the brick
//     that actually authors destructibles in the editor, not fabricated against no consumer here.
enum class PlayPhase : std::uint8_t {
    Edit = 0,    // the sim schedule is paused; edits apply directly (m9.0 §4)
    Playing = 1, // ticking live, one fixed step per call the caller makes
    Paused = 2,  // a session exists (a snapshot was taken) but ticking is halted; render continues
};

// The `PlayState` payload: the phase plus how many fixed ticks have run since play()/pause() first
// left Edit (reset to 0 by stop()). Sent every viewport frame, like ViewportCamera — a handful of
// bytes — so it is what drives the editor's tick counter and state-coloured viewport border live.
struct PlayStateMsg {
    PlayPhase phase = PlayPhase::Edit;
    std::uint64_t tick_count = 0;
};

// Serialize / parse the `PlayState` payload: [phase:u8][tick_count:u64].
[[nodiscard]] std::vector<std::byte> serialize_play_state(const PlayStateMsg& msg);
[[nodiscard]] bool parse_play_state(std::span<const std::byte> payload, PlayStateMsg& out);

class PlaySession {
public:
    // Begin (from Edit: snapshot `world` first) or resume (from Paused — keeping the ORIGINAL
    // snapshot, so a later Stop still restores the true pre-play state, not the paused mid-play
    // one) the simulation. Idempotent while already Playing.
    void play(const ecs::World& world);

    // Ensure a session exists — snapshotting `world` on a first call from Edit, exactly like
    // play() — but leave the steady-state phase at Paused. Used by the Pause message (Playing ->
    // Paused) and by a Step requested straight from Edit, which must still end up with a snapshot
    // to eventually Stop back to.
    void pause(const ecs::World& world);

    // Restore `world` from the pre-play snapshot: despawn every entity currently in `world`, then
    // reconstruct every entity from the snapshot's component data (never handles —
    // deserialize_world always mints fresh entity ids; see its doc comment). Returns to Edit and
    // clears the snapshot and tick counter. No-op returning false if already Edit — nothing was
    // playing to restore.
    bool stop(ecs::World& world);

    // The caller's single source-of-truth call: exactly once per fixed tick it actually ran (a
    // continuous Play tick or an armed Step), so tick_count() always matches ticks really taken.
    void record_tick() noexcept { ++tick_count_; }

    [[nodiscard]] PlayPhase phase() const noexcept { return phase_; }

    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }

private:
    PlayPhase phase_ = PlayPhase::Edit;
    std::vector<std::byte> snapshot_;
    std::uint64_t tick_count_ = 0;
};

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
