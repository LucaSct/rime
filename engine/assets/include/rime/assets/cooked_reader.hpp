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
#include "rime/assets/clip_asset.hpp"
#include "rime/assets/destructible_asset.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/assets/skeleton_asset.hpp"
#include "rime/assets/terrain_layer_asset.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/assets/virtual_geometry.hpp"

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
    InvalidTexture,  // texture: unknown format, a mip table inconsistent with the base extent, or
                     // an extent past the sanity ceiling (see decode_texture)
    InvalidMaterial, // material: unknown alpha mode, or a non-finite factor
    InvalidSkeleton, // skeleton: joints out of topological order, or a non-finite bind value
    InvalidClip,     // clip: bad channel path/interp, non-monotonic times, or a non-finite value
    InvalidDestructible, // destructible: part/bond/anchor counts inconsistent, index out of range,
                         // face-vertex count outside 3..16, or a non-finite geometry value
    InvalidMeshSdf, // sdf: unknown encoding, a non-finite/non-positive header value, a resolution
                    // outside the sanity ceiling, or a distance sample exceeding max_abs_distance
    InvalidVirtualGeometry, // virtual geometry: malformed versioned page/cluster/group payload
    InvalidHeightfield,     // heightfield: grid dimensions outside [2, ceiling], a non-finite or
                            // non-positive spacing/scale, an unknown triangulation, or a sample
                            // outside the header's own recorded [min_sample, max_sample]
    InvalidTerrainLayer,    // terrain layer: a zero material or texture id, a non-finite or
                            // non-positive uv_scale, or a non-finite or negative height_contrast
    Io,                     // the file could not be opened/read (load-from-path only) — keep LAST:
                            // kAssetErrorCount below is derived from it
};

// How many AssetError values exist — the size of a per-error counter table.
inline constexpr std::size_t kAssetErrorCount = static_cast<std::size_t>(AssetError::Io) + 1;

// A tally of REJECTED loads, by reason. A reader that refuses a file returns a typed error to its
// caller — but a caller that retries, falls back to a placeholder, or streams the next tile moves
// on, and the refusal then exists nowhere. "Every skip/drop path gets a counter" is the house rule
// for exactly that reason (a streaming system that silently drops corrupt tiles reads as a world
// with holes in it, not as a bug). CALLER-OWNED rather than a process-wide static on purpose: the
// engine runs loaders on the job system, and a global mutable tally is the hidden shared state the
// threading guardrail forbids. One owner per loader (an AssetServer, a streaming tile cache, a
// test); aggregate if you need a total.
struct AssetRejectCounters {
    std::uint64_t total = 0;
    std::array<std::uint64_t, kAssetErrorCount> by_error{};

    void record(AssetError error) noexcept {
        ++total;
        ++by_error[static_cast<std::size_t>(error)];
    }

    [[nodiscard]] std::uint64_t count(AssetError error) const noexcept {
        return by_error[static_cast<std::size_t>(error)];
    }
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
// rejected with SchemaMismatch instead of being misread. The Rust cooker embeds this same value;
// the M6.3 texture golden-fixture test is the cross-language drift alarm (mirrors
// mesh_schema_hash).
[[nodiscard]] std::uint64_t texture_schema_hash() noexcept;

// Decode a texture payload (the bytes after the header) into a fully validated TextureAsset.
// Assumes the caller has confirmed the header's kind and schema hash. The base extent, format, and
// a full mip chain are read and cross-checked: every level's extent must be the base halved to that
// level, every level's byte size must be width*height*4, offsets must tile the blob with no gap or
// overlap, and the blob must be exactly as long as the mip sizes sum to — so a corrupt table can
// never make a later upload read past the pixels.
[[nodiscard]] std::optional<TextureAsset> decode_texture(std::span<const std::byte> payload,
                                                         AssetError& out_error) noexcept;

// The one-call texture path: read a whole cooked file, confirm it is a texture of the expected
// schema, and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<TextureAsset> read_texture(std::span<const std::byte> file,
                                                       AssetError& out_error,
                                                       AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *material* payload to match. It is the
// reflection type_hash of the v1 material record (see cooked_reader.cpp) — the fixed set of factor
// and texture-reference fields the reader walks. Change that record and previously cooked materials
// are rejected with SchemaMismatch instead of being misread. The Rust cooker embeds this same
// value; the M6.4 material golden-fixture test is the cross-language drift alarm (mirrors
// mesh_schema_hash).
[[nodiscard]] std::uint64_t material_schema_hash() noexcept;

// Decode a material payload (the bytes after the header) into a fully validated MaterialAsset.
// Assumes the caller has confirmed the header's kind and schema hash. A material is a FIXED-size
// record, so the payload must be exactly the right length (no shorter, no trailing bytes), the
// alpha mode must be a known enum value, and every factor must be finite — a NaN/Inf slipped into
// the bytes is rejected rather than propagated into the shader. Texture-reference AssetIds are read
// as-is (0 = no texture); whether the referenced asset exists is the loader's concern, not the
// decoder's.
[[nodiscard]] std::optional<MaterialAsset> decode_material(std::span<const std::byte> payload,
                                                           AssetError& out_error) noexcept;

// The one-call material path: read a whole cooked file, confirm it is a material of the expected
// schema, and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<MaterialAsset> read_material(std::span<const std::byte> file,
                                                         AssetError& out_error,
                                                         AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *skeleton* payload to match: the
// reflection type_hash of the v1 per-joint record (parent, name hash, inverse-bind matrix,
// bind-pose TRS — see cooked_reader.cpp), the repeated unit the reader walks. Change that record
// and previously cooked skeletons are rejected with SchemaMismatch instead of being misread. The
// Rust cooker embeds this same value; the cross-language fixture test is the drift alarm (mirrors
// mesh_schema_hash).
[[nodiscard]] std::uint64_t skeleton_schema_hash() noexcept;

// Decode a skeleton payload (the bytes after the header) into a fully validated Skeleton. Assumes
// the caller has confirmed the header's kind and schema hash. The joint count sizes a fixed-size
// joint table whose total length must be exactly what remains; every joint's parent must be
// kNoParent or an already-seen (smaller) index, so the topological-order invariant the sampler
// relies on holds by construction; and every bind-pose float must be finite — so a corrupt table
// can neither walk off the payload nor feed a NaN into the palette.
[[nodiscard]] std::optional<Skeleton> decode_skeleton(std::span<const std::byte> payload,
                                                      AssetError& out_error) noexcept;

// The one-call skeleton path: read a whole cooked file, confirm it is a skeleton of the expected
// schema, and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<Skeleton> read_skeleton(std::span<const std::byte> file,
                                                    AssetError& out_error,
                                                    AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *animation-clip* payload to match: the
// reflection type_hash of the v1 channel record (target joint, path, interpolation, key count — see
// cooked_reader.cpp), the repeated unit the reader walks to slice the keyframe blob. Change that
// record and previously cooked clips are rejected with SchemaMismatch instead of being misread. The
// Rust cooker embeds this same value; the cross-language fixture test is the drift alarm.
[[nodiscard]] std::uint64_t clip_schema_hash() noexcept;

// Decode a clip payload (the bytes after the header) into a fully validated Clip. Assumes the
// caller has confirmed the header's kind and schema hash. The dense per-joint table is
// reconstructed from a sparse channel list: every channel's target joint must be in range, its
// path/interpolation a known enum, its times strictly increasing and finite, and its values finite;
// the keyframe blob must be exactly the length the channel table implies — so no length is trusted
// past the bytes present.
[[nodiscard]] std::optional<Clip> decode_clip(std::span<const std::byte> payload,
                                              AssetError& out_error) noexcept;

// The one-call clip path: read a whole cooked file, confirm it is a clip of the expected schema,
// and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<Clip> read_clip(std::span<const std::byte> file,
                                            AssetError& out_error,
                                            AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *destructible* payload to match: the
// reflection type_hash of the v1 per-part record (COM, AABB, volume, and the vertex/face/index
// counts that slice the geometry blobs — see cooked_reader.cpp), the repeated table unit the reader
// walks. Change that record and previously cooked destructibles are rejected with SchemaMismatch
// instead of being misread. The Rust cooker embeds this same value (M8.1); the cross-language
// oracle test — which registers every decoded part into a real PhysicsWorld — is the drift alarm.
[[nodiscard]] std::uint64_t destructible_schema_hash() noexcept;

// Decode a destructible payload (the bytes after the header) into a fully validated
// DestructibleAsset. Assumes the caller has confirmed the header's kind and schema hash. Every
// count is bounds-checked against the bytes present before an allocation is sized from it; every
// face has 3..16 vertices and every index is in range for its part; every bond/anchor references a
// real part; and every geometry float is finite — so a corrupt file can neither walk off the
// payload nor feed a degenerate hull into register_hull (which would reject it anyway — this is the
// earlier, typed gate).
[[nodiscard]] std::optional<DestructibleAsset>
decode_destructible(std::span<const std::byte> payload, AssetError& out_error) noexcept;

// The one-call destructible path: read a whole cooked file, confirm it is a destructible of the
// expected schema, and decode it. `out_id`, if non-null, receives the payload's content-hash
// identity.
[[nodiscard]] std::optional<DestructibleAsset>
read_destructible(std::span<const std::byte> file,
                  AssetError& out_error,
                  AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *mesh SDF* payload to match: the
// reflection type_hash of the v1 fixed header record (local bounds, grid placement, voxel size,
// resolution, encoding, max_abs_distance — see cooked_reader.cpp). Unlike
// Mesh/Texture/Skeleton/Clip (which fingerprint a REPEATED table record) this mirrors Material: the
// header IS the whole *structured* part of the payload — the trailing distances blob is bare f32
// scalars with no per-element layout of its own to protect (exactly like a mesh's raw index array,
// which also needs no fingerprint). Change any header field and previously cooked SDFs are rejected
// with SchemaMismatch instead of being misread. The Rust cooker embeds this same value (M10.4a);
// the cross-language fixture test is the drift alarm.
[[nodiscard]] std::uint64_t sdf_schema_hash() noexcept;

// Decode a mesh-SDF payload (the bytes after the header) into a fully validated MeshSdfAsset.
// Assumes the caller has confirmed the header's kind and schema hash. Every header float is checked
// finite, voxel_size and every resolution axis must be positive (resolution additionally capped by
// a generous sanity ceiling — see cooked_reader.cpp — so a corrupt file cannot size a
// multi-gigabyte allocation), the encoding must be a known value, and the trailing blob must be
// EXACTLY resolution.x*y*z f32 values — no shorter, no trailing bytes. Every sample is checked
// finite and bounded by max_abs_distance (an integrity check the cooker's own max-reduction makes
// exact, not approximate — see docs/design/assets.md).
[[nodiscard]] std::optional<MeshSdfAsset> decode_mesh_sdf(std::span<const std::byte> payload,
                                                          AssetError& out_error) noexcept;

// The one-call mesh-SDF path: read a whole cooked file, confirm it is a MeshSdf of the expected
// schema, and decode it. `out_id`, if non-null, receives the payload's content-hash identity.
[[nodiscard]] std::optional<MeshSdfAsset> read_mesh_sdf(std::span<const std::byte> file,
                                                        AssetError& out_error,
                                                        AssetId* out_id = nullptr) noexcept;

// The schema fingerprint for the virtual-geometry payload (v1 and v2 share it; the payload carries
// its own version, see ADR-0059). Unlike MeshAsset this is a companion payload, so its own
// versioned table layout is kept separate from the stable mesh ABI.
[[nodiscard]] std::uint64_t virtual_geometry_schema_hash() noexcept;

// Decode a versioned virtual-geometry payload. The payload begins with its own u32 payload version
// and contains only CPU-side page/cluster/group metadata and page bytes; GPU addresses and
// residency are runtime concerns. Every count and graph edge is checked before allocation, then the
// completed contract is passed through validate_virtual_geometry().
[[nodiscard]] std::optional<VirtualGeometryAsset>
decode_virtual_geometry(std::span<const std::byte> payload, AssetError& out_error) noexcept;

// The one-call virtual-geometry path: read a whole RMA1 file, confirm it is the companion payload
// with the expected schema, and decode it. `out_id`, if non-null, receives the payload hash.
[[nodiscard]] std::optional<VirtualGeometryAsset>
read_virtual_geometry(std::span<const std::byte> file,
                      AssetError& out_error,
                      AssetId* out_id = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *heightfield* payload to match (M19.1):
// the reflection type_hash of the v1 fixed header record (payload version, grid dimensions,
// spacing, origin, height scale/offset, triangulation, sample range — see cooked_reader.cpp). Like
// the SDF, the header is the whole structured part of the payload; the trailing sample blob is bare
// u16 scalars. The Rust cooker embeds this same value; the cross-language fixture test
// (fixture_test.cpp, terrain.rhf) is the drift alarm.
[[nodiscard]] std::uint64_t heightfield_schema_hash() noexcept;

// The newest heightfield payload version decode_heightfield understands — the FIRST field of the
// payload. The RMA1 container version guards the envelope; this guards the heightfield payload's
// own layout, so a payload from a future cook is refused cleanly by an old build as
// UnsupportedVersion instead of being misread.
//
// v2 (ADR-0063, m19.4) APPENDS the splat-material block after the sample blob: the weight grid's
// own dimensions, a layer count, the four-slot material palette, and four u8 weights per texel.
// **v1 remains valid and still decodes**, to an asset whose `has_splat()` is false — one material,
// shaded as m19.3 shades it. Nothing in the fixed header record moved, which is why
// heightfield_schema_hash() is UNCHANGED: the version field, not the schema hash, is what
// discriminates the two layouts, and that is what the field was added for. Changing the hash would
// have rejected every terrain already cooked, the cross-language fixture included.
inline constexpr std::uint32_t kHeightfieldPayloadVersion = 2;
inline constexpr std::uint32_t kHeightfieldMinPayloadVersion = 1;

// Decode a heightfield payload (the bytes after the header) into a fully validated
// HeightfieldAsset. Assumes the caller has confirmed the header's kind and schema hash. The payload
// version must be kHeightfieldPayloadVersion; every header float must be finite, the spacings and
// height scale positive; the grid must be at least 2x2 samples and within a per-axis sanity ceiling
// (so a corrupt file cannot size a huge allocation); the triangulation a known value; and the
// sample blob EXACTLY columns*rows u16s with every sample inside [min_sample, max_sample].
[[nodiscard]] std::optional<HeightfieldAsset> decode_heightfield(std::span<const std::byte> payload,
                                                                 AssetError& out_error) noexcept;

// The one-call heightfield path: read a whole RMA1 file, confirm it is a Heightfield of the
// expected schema, and decode it. `out_id`, if non-null, receives the payload's content hash. If
// `rejects` is non-null, every refusal — envelope, kind, schema, or payload — is also tallied
// there by reason (see AssetRejectCounters for why a returned error alone is not enough).
[[nodiscard]] std::optional<HeightfieldAsset>
read_heightfield(std::span<const std::byte> file,
                 AssetError& out_error,
                 AssetId* out_id = nullptr,
                 AssetRejectCounters* rejects = nullptr) noexcept;

// The schema fingerprint the current build expects a cooked *terrain layer* payload to match
// (M19.7a, ADR-0066): the reflection type_hash of the v1 record (material id, packed albedo/height
// texture id, world UV scale, height contrast — see cooked_reader.cpp). Like the material this
// record IS the whole payload. The Rust cooker embeds this same value; the cross-language fixture
// test (fixture_test.cpp, terrain_layer.rtl) is the drift alarm.
[[nodiscard]] std::uint64_t terrain_layer_schema_hash() noexcept;

// Decode a terrain-layer payload (the bytes after the header) into a fully validated
// TerrainLayerAsset. Assumes the caller has confirmed the header's kind and schema hash. A FIXED
// 28-byte record: the payload must be exactly that long (short = Truncated, trailing bytes =
// SizeMismatch); both AssetIds must be nonzero; uv_scale must be finite and > 0 on both axes; and
// height_contrast finite and >= 0. Whether the referenced material and texture exist — and are of
// the right kind — is the loader's concern, not the decoder's (exactly as for a material's
// texture ids).
[[nodiscard]] std::optional<TerrainLayerAsset>
decode_terrain_layer(std::span<const std::byte> payload, AssetError& out_error) noexcept;

// The one-call terrain-layer path: read a whole RMA1 file, confirm it is a TerrainLayer of the
// expected schema, and decode it. `out_id`, if non-null, receives the payload's content hash. If
// `rejects` is non-null, every refusal — envelope, kind, schema, or payload — is also tallied there
// by reason, as read_heightfield does.
[[nodiscard]] std::optional<TerrainLayerAsset>
read_terrain_layer(std::span<const std::byte> file,
                   AssetError& out_error,
                   AssetId* out_id = nullptr,
                   AssetRejectCounters* rejects = nullptr) noexcept;

} // namespace rime::assets
