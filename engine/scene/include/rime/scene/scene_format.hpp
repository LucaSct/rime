// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rime::ecs {
class World;
}

// The `.rscene` scene format (M9 / m9.2, ADR-0031) — a world written to a human-diffable text file
// and read back. It is REFLECTION-DRIVEN and generic: a component registered once (RIME_REFLECT)
// saves and loads with zero code here, keyed by its stable `type_hash` so the file survives a
// different component-registration order (the same bet as the m9.1 editor host, now on disk). The
// C++ writer/reader is the *reference* implementation of the format; the Rust editor reuses it via
// files, never by reimplementing the byte layout.
//
// Why text, not the m9.1 binary snapshot: a scene is authored and version-controlled. Text diffs
// cleanly in git, a human can read and hand-edit it, and a merge is a merge. The trade — larger,
// slower to parse — does not matter for authoring-time scene files (a cooked binary scene is a
// future seam, noted in docs/design/scene-format.md, not v1).
//
// Format at a glance (full grammar in the design note):
//
//     rime_scene 1
//     entity 0 {
//       rime::ecs::LocalTransform 0x2a3b... {
//         value { translation { x 0 y 1.5 z 0 } rotation { x 0 y 0 z 0 w 1 } scale { x 1 y 1 z 1 }
//         }
//       }
//       rime::render::Camera 0x9c1d... { fov_y 0.87266 z_near 0.1 z_far 1000 active true }
//     }
//     entity 1 { rime::ecs::Parent 0x77e0... { value @0 } }   # value @0 = "parented to entity 0"
//
// Whitespace is insignificant (indentation is cosmetic); `#` starts a comment to end of line. Each
// component carries its `type_hash` so a schema drift is a clean, explicit error, never a misread.
// An entity-reference field (a `Parent`'s target) is written as a scene-LOCAL id (`@0`) and
// remapped to the freshly-spawned entity on load — so the file never bakes in volatile runtime
// handles.
namespace rime::scene {

// The current on-disk format version, emitted on the `rime_scene` header line. Bumped only if the
// text grammar changes incompatibly (component schema drift is handled per-component by type_hash,
// not by this number).
inline constexpr int kSceneFormatVersion = 1;

// How to load. The default is STRICT — every component type the file names must be registered —
// because that is what a game wants: a scene it authored, loaded by a build that is missing one of
// its own modules, is a bug and should say so at the door.
//
// A TOOL wants the other answer (m14.1). An editor is expected to open scenes authored by builds
// that had modules it does not, and refusing the whole file over one unknown record makes it unable
// to open the very content the engine ships — which is exactly what happened: the editor could not
// open the block because it had never registered `blockkit::SlabRole`.
struct LoadOptions {
    // Skip a component whose type this world has never heard of, and count it, instead of failing
    // the load. SCHEMA DRIFT IS STILL FATAL either way, and the difference is the whole point: an
    // unknown type means "this build does not have that module", which is survivable; a drifted
    // type means "this build HAS that module and the file is stale", which is data you would be
    // silently discarding.
    //
    // WHAT A CALLER OWES: anything that may SAVE the world afterwards must check
    // `skipped_components` and refuse, or offer to, because writing back a world that dropped what
    // it could not read is how an editor quietly deletes the parts of a scene it did not
    // understand. Tolerance without that check is worse than the error message it replaced.
    bool allow_unknown_components = false;
};

// Outcome of a load. On failure `ok` is false and `error` explains why (a bad header, a
// schema-drifted or — under strict options — unknown component type, an unknown field, a malformed
// value, a dangling entity reference); the world may have been PARTIALLY populated, so load into a
// world you are willing to discard on failure. On success `entities`/`components` report what was
// created.
struct LoadReport {
    bool ok = false;
    std::string error;
    std::size_t entities = 0;
    std::size_t components = 0;

    // Components dropped because this world has no such type (only reachable with
    // `LoadOptions::allow_unknown_components`). Zero on a strict load, which is why it is safe for
    // every existing caller to ignore it.
    std::size_t skipped_components = 0;
    // The distinct type names behind that count, in first-seen order — so a report can say WHICH
    // module is missing rather than only how many records went missing. Bounded: a file naming
    // thousands of unknown types is a corrupt file, not a diagnostic worth printing in full.
    std::vector<std::string> unknown_types;
};

// The cap on `LoadReport::unknown_types`. The count keeps counting past it.
inline constexpr std::size_t kMaxReportedUnknownTypes = 16;

// Serialize every live entity and its *reflected* components to `.rscene` text. Entities are
// numbered 0..N-1 in a stable iteration order and any entity-reference field is emitted as that
// local id (or `null` for kNullEntity / a reference outside the saved set). Unreflected components
// carry no inspectable state and are skipped — a documented property, not a silent data loss (a
// component with nothing reflected has nothing to author).
// `excluded_derived`, when non-null, receives the number of DERIVED components the writer refused
// to serialize (see `ecs::kDerivedComponent`). A counter rather than silence, because an exclusion
// that quietly stops working is indistinguishable from one that had nothing to exclude — and the
// failure it prevents (a scene carrying this session's registry indices) is invisible until the
// next run draws the wrong thing.
[[nodiscard]] std::string save_scene_to_string(const ecs::World& world,
                                               std::size_t* excluded_derived = nullptr);

// Parse `.rscene` text into `world`. By default every component type named in the file must be
// registered in `world` with a matching `type_hash` — register your component set first, and
// `worldkit::register_engine_components` is the one list that names the engine's own (a game adds
// its content module's on top). Spawns one fresh entity per record and remaps entity references to
// them. WorldTransform is NOT written by a load (it is derived — run ecs::propagate_transforms
// afterward). Pass `LoadOptions` to let unknown types be skipped and counted instead.
[[nodiscard]] LoadReport
load_scene_from_string(ecs::World& world, std::string_view text, const LoadOptions& options = {});

// File conveniences. `save_scene_file` writes the whole buffer; `load_scene_file` reads the whole
// file then parses it. A missing/unreadable file is a clean `false` / `LoadReport{ok=false}`.
[[nodiscard]] bool save_scene_file(const ecs::World& world, const std::filesystem::path& path);
[[nodiscard]] LoadReport load_scene_file(ecs::World& world,
                                         const std::filesystem::path& path,
                                         const LoadOptions& options = {});

} // namespace rime::scene
