// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/texture_asset.hpp"

// The RMA1 cooked-container reader (ADR-0024, decision 3). A cooked file is bytes off disk, and the
// engine trusts nothing it reads: cooked data is treated exactly like network data, the discipline
// the S0.4 stream protocol set. Every file begins with a fixed, versioned header
//
//   [ magic "RMA1" : 4 bytes ][ container_version : u16 ][ asset_kind : u16 ]
//   [ type_schema_hash : u64 ][ payload_size : u64 ]
//
// followed by a kind-specific payload of exactly `payload_size` bytes. Everything is little-endian
// and read field-by-field (never a struct memcpy), every length is bounds-checked against what the
// buffer actually holds *before* anything is allocated from it, and any inconsistency is a clean,
// typed `AssetError` — never undefined behaviour. The engine contains no glTF/PNG/STL parser; this
// reader, and the Rust cooker that writes what it reads (M6.2), are the whole asset boundary.
namespace rime::assets {

// The 4-byte magic, written literally so a cooked file shows "RMA1" in a hex dump. (Rime Model
// Asset, container v1.)
inline constexpr std::array<std::byte, 4> kCookedMagic = {std::byte{'R'},
                                                          std::byte{'M'},
                                                          std::byte{'A'},
                                                          std::byte{'1'}};

// The only container version M6.1 understands. Bump for any incompatible change to the *envelope*
// (not the payload — payload evolution is what type_schema_hash and the attribute flags handle).
inline constexpr std::uint16_t kContainerVersion = 1;

// Size of the fixed header above: 4 + 2 + 2 + 8 + 8.
inline constexpr std::size_t kCookedHeaderSize = 24;

// Why a load failed. Every reader path returns one of these instead of throwing or, worse, reading
// past the buffer; the M6.1 negative battery drives one file per case. Ordered roughly outer→inner.
enum class AssetError {
    Truncated,          // ran out of bytes reading a field (a short/clipped file)
    BadMagic,           // first four bytes are not "RMA1"
    UnsupportedVersion, // container_version is not one we read
    WrongKind,          // header's asset_kind is not the kind the caller asked to load
    SchemaMismatch,  // type_schema_hash != the layout this build was compiled against ("re-cook")
    SizeMismatch,    // payload_size, or an inner length, disagrees with the bytes present
    InvalidLayout,   // mesh: attribute flags / stride / counts are internally inconsistent
    IndexOutOfRange, // mesh: an index references a vertex that does not exist
    BadSubmesh,      // mesh: a submesh's [first, first+count) falls outside the index buffer
    InvalidTexture,  // texture: unknown format, or a mip table inconsistent with the base extent
    Io,              // the file could not be opened/read (load-from-path only)
};

// A short human-readable tag for an error (logging, test messages).
[[nodiscard]] std::string_view to_string(AssetError error) noexcept;

// The parsed, validated container header. `payload` (returned separately) is the exact
// payload_size-byte span the kind-specific decoder consumes.
struct CookedHeader {
    std::uint16_t container_version = 0;
    AssetKind kind = AssetKind::Mesh;
    std::uint64_t type_schema_hash = 0;
    std::uint64_t payload_size = 0;
};

// The schema fingerprint the current build expects a cooked *mesh* payload to match. It is the
// reflection type_hash of the v1 vertex layout (see cooked_reader.cpp), so if that layout ever
// changes, previously cooked meshes are rejected with SchemaMismatch instead of being misread. The
// Rust cooker embeds this same value; the M6.2 golden-fixture test is the cross-language drift
// alarm.
[[nodiscard]] std::uint64_t mesh_schema_hash() noexcept;

// Validate the container envelope and split off the payload. On success returns the header and sets
// `out_payload` to the payload bytes; on failure returns nullopt and sets `out_error`. Does not
// look inside the payload (that is the kind-specific decoder's job).
[[nodiscard]] std::optional<CookedHeader> read_header(std::span<const std::byte> file,
                                                      std::span<const std::byte>& out_payload,
                                                      AssetError& out_error) noexcept;

// Decode a mesh payload (the bytes after the header) into a fully validated MeshAsset. Assumes the
// caller has already confirmed the header's kind and schema hash. Every length is checked before an
// allocation is sized from it, and every index is checked against the vertex count.
[[nodiscard]] std::optional<MeshAsset> decode_mesh(std::span<const std::byte> payload,
                                                   AssetError& out_error) noexcept;

// The one-call path: read the header of a whole cooked file, confirm it is a mesh of the expected
// schema, and decode it. If `out_id` is non-null it receives the content-hash identity of the
// payload (the registry uses this to de-duplicate loads without decoding twice).
[[nodiscard]] std::optional<MeshAsset> read_mesh(std::span<const std::byte> file,
                                                 AssetError& out_error,
                                                 AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *texture* payload to match. It is the
// reflection type_hash of the v1 mip-descriptor record (see cooked_reader.cpp) — the repeated unit
// the reader walks to slice the pixel blob. Change that record and previously cooked textures are
// rejected with SchemaMismatch instead of being misread. The Rust cooker embeds this same value; the
// M6.3 texture golden-fixture test is the cross-language drift alarm (mirrors mesh_schema_hash).
[[nodiscard]] std::uint64_t texture_schema_hash() noexcept;

// Decode a texture payload (the bytes after the header) into a fully validated TextureAsset. Assumes
// the caller has confirmed the header's kind and schema hash. The base extent, format, and a full
// mip chain are read and cross-checked: every level's extent must be the base halved to that level,
// every level's byte size must be width*height*4, offsets must tile the blob with no gap or overlap,
// and the blob must be exactly as long as the mip sizes sum to — so a corrupt table can never make a
// later upload read past the pixels.
[[nodiscard]] std::optional<TextureAsset> decode_texture(std::span<const std::byte> payload,
                                                         AssetError& out_error) noexcept;

// The one-call texture path: read a whole cooked file, confirm it is a texture of the expected
// schema, and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<TextureAsset> read_texture(std::span<const std::byte> file,
                                                       AssetError& out_error,
                                                       AssetId* out_id = nullptr) noexcept;

} // namespace rime::assets
