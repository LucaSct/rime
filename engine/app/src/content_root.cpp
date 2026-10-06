// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The content root search (m20.2). The order and its reasons are in the header; this file is the
// mechanics, and the one rule worth restating here is that NO step throws: a path that cannot be
// stat'ed is a verdict in `tried`, because the error a user reads is built from that list.

#include "rime/app/content_root.hpp"

#include <fmt/core.h>

#include <system_error>

#include "rime/platform/filesystem.hpp"

namespace rime::app {

namespace fs = std::filesystem;

std::string_view to_string(ContentSource source) noexcept {
    switch (source) {
        case ContentSource::None:
            return "none";
        case ContentSource::Explicit:
            return "explicit";
        case ContentSource::BesideExecutable:
            return "beside the executable";
        case ContentSource::InTree:
            return "in-tree (dev)";
    }
    return "unknown";
}

namespace {

// An absolute, lexically clean form for printing and comparing. `weakly_canonical` resolves the
// symlinks of the part that exists — so `build/dev/bin` reached through a symlinked checkout still
// compares equal to itself — and never throws through the error_code overload.
fs::path clean(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? p.lexically_normal() : out;
}

// Does `root` qualify? Returns the verdict text; `ok` says whether it is a root.
std::string judge(const fs::path& root, const fs::path& required, bool& ok) {
    ok = false;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return "missing";
    }
    if (!required.empty() && !fs::is_regular_file(root / required, ec)) {
        return fmt::format("exists, but has no {}", required.generic_string());
    }
    ok = true;
    return "found";
}

} // namespace

ContentRoot resolve_content_root(const ContentSearch& search) {
    ContentRoot out{};
    auto note = [&](std::string_view label, const fs::path& where, std::string_view verdict) {
        out.tried.push_back(fmt::format("{} {}: {}", label, where.string(), verdict));
    };
    auto accept = [&](const fs::path& dir, ContentSource source, std::string why) {
        out.ok = true;
        out.dir = dir;
        out.source = source;
        out.why = std::move(why);
        return out;
    };

    // ── 1. --content: the only candidate when given ─────────────────────────────────────────────
    if (!search.explicit_dir.empty()) {
        const fs::path dir = clean(search.explicit_dir);
        bool ok = false;
        const std::string verdict = judge(dir, search.required, ok);
        note("--content", dir, verdict);
        if (ok) {
            return accept(dir, ContentSource::Explicit, "named by --content");
        }
        out.tried.emplace_back("(an explicit --content is the only place looked: nothing else was "
                               "tried)");
        return out;
    }

    // ── 2. beside the executable ────────────────────────────────────────────────────────────────
    const fs::path exe =
        search.executable.empty() ? platform::executable_path() : search.executable;
    fs::path exe_dir;
    if (exe.empty()) {
        out.tried.emplace_back("beside the executable: the executable's own path is unknown on "
                               "this platform");
    } else {
        exe_dir = clean(exe).parent_path();
        const fs::path dir = exe_dir / "content";
        bool ok = false;
        const std::string verdict = judge(dir, search.required, ok);
        note("beside the executable", dir, verdict);
        if (ok) {
            return accept(dir,
                          ContentSource::BesideExecutable,
                          fmt::format("the content/ directory beside {}", clean(exe).string()));
        }
    }

    // ── 3. the in-tree dev fallback — only for a binary that has not been moved ────────────────
    if (search.dev_content_dir.empty() || search.dev_binary_dir.empty()) {
        out.tried.emplace_back("in-tree fallback: none (this build names no source-tree content)");
        return out;
    }
    const fs::path tree = clean(search.dev_content_dir);
    const fs::path bin = clean(search.dev_binary_dir);
    std::error_code ec;
    const bool in_build_dir = !exe_dir.empty() && fs::equivalent(exe_dir, bin, ec) && !ec;
    if (!in_build_dir) {
        note("in-tree fallback",
             tree,
             fmt::format("not considered — the executable is not in its build directory {} "
                         "(a moved binary never reads the source tree)",
                         bin.string()));
        return out;
    }
    bool ok = false;
    const std::string verdict = judge(tree, search.required, ok);
    note("in-tree fallback", tree, verdict);
    if (ok) {
        return accept(tree,
                      ContentSource::InTree,
                      fmt::format("in-tree fallback: the executable is in its build directory {} "
                                  "(dev run; a bundle never uses this)",
                                  bin.string()));
    }
    return out;
}

std::string describe_content_failure(const ContentRoot& root) {
    std::string text = "no content root found; tried, in order:";
    for (std::size_t i = 0; i < root.tried.size(); ++i) {
        text += fmt::format("\n  {}. {}", i + 1, root.tried[i]);
    }
    return text;
}

} // namespace rime::app
