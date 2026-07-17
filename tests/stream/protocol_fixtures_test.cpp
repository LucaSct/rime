// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Cross-language protocol conformance — the C++ half (M9.3). The editor is a Rust client of the
// live C++ engine (ADR-0016), so the two speak the *same wire* implemented twice: engine/stream (+
// editorhost) here, tools/rime-protocol there. This test emits a golden byte vector for each
// message from the real C++ encoders and guards it: a normal run asserts the committed fixture
// still matches (catching a C++-side wire change), and `RIME_WRITE_PROTOCOL_FIXTURES=1` regenerates
// the committed files. The Rust crate's tests/conformance.rs reads the SAME committed files and
// must decode them + re-encode identical bytes — so drift on either side is a red test, not a
// silent field mismatch. GPU-free (pure encoders), so it runs on every CI OS.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/render/components.hpp"
#include "rime/stream/frame_codec.hpp" // FrameEncoder (the real LZ4 frame encoder)
#include "rime/stream/protocol.hpp"

namespace fs = std::filesystem;
using namespace rime;

namespace {

// The 6-byte handshake: [magic:u32][version:u16].
std::vector<std::byte> handshake_bytes() {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(stream::kProtocolMagic);
    w.u16(stream::kProtocolVersion);
    return out;
}

// An InputEvent payload with a value in every field (so a byte-off mirror is caught).
std::vector<std::byte> input_bytes() {
    stream::InputEvent e;
    e.kind = stream::InputEvent::Kind::PointerDown;
    e.code = 1;
    e.x = 100;
    e.y = -50;
    e.scroll_x = 0.5f;
    e.scroll_y = -0.25f;
    e.mods = 3;
    e.client_us = 123456789ULL;
    e.seq = 42;
    std::vector<std::byte> out;
    e.encode(out);
    return out;
}

// A FrameMessage payload (header + a short opaque data tail; the tail stands in for encoded pixels
// — LZ4 pixel decode is the viewport panel's brick, not this one).
std::vector<std::byte> frame_bytes() {
    stream::FrameMessage f;
    f.sequence = 7;
    f.capture_us = 1000;
    f.readback_us = 2000;
    f.encode_us = 3000;
    f.wire_us = 4000;
    f.last_input_seq = 42;
    f.last_input_client_us = 123456789ULL;
    f.codec = stream::Codec::LZ4;
    f.desc.extent = rhi::Extent2D{4, 2};
    f.desc.format = rhi::Format::RGBA8Unorm;
    f.data = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    std::vector<std::byte> out;
    f.encode(out);
    return out;
}

// A small, fixed world for the editor schema/snapshot fixtures: a camera entity and a two-component
// mesh entity (MeshRef + MaterialRef). Deterministic — fixed registration + spawn order, hashes
// folded from field names — so the bytes are identical across runs and platforms. Kept to the
// render components on purpose, so this cross-language fixture (and the M9.3 protocol crate) does
// not depend on any later brick's component set — the protocol conformance is about bytes, not
// which types exist.
void build_world(ecs::World& w) {
    render::register_render_components(w);
    (void)w.spawn_with(render::Camera{0.9f, 0.1f, 500.0f, true});
    (void)w.spawn_with(render::MeshRef{7}, render::MaterialRef{3});
}

std::vector<std::byte> schema_bytes() {
    ecs::World w;
    build_world(w);
    return editorhost::serialize_schema(w);
}

std::vector<std::byte> snapshot_bytes() {
    ecs::World w;
    build_world(w);
    return editorhost::serialize_world(w);
}

// A SetComponent edit payload (editor -> engine): [index][generation][hash][blob_len][blob].
std::vector<std::byte> set_component_bytes() {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(3);
    w.u32(1);
    w.u64(core::reflect<render::Camera>().type_hash);
    const std::vector<std::byte> blob = core::serialize(render::Camera{1.2f, 0.2f, 800.0f, false});
    w.u32(static_cast<std::uint32_t>(blob.size()));
    w.bytes(blob);
    return out;
}

// A known 8x8 RGBA gradient — the pixels the editor viewport must recover after an LZ4 round trip.
std::vector<std::byte> lz4_pixels_raw() {
    constexpr std::uint32_t w = 8;
    constexpr std::uint32_t h = 8;
    std::vector<std::byte> px(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            px[i + 0] = static_cast<std::byte>(x * 32); // R ramps across
            px[i + 1] = static_cast<std::byte>(y * 32); // G ramps down
            px[i + 2] = static_cast<std::byte>(128);    // B constant
            px[i + 3] = static_cast<std::byte>(255);    // A opaque
        }
    }
    return px;
}

// A FrameMessage whose data is the gradient compressed by the REAL engine LZ4 encoder (liblz4's
// LZ4_compress_default block) — the editor viewport's lossless local codec. The Rust crate must
// LZ4-decompress it back to lz4_pixels_raw() exactly, the cross-language proof for pixel frames.
std::vector<std::byte> frame_lz4_bytes() {
    const std::vector<std::byte> raw = lz4_pixels_raw();
    stream::ImageDesc desc;
    desc.extent = rhi::Extent2D{8, 8};
    desc.format = rhi::Format::RGBA8Unorm;
    stream::FrameEncoder enc;
    std::vector<std::byte> compressed;
    (void)enc.encode(stream::Codec::LZ4, desc, raw, compressed);
    stream::FrameMessage f;
    f.sequence = 1;
    f.codec = stream::Codec::LZ4;
    f.desc = desc;
    f.data = compressed;
    std::vector<std::byte> out;
    f.encode(out);
    return out;
}

bool write_file(const fs::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::optional<std::vector<std::byte>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) {
        std::memcpy(out.data(), raw.data(), raw.size());
    }
    return out;
}

struct Fixture {
    const char* name;
    std::vector<std::byte> bytes;
};

std::vector<Fixture> all_fixtures() {
    return {
        {"handshake.bin", handshake_bytes()},
        {"input_event.bin", input_bytes()},
        {"frame_message.bin", frame_bytes()},
        {"schema.bin", schema_bytes()},
        {"snapshot.bin", snapshot_bytes()},
        {"set_component.bin", set_component_bytes()},
        {"frame_lz4.bin", frame_lz4_bytes()},
        {"frame_lz4_pixels.bin", lz4_pixels_raw()},
    };
}

} // namespace

TEST_CASE("protocol fixtures: committed goldens match the C++ encoders") {
    const fs::path dir = RIME_PROTOCOL_FIXTURE_DIR;
    // Bootstrap: `RIME_WRITE_PROTOCOL_FIXTURES=1 ctest -R protocol_fixtures` (re)writes the
    // committed goldens after a deliberate wire change — run once, then commit and regenerate the
    // Rust side too.
    const bool writing = std::getenv("RIME_WRITE_PROTOCOL_FIXTURES") != nullptr;
    if (writing) {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
    for (const Fixture& f : all_fixtures()) {
        const fs::path path = dir / f.name;
        if (writing) {
            REQUIRE(write_file(path, f.bytes));
            MESSAGE("wrote " << path.string() << " (" << f.bytes.size() << " bytes)");
        } else {
            const std::optional<std::vector<std::byte>> golden = read_file(path);
            REQUIRE_MESSAGE(golden.has_value(),
                            "missing fixture (bootstrap it): " << path.string());
            CHECK_MESSAGE(*golden == f.bytes,
                          "C++ encoder drifted from committed golden: " << f.name);
        }
    }
}
