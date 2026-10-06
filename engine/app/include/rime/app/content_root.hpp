// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The content root (m20.2, ADR-0076-m20.2): where a shipped game finds its files.
//
// ADR-0046 §1 says content resolves RELATIVE TO THE EXECUTABLE. Before this brick every sample that
// read a file took its directory from a compile-time path (`RIME_BLOCK_COOKED_DIR` and friends), so
// a copied build looked for its content where the source tree used to be — which works on exactly
// one machine, the one that built it, and fails everywhere else by loading nothing.
//
// One function answers the question, with a fixed search order:
//
//   1. `--content <dir>` — explicit. When it is given it is the ONLY candidate: an operator who
//      names a directory and is wrong must hear so, not be quietly handed some other content.
//   2. `<directory of the executable>/content` — the bundle layout `scripts/export-game.sh` writes.
//   3. the game's in-tree content directory — the DEV fallback. Considered only when the running
//      executable sits in the directory the build wrote it to (`dev_binary_dir`). That is the
//      precise meaning of "a dev build" here: not a build type (Release ctest runs in-tree too) but
//      "this binary has not been moved". A copied binary — a bundle — never falls back to the
//      source tree, so deleting a bundle's content is an error even on the machine that built it,
//      instead of a run that silently reads the repository and passes.
//
// A candidate counts only if it holds `required` (the entry scene), so an empty or wrong `content/`
// beside the binary is "tried, and why it did not qualify", not a root. A failure names EVERY
// candidate and its verdict: the error a user reads must say where we looked.
namespace rime::app {

enum class ContentSource : std::uint8_t {
    None,             // not resolved (the game declares no content), or the search failed
    Explicit,         // --content
    BesideExecutable, // <exe dir>/content
    InTree,           // the dev fallback
};

[[nodiscard]] std::string_view to_string(ContentSource source) noexcept;

struct ContentSearch {
    std::filesystem::path explicit_dir; // --content; empty = not given
    // The running executable. Empty = `platform::executable_path()`. Injectable so a test can put
    // the "executable" anywhere without copying a binary.
    std::filesystem::path executable;
    // The dev fallback: the game's content directory in the source tree, and the directory the
    // build writes the game's executable into. Both empty = no fallback (what a test or a game
    // that never shipped from this tree passes).
    std::filesystem::path dev_content_dir;
    std::filesystem::path dev_binary_dir;
    // A path RELATIVE to a candidate root that must exist for the candidate to count. Empty = the
    // directory existing is enough.
    std::filesystem::path required;
};

struct ContentRoot {
    bool ok = false;
    std::filesystem::path dir; // absolute, when ok
    ContentSource source = ContentSource::None;
    std::string why; // one line: why this root (when ok) — what the runner logs
    // Every candidate, in search order, with its verdict ("found", "missing", "not considered: …").
    // Kept on success too: "why not the one I expected" is a question an operator asks.
    std::vector<std::string> tried;
};

// Resolve. Never throws: filesystem errors are verdicts in `tried`.
[[nodiscard]] ContentRoot resolve_content_root(const ContentSearch& search);

// The failure text: "no content root found" and one line per candidate tried.
[[nodiscard]] std::string describe_content_failure(const ContentRoot& root);

} // namespace rime::app
