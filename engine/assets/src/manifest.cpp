// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/manifest.hpp"

#include <array>
#include <charconv>

#include "rime/core/diagnostics/log.hpp"

namespace rime::assets {
namespace {

// Map a manifest's lowercase kind name to the enum. Only the kinds actually cooked so far are
// known; the table grows one entry per brick that adds a cooked kind. An unknown name is a hard
// parse error rather than a silent guess.
[[nodiscard]] std::optional<AssetKind> kind_from_string(std::string_view name) noexcept {
    if (name == "mesh") {
        return AssetKind::Mesh;
    }
    if (name == "texture") {
        return AssetKind::Texture;
    }
    if (name == "material") {
        return AssetKind::Material;
    }
    if (name == "skeleton") {
        return AssetKind::Skeleton;
    }
    if (name == "clip") {
        return AssetKind::AnimationClip;
    }
    if (name == "mesh_sdf") {
        return AssetKind::MeshSdf;
    }
    return std::nullopt;
}

// Parse a lowercase hex u64 (the content-hash id), requiring the whole field to be valid hex.
[[nodiscard]] std::optional<std::uint64_t> parse_hex_u64(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Split a line into exactly `N` tab-separated fields. Returns false if the count differs (a
// manifest line with the wrong shape is malformed, not something to pad or truncate).
template <std::size_t N>
[[nodiscard]] bool split_tabs(std::string_view line,
                              std::array<std::string_view, N>& out) noexcept {
    std::size_t field = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        const std::string_view piece = line.substr(
            start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
        if (field >= N) {
            return false; // more fields than expected
        }
        out[field++] = piece;
        if (tab == std::string_view::npos) {
            break;
        }
        start = tab + 1;
    }
    return field == N;
}

} // namespace

std::optional<Manifest> Manifest::parse(std::string_view text) {
    Manifest manifest;

    std::size_t line_start = 0;
    int line_number = 0;
    while (line_start <= text.size()) {
        const std::size_t newline = text.find('\n', line_start);
        const std::size_t line_end = (newline == std::string_view::npos) ? text.size() : newline;
        std::string_view line = text.substr(line_start, line_end - line_start);
        ++line_number;

        // Tolerate Windows line endings: a manifest cooked on Windows carries CRLF.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Skip blank lines and comments (the cooker's `# rime-manifest …` banner rides here).
        const bool blank_or_comment = line.empty() || line.front() == '#';
        if (!blank_or_comment) {
            std::array<std::string_view, 4> fields{};
            if (!split_tabs(line, fields)) {
                RIME_ERROR("manifest: line {} has the wrong number of fields", line_number);
                return std::nullopt;
            }
            const std::optional<AssetKind> kind = kind_from_string(fields[1]);
            const std::optional<std::uint64_t> id = parse_hex_u64(fields[2]);
            if (!kind || !id) {
                RIME_ERROR("manifest: line {} has an unknown kind or malformed id", line_number);
                return std::nullopt;
            }
            manifest.entries_.push_back(
                ManifestEntry{std::string(fields[0]), *kind, AssetId{*id}, std::string(fields[3])});
        }

        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }

    return manifest;
}

const ManifestEntry* Manifest::find_by_source(std::string_view source_path) const noexcept {
    for (const ManifestEntry& entry : entries_) {
        if (entry.source_path == source_path) {
            return &entry;
        }
    }
    return nullptr;
}

const ManifestEntry* Manifest::find_by_id(AssetId id) const noexcept {
    for (const ManifestEntry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace rime::assets
