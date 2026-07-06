// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rime/assets/asset_id.hpp"

// The cook manifest (ADR-0024, decision 2): the plain-text index a cook emits, one line per asset,
// mapping a source path to its kind, content-hash id, and cooked filename. It is *derived data* —
// regenerable from the sources at any time — so it can never lie the way a hand-authored database
// can. The runtime registry, the M9 asset browser, and humans all read it. M6.1 ships only the
// reader; the Rust cooker writes it at M6.2.
//
// Line grammar (tab-separated so paths may contain spaces):
//
//   source-path <TAB> kind <TAB> asset-id-hex <TAB> cooked-file
//
// where `kind` is a lowercase name ("mesh", "texture", …) and `asset-id-hex` is the 16-digit
// lowercase hex of the u64 content hash. Blank lines and lines beginning with '#' (the cooker
// writes a `# rime-manifest v1 …` banner) are ignored, so the format has room for comments and a
// header.
namespace rime::assets {

// One parsed manifest line.
struct ManifestEntry {
    std::string source_path;
    AssetKind kind = AssetKind::Mesh;
    AssetId id;
    std::string cooked_file;
};

// A parsed manifest: its entries in file order, plus lookups by source path and by id.
class Manifest {
public:
    // Parse manifest text. Returns nullopt (after logging the offending line number) if any
    // non-comment line is malformed — a manifest is machine-written, so a bad line means a real
    // bug, not something to paper over.
    [[nodiscard]] static std::optional<Manifest> parse(std::string_view text);

    [[nodiscard]] const std::vector<ManifestEntry>& entries() const noexcept { return entries_; }

    // First entry whose source path matches, or nullptr. (Source paths are unique in a well-formed
    // manifest; "first" only matters for a hand-edited one.)
    [[nodiscard]] const ManifestEntry* find_by_source(std::string_view source_path) const noexcept;

    // First entry with this content id, or nullptr.
    [[nodiscard]] const ManifestEntry* find_by_id(AssetId id) const noexcept;

private:
    std::vector<ManifestEntry> entries_;
};

} // namespace rime::assets
