// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The content root search (m20.2, ADR-0076-m20.2): the order, the "a moved binary never reads the
// source tree" rule, and the failure text naming every path tried. The executable path is INJECTED
// (`ContentSearch::executable`), so these tests move a pretend binary around a temp directory
// instead of copying a real one — the real-binary half is `scripts/export-proof.sh`.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "rime/app/content_root.hpp"

using namespace rime::app;
namespace fs = std::filesystem;

namespace {

// A scratch tree: <tmp>/rime_content_root_test/{build/bin, src/content, bundle/{content}}.
struct Tree {
    fs::path base = fs::temp_directory_path() / "rime_content_root_test";
    fs::path bin = base / "build" / "bin";
    fs::path src_content = base / "src" / "content";
    fs::path bundle = base / "bundle";

    Tree() {
        fs::remove_all(base);
        fs::create_directories(bin);
        fs::create_directories(src_content);
        fs::create_directories(bundle / "content");
        touch(src_content / "level.rscene");
        touch(bundle / "content" / "level.rscene");
    }

    ~Tree() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    static void touch(const fs::path& p) { std::ofstream(p) << "rime_scene 1\n"; }

    // A search for a game whose build put its binary in `bin` and whose source content is
    // `src_content`, run as if the executable were `exe`.
    [[nodiscard]] ContentSearch search(const fs::path& exe) const {
        ContentSearch s{};
        s.executable = exe;
        s.dev_content_dir = src_content;
        s.dev_binary_dir = bin;
        s.required = "level.rscene";
        return s;
    }
};

// The same normalisation content_root.cpp's own `clean()` applies before it PRINTS a candidate
// (engine/app/src/content_root.cpp). A needle built from the raw path is not the string the
// implementation emits: `weakly_canonical` resolves the symlinks of the part that exists, and on
// Windows it also rewrites an 8.3 short component — `C:\Users\RUNNER~1\...` becomes
// `C:\Users\runneradmin\...` — so the uncanonicalised needle never appeared in `tried` and
// this file's one path-derived assertion failed on windows-latest only.
fs::path printed_as(const fs::path& p) {
    std::error_code ec;
    const fs::path out = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? p.lexically_normal() : out;
}

bool tried_mentions(const ContentRoot& r, const std::string& needle) {
    for (const std::string& line : r.tried) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("content root: a binary in its build directory falls back to the source tree") {
    const Tree t;
    const ContentRoot r = resolve_content_root(t.search(t.bin / "game"));
    REQUIRE(r.ok);
    CHECK(r.source == ContentSource::InTree);
    CHECK(fs::equivalent(r.dir, t.src_content));
    // It looked beside the executable FIRST and says so.
    CHECK(tried_mentions(r, "beside the executable"));
    CHECK(r.why.find("in-tree fallback") != std::string::npos);
}

TEST_CASE("content root: content beside the executable wins over the source tree") {
    const Tree t;
    fs::create_directories(t.bin / "content");
    Tree::touch(t.bin / "content" / "level.rscene");
    const ContentRoot r = resolve_content_root(t.search(t.bin / "game"));
    REQUIRE(r.ok);
    CHECK(r.source == ContentSource::BesideExecutable);
    CHECK(fs::equivalent(r.dir, t.bin / "content"));
}

TEST_CASE("content root: a moved binary (a bundle) finds its own content") {
    const Tree t;
    const ContentRoot r = resolve_content_root(t.search(t.bundle / "game"));
    REQUIRE(r.ok);
    CHECK(r.source == ContentSource::BesideExecutable);
    CHECK(fs::equivalent(r.dir, t.bundle / "content"));
}

TEST_CASE("content root: a moved binary with no content NEVER falls back to the source tree") {
    // The rule the bundle proof depends on: on the machine that built it, the source tree is right
    // there and readable — and using it would make a bundle with its content deleted pass.
    const Tree t;
    fs::remove_all(t.bundle / "content");
    const ContentRoot r = resolve_content_root(t.search(t.bundle / "game"));
    CHECK_FALSE(r.ok);
    CHECK(r.source == ContentSource::None);
    // Mirrors the implementation: clean(exe).parent_path() / "content".
    const fs::path printed = printed_as(t.bundle / "game").parent_path() / "content";
    CHECK(tried_mentions(r, printed.string() + ": missing"));
    CHECK(tried_mentions(r, "not considered"));
    const std::string text = describe_content_failure(r);
    CHECK(text.find("no content root found") != std::string::npos);
    CHECK(text.find("1. beside the executable") != std::string::npos);
    CHECK(text.find("2. in-tree fallback") != std::string::npos);
}

TEST_CASE("content root: a directory without the required file is not a root") {
    const Tree t;
    fs::remove(t.bundle / "content" / "level.rscene");
    const ContentRoot r = resolve_content_root(t.search(t.bundle / "game"));
    CHECK_FALSE(r.ok);
    CHECK(tried_mentions(r, "exists, but has no level.rscene"));
}

TEST_CASE("content root: an explicit --content is the only candidate") {
    const Tree t;
    // Right: it wins even though the bundle has its own content.
    ContentSearch s = t.search(t.bundle / "game");
    s.explicit_dir = t.src_content;
    const ContentRoot r = resolve_content_root(s);
    REQUIRE(r.ok);
    CHECK(r.source == ContentSource::Explicit);
    CHECK(fs::equivalent(r.dir, t.src_content));

    // Wrong: a refusal, NOT a quiet fall-through to the perfectly good content beside the binary.
    s.explicit_dir = t.base / "nowhere";
    const ContentRoot wrong = resolve_content_root(s);
    CHECK_FALSE(wrong.ok);
    CHECK(tried_mentions(wrong, "nowhere: missing"));
    CHECK_FALSE(tried_mentions(wrong, "beside the executable"));
}

TEST_CASE("content root: no dev fallback declared means none is tried") {
    const Tree t;
    ContentSearch s = t.search(t.bin / "game");
    s.dev_content_dir.clear();
    s.dev_binary_dir.clear();
    const ContentRoot r = resolve_content_root(s);
    CHECK_FALSE(r.ok);
    CHECK(tried_mentions(r, "in-tree fallback: none"));
}
