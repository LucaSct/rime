// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The adapter-selection rules, as pure logic over AdapterCandidate. Two rules carry a deliberate
// "why". Ties break to the LOWEST index (the first the driver enumerated): enumeration order is
// stable on a given machine but not something a caller should know about, so picking the first
// keeps the choice deterministic — a strict `>` means a later equal-scored device never displaces
// an earlier one. An explicit preference outranks the type score: "run this job on the CPU adapter"
// is a stronger instruction than "prefer a discrete GPU", and filtering to the named adapter first
// is what makes a two-GPU machine (a 1060 for everything, a 3060 for heavy work) expressible at
// all.

#include "rime/rhi/adapter_choice.hpp"

#include <cctype>

namespace rime::rhi {
namespace {

// ASCII-only case-insensitive comparison. We deliberately avoid std::locale: it is slow, and its
// behaviour varies by the process's locale, which a GPU-name match has no business depending on.
bool ascii_iequals(char a, char b) noexcept {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

bool contains_ignore_case(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && ascii_iequals(haystack[i + j], needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

} // namespace

std::optional<AdapterChoice> choose_adapter(std::span<const AdapterCandidate> candidates,
                                            std::string_view preference) {
    if (candidates.empty())
        return std::nullopt;

    // Does the preference name at least one candidate? If so we choose from ONLY those; if not we
    // fall back to the default choice below rather than failing — a stale or mistyped preference
    // must not prevent device creation.
    bool preference_matched_any = false;
    if (!preference.empty()) {
        for (const AdapterCandidate& c : candidates) {
            if (contains_ignore_case(c.name, preference)) {
                preference_matched_any = true;
                break;
            }
        }
    }

    // Strict `>` (not `>=`) is what implements the lowest-index-wins tie rule: an equal score never
    // displaces the incumbent, so the first candidate of a tied group wins.
    std::size_t best = 0;
    int best_score = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (preference_matched_any && !contains_ignore_case(candidates[i].name, preference))
            continue;
        if (candidates[i].type_score > best_score) {
            best_score = candidates[i].type_score;
            best = i;
        }
    }

    AdapterChoice choice{};
    choice.index = best;
    choice.preference_matched = preference_matched_any;
    choice.preference_unmatched = !preference.empty() && !preference_matched_any;
    return choice;
}

} // namespace rime::rhi
