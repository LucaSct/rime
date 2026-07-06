// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the cook-manifest reader (M6.1). A well-formed manifest parses into entries with the
// right fields; lookups by source path and id work; CRLF line endings, blank lines, and comment
// banners are tolerated; and a machine-written manifest with a malformed line is rejected outright
// (a bad line means a real bug in the cooker, not something to paper over).

#include <doctest/doctest.h>

#include <string_view>

#include "rime/assets/manifest.hpp"

using namespace rime::assets;

TEST_CASE("parses a well-formed manifest with a banner and blank lines") {
    const std::string_view text = "# rime-manifest v1 cooker=0.1.0\n"
                                  "\n"
                                  "meshes/barrel.gltf\tmesh\t0123456789abcdef\tbarrel.rmesh\n"
                                  "meshes/crate.gltf\tmesh\tfeedfacecafebeef\tcrate.rmesh\n";
    const std::optional<Manifest> manifest = Manifest::parse(text);
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->entries().size() == 2);

    const ManifestEntry& e0 = manifest->entries()[0];
    CHECK(e0.source_path == "meshes/barrel.gltf");
    CHECK(e0.kind == AssetKind::Mesh);
    CHECK(e0.id == AssetId{0x0123456789abcdefull});
    CHECK(e0.cooked_file == "barrel.rmesh");
    CHECK(manifest->entries()[1].id == AssetId{0xfeedfacecafebeefull});
}

TEST_CASE("lookups by source path and by id") {
    const std::optional<Manifest> manifest =
        Manifest::parse("a/b.gltf\tmesh\t00000000000000ff\tb.rmesh\n");
    REQUIRE(manifest.has_value());

    const ManifestEntry* by_src = manifest->find_by_source("a/b.gltf");
    REQUIRE(by_src != nullptr);
    CHECK(by_src->id == AssetId{0xffull});
    CHECK(manifest->find_by_id(AssetId{0xffull}) == by_src);
    CHECK(manifest->find_by_source("missing") == nullptr);
    CHECK(manifest->find_by_id(AssetId{0x1ull}) == nullptr);
}

TEST_CASE("tolerates Windows CRLF line endings") {
    const std::optional<Manifest> manifest = Manifest::parse("x.gltf\tmesh\t1\tx.rmesh\r\n");
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->entries().size() == 1);
    CHECK(manifest->entries()[0].cooked_file == "x.rmesh"); // no stray '\r'
    CHECK(manifest->entries()[0].id == AssetId{0x1ull});
}

TEST_CASE("rejects malformed lines") {
    CHECK_FALSE(Manifest::parse("too\tfew\tfields\n").has_value());         // three fields
    CHECK_FALSE(Manifest::parse("a\tb\tc\td\te\n").has_value());            // five fields
    CHECK_FALSE(Manifest::parse("a\tnotakind\t1\tf.rmesh\n").has_value());  // unknown kind
    CHECK_FALSE(Manifest::parse("a\tmesh\tnothex\tf.rmesh\n").has_value()); // malformed id
}

TEST_CASE("an empty or comment-only manifest is valid and empty") {
    const std::optional<Manifest> empty = Manifest::parse("");
    REQUIRE(empty.has_value());
    CHECK(empty->entries().empty());

    const std::optional<Manifest> comment = Manifest::parse("# just a header\n");
    REQUIRE(comment.has_value());
    CHECK(comment->entries().empty());
}
