// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Reader for ICEM's provenance file (E3): the `.icejson` "why" export — the Ledger DAG recording which
// law / material / rule / safety-factor produced every computed value. The viewer surfaces it in a
// provenance panel so a selected quantity shows *why it is what it is* (its causes). ICEM writes the
// file deliberately one node object per line inside a small header (see ICEM's docs/math/provenance-io.md),
// so this parses it line-by-line with simple string scans — no JSON library. The two repos share only
// the file format, never code. See docs/math/provenance-panel.md.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace rime::viewer {

// One ledger node: a computed value and its causes.
struct ProvNode {
    std::uint32_t id = 0;
    std::string origin;             // Input / Law / Material / Rule / Heuristic / SimResult / SafetyFactor / Derived
    std::string label;              // human-readable, e.g. "hoop stress  sigma = p r / t"
    double value = 0.0;             // SI base units
    std::string unit;
    std::vector<std::uint32_t> inputs; // ids of the nodes that produced this one
};

struct Provenance {
    std::string design;
    std::string hash;
    bool passes = false;
    std::vector<ProvNode> nodes;
    [[nodiscard]] bool usable() const { return !nodes.empty(); }
};

namespace detail {

// The string value of `"key":"..."` in `line`, or "" if absent. Labels/units carry no quotes to escape.
[[nodiscard]] inline std::string json_str(const std::string& line, const std::string& key) {
    const std::string k = "\"" + key + "\":\"";
    const std::size_t a = line.find(k);
    if (a == std::string::npos) return {};
    const std::size_t b = line.find('"', a + k.size());
    if (b == std::string::npos) return {};
    return line.substr(a + k.size(), b - (a + k.size()));
}

// The numeric value of `"key":<number>` in `line` (reads up to the next ',' or '}' or ']').
[[nodiscard]] inline double json_num(const std::string& line, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    const std::size_t a = line.find(k);
    if (a == std::string::npos) return 0.0;
    return std::strtod(line.c_str() + a + k.size(), nullptr);
}

// The integer array of `"inputs":[a,b,c]` in `line`.
[[nodiscard]] inline std::vector<std::uint32_t> json_ids(const std::string& line) {
    std::vector<std::uint32_t> ids;
    const std::string k = "\"inputs\":[";
    const std::size_t a = line.find(k);
    if (a == std::string::npos) return ids;
    const std::size_t b = line.find(']', a);
    std::size_t p = a + k.size();
    while (p < b) {
        char* end = nullptr;
        const long v = std::strtol(line.c_str() + p, &end, 10);
        if (end == line.c_str() + p) break; // no digits (empty array)
        ids.push_back(static_cast<std::uint32_t>(v));
        p = static_cast<std::size_t>(end - line.c_str());
        while (p < b && (line[p] == ',' || line[p] == ' ')) ++p;
    }
    return ids;
}

} // namespace detail

// Parse a `.icejson` provenance file. nullopt on I/O error or if it is not an icem-provenance file.
[[nodiscard]] inline std::optional<Provenance> load_provenance(const std::string& path) {
    std::ifstream is(path);
    if (!is) return std::nullopt;

    Provenance p;
    std::string line;
    bool header_seen = false;
    while (std::getline(is, line)) {
        if (!header_seen && line.find("\"kind\":\"icem-provenance\"") != std::string::npos) {
            header_seen = true;
            p.design = detail::json_str(line, "design");
            p.hash = detail::json_str(line, "hash");
            p.passes = line.find("\"passes\":true") != std::string::npos;
            continue;
        }
        if (line.rfind("{\"id\":", 0) == 0) { // a node object line
            ProvNode n;
            n.id = static_cast<std::uint32_t>(detail::json_num(line, "id"));
            n.origin = detail::json_str(line, "origin");
            n.label = detail::json_str(line, "label");
            n.value = detail::json_num(line, "value");
            n.unit = detail::json_str(line, "unit");
            n.inputs = detail::json_ids(line);
            p.nodes.push_back(std::move(n));
        }
    }
    if (!header_seen || p.nodes.empty()) return std::nullopt;
    return p;
}

} // namespace rime::viewer
