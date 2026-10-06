// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rime/core/reflect/serialize.hpp"
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_refs.hpp"

namespace rime::ecs {
class World;
}

// **Editor identity** (ADR-0075) — the stable name the editor uses for an entity.
//
// THE PROBLEM. An `ecs::Entity` is (slot index, generation). That is the right handle for the
// engine: a despawn bumps the slot's generation, so every stale copy of the handle is detectably
// dead, forever. It is the wrong name for an editor, whose undo history and selection must outlive
// the very operations that kill handles — a despawn the user is about to undo, or a Play→Stop that
// rebuilds whatever the simulation destroyed. Before this brick the editor held raw handles, and
// one Play→Stop (which respawned every entity) silently killed the whole undo history and dropped
// every `Parent` to null.
//
// THE IDEA. Separate *identity* from *storage*. `EditorId` is a plain u64 the editor host stamps on
// every entity the editor can see — on scene load (in file order), on spawn, on place — from a
// counter that only ever goes up, so a value names one entity for the whole session and is never
// reused, not even after a despawn. That is what lets an undone despawn come back under its OLD id
// while getting a FRESH handle: the engine's handle-safety rule (generations never rewind) and the
// editor's need for a stable name stop being in conflict, because they are about different things.
//
// Everything the editor says about an entity — which one a command targets, and the value of every
// Entity-typed field inside a component — travels as an EditorId. The host translates at the
// boundary, through reflection (ecs/entity_refs.hpp), exactly as the scene format translates
// handles to scene-local `@N` ids. An id the host cannot resolve is REFUSED and counted, never
// guessed at and never quietly turned into null.
namespace rime::editorhost {

// The component. Reflected (below) so it rides the same machinery as every other component — the
// play/stop baseline captures and restores it like any value — and marked a session component so
// no `.rscene` ever carries it (the host reassigns ids on load, in file order).
struct EditorId {
    std::uint64_t value = 0;
};

// 0 is never issued: it is the wire's "no entity" (a null reference, a hidden gizmo).
inline constexpr std::uint64_t kNoEditorId = 0;
// What an Entity field that points at a DEAD handle is sent as. Never issued either, so if the
// editor ever echoes it back the host refuses it — a dangling reference stays visibly dangling
// rather than being laundered into a null.
inline constexpr std::uint64_t kDanglingEditorId = ~std::uint64_t{0};

// Register EditorId in `world`. Idempotent. The host calls it before it loads a scene.
void register_editor_components(ecs::World& world);

// The EditorId `e` carries, or kNoEditorId (dead, id-less, or EditorId not registered).
[[nodiscard]] std::uint64_t editor_id_of(const ecs::World& world, ecs::Entity e) noexcept;

// Every live entity, ordered by EditorId ascending, then any id-less entity in iteration order.
// The order the editor's outliner lists and the host saves in: creation order, independent of
// which archetype an entity's current component set put it in.
[[nodiscard]] std::vector<ecs::Entity> entities_in_editor_order(const ecs::World& world);

// Refusals and repairs, counted. Every refusal also logs a named error; the counters are what a
// test (or a status readout) checks, because a refusal that is only logged is easy to miss.
struct IdentityCounters {
    std::uint64_t unresolved_ids = 0;    // a command named an id no live entity holds
    std::uint64_t unresolved_refs = 0;   // an Entity field (in or out) that could not be resolved
    std::uint64_t refused_adoptions = 0; // a respawn asked for an id never issued, or still live
    std::uint64_t nulled_refs = 0;       // references to a despawned entity, nulled by the despawn
};

// The host's id registry: the counter, and a cache from id to the handle that currently holds it.
// The cache is only a cache — every hit is checked against the world (alive, and still carrying
// that id) and a miss rebuilds it from the components — so a Stop that respawns entities, or game
// code that despawns one, can never make it answer with a wrong handle.
class EditorIds {
public:
    // Give every live entity that lacks an EditorId a fresh one, in ascending slot-index order —
    // which on a freshly loaded world IS file order, because the scene loader spawns one entity per
    // record in order into an empty directory. Any id already present (an earlier session's
    // restore, a hand-made world) moves the counter past it so it can never be issued twice.
    // Returns how many were assigned.
    std::size_t assign_missing(ecs::World& world);

    // Stamp a fresh id on `e` (which must be alive and id-less) and return it.
    std::uint64_t assign(ecs::World& world, ecs::Entity e);

    // Can `id` be given back to a respawned entity? Only if this registry issued it (it is below
    // the counter) and no live entity holds it now. Counts and logs a refusal.
    [[nodiscard]] bool can_adopt(ecs::World& world, std::uint64_t id);

    // Stamp a previously issued id back onto `e` — an undone despawn. Call can_adopt first.
    void adopt(ecs::World& world, ecs::Entity e, std::uint64_t id);

    // The live handle holding `id`, or kNullEntity. Not counted: callers that REFUSE on a miss
    // count it themselves, because only they know whether a miss is an error.
    [[nodiscard]] ecs::Entity resolve(const ecs::World& world, std::uint64_t id);

    [[nodiscard]] std::uint64_t next_id() const noexcept { return next_; }

    [[nodiscard]] IdentityCounters& counters() noexcept { return counters_; }

    [[nodiscard]] const IdentityCounters& counters() const noexcept { return counters_; }

private:
    void rebuild_cache(const ecs::World& world);

    std::uint64_t next_ = 1;
    std::unordered_map<std::uint64_t, ecs::Entity> cache_;
    IdentityCounters counters_;
};

// Rewrite every Entity field of one reflection-serialized component blob (`info`'s type) through
// `fn(Entity&) -> bool`, re-serializing the result. The blob is decoded into a scratch object, the
// shared walker (ecs::for_each_entity_ref) visits the fields, and the object is encoded again — so
// the rewrite is reflection-exact and never pattern-matches bytes. Returns false if `fn` refused a
// field or the blob does not decode; `blob` is unchanged then. A type with no Entity field is a
// no-op returning true.
template <class F>
bool rewrite_blob_entity_refs(const ecs::ComponentInfo& info, std::vector<std::byte>& blob, F&& fn);

// ── Wire messages that name an entity (ADR-0075: they carry its EditorId) ──────────────────

// SetComponent (editor -> engine): [editor_id:u64][type_hash:u64][blob_len:u32][blob]. Entity
// fields inside the blob are EditorIds too.
struct SetComponentMsg {
    std::uint64_t editor_id = kNoEditorId;
    std::uint64_t type_hash = 0;
    std::vector<std::byte> blob;
};
[[nodiscard]] std::vector<std::byte> serialize_set_component(const SetComponentMsg& msg);
[[nodiscard]] bool parse_set_component(std::span<const std::byte> payload, SetComponentMsg& out);

// AddComponent / RemoveComponent (editor -> engine): [editor_id:u64][type_hash:u64].
struct ComponentRefMsg {
    std::uint64_t editor_id = kNoEditorId;
    std::uint64_t type_hash = 0;
};
[[nodiscard]] std::vector<std::byte> serialize_component_ref(const ComponentRefMsg& msg);
[[nodiscard]] bool parse_component_ref(std::span<const std::byte> payload, ComponentRefMsg& out);

// Spawn and Despawn (editor -> engine): [editor_id:u64]. For Despawn, the entity to remove. For
// Spawn, kNoEditorId asks for a fresh id; a previously issued id respawns under it (a redo).
[[nodiscard]] std::vector<std::byte> serialize_entity_ref(std::uint64_t editor_id);
[[nodiscard]] bool parse_entity_ref(std::span<const std::byte> payload, std::uint64_t& editor_id);

// SpawnEntity (editor -> engine): [editor_id:u64][exact:u8][comp_count:u16] then per component
// [type_hash:u64][blob_len:u32][blob]. `editor_id` as for Spawn. `exact` = 0 is a placement: the
// entity gets a default LocalTransform first unless the list carries one (m15.3). `exact` = 1 is a
// restore (an undone despawn): the entity gets exactly the listed components and nothing else.
struct SpawnEntityMsg {
    std::uint64_t editor_id = kNoEditorId;
    bool exact = false;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> components;
};
[[nodiscard]] std::vector<std::byte> serialize_spawn_entity(const SpawnEntityMsg& msg);
[[nodiscard]] bool parse_spawn_entity(std::span<const std::byte> payload, SpawnEntityMsg& out);

// EditResult (engine -> editor): [ok:u8][editor_id:u64] — the reply to every Spawn, SpawnEntity,
// Despawn, AddComponent and RemoveComponent, in the order they arrived. The editor commits an
// undo step only on `ok`, and learns a fresh spawn's id from it. Deliberately minimal: the world
// itself still comes back through the snapshot round trip.
struct EditResultMsg {
    bool ok = false;
    std::uint64_t editor_id = kNoEditorId;
};
[[nodiscard]] std::vector<std::byte> serialize_edit_result(const EditResultMsg& msg);
[[nodiscard]] bool parse_edit_result(std::span<const std::byte> payload, EditResultMsg& out);

} // namespace rime::editorhost

namespace rime::ecs {
template <> inline constexpr bool kSessionComponent<editorhost::EditorId> = true;
}

RIME_REFLECT_BEGIN(rime::editorhost::EditorId)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()

// ── Template definitions ───────────────────────────────────────────────────────────────────────

namespace rime::editorhost {

template <class F>
bool rewrite_blob_entity_refs(const ecs::ComponentInfo& info, std::vector<std::byte>& blob, F&& fn) {
    if (info.type_info == nullptr || !ecs::has_entity_refs(*info.type_info)) {
        return true;
    }
    // A scratch instance to decode into. max_align_t storage covers every component the engine
    // registers today; a type that is over-aligned is refused rather than mis-addressed.
    if (info.alignment > alignof(std::max_align_t)) {
        return false;
    }
    std::vector<std::max_align_t> storage((info.size + sizeof(std::max_align_t) - 1) /
                                              sizeof(std::max_align_t) +
                                          1);
    void* object = storage.data();
    info.ops.default_construct(object);
    bool ok = core::deserialize(*info.type_info, object, blob) &&
              ecs::for_each_entity_ref(*info.type_info, static_cast<std::byte*>(object), fn);
    if (ok) {
        blob = core::serialize(*info.type_info, object);
    }
    info.ops.destroy(object, 1);
    return ok;
}

} // namespace rime::editorhost
