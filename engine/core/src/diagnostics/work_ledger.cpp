// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/diagnostics/work_ledger.hpp"

#include <algorithm>

namespace rime::core {
namespace {

// JSON string escaping, restricted to what RFC 8259 actually requires: the two structural
// characters and the C0 control range. Counter names in this engine are literals like
// "physics.awake_bodies", so this will almost never fire — but "almost never" is not "never", and
// a name arriving from a config file one day should produce invalid JSON no more than a name from
// a literal does.
void append_escaped(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += kHex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
}

const char* kind_word(BudgetKind kind) {
    return kind == BudgetKind::AtMost ? "at most" : "at least";
}

} // namespace

WorkCounter* WorkLedger::find(std::string_view name) {
    const auto it = std::find_if(counters_.begin(), counters_.end(), [name](const WorkCounter& c) {
        return c.name == name;
    });
    return it == counters_.end() ? nullptr : &*it;
}

const WorkCounter* WorkLedger::find(std::string_view name) const {
    const auto it = std::find_if(counters_.begin(), counters_.end(), [name](const WorkCounter& c) {
        return c.name == name;
    });
    return it == counters_.end() ? nullptr : &*it;
}

void WorkLedger::set(std::string_view name, std::uint64_t value) {
    if (WorkCounter* existing = find(name)) {
        existing->value = value;
        return;
    }
    counters_.push_back({name, value});
}

void WorkLedger::add(std::string_view name, std::uint64_t delta) {
    if (WorkCounter* existing = find(name)) {
        existing->value += delta;
        return;
    }
    counters_.push_back({name, delta});
}

std::uint64_t WorkLedger::value_or(std::string_view name, std::uint64_t fallback) const {
    const WorkCounter* c = find(name);
    return c ? c->value : fallback;
}

bool WorkLedger::contains(std::string_view name) const {
    return find(name) != nullptr;
}

std::string WorkLedger::to_json(int indent) const {
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent), ' ') : "";

    std::string out = "{";
    bool first = true;
    for (const WorkCounter& c : counters_) {
        if (!first)
            out += ',';
        first = false;
        if (pretty) {
            out += '\n';
            out += pad;
        }
        out += '"';
        append_escaped(out, c.name);
        out += pretty ? "\": " : "\":";
        out += std::to_string(c.value);
    }
    if (pretty && !counters_.empty())
        out += '\n';
    out += '}';
    return out;
}

WorkBudget& WorkBudget::at_most(std::string_view name, std::uint64_t limit) {
    rules_.push_back({name, BudgetKind::AtMost, limit});
    return *this;
}

WorkBudget& WorkBudget::at_least(std::string_view name, std::uint64_t limit) {
    rules_.push_back({name, BudgetKind::AtLeast, limit});
    return *this;
}

std::vector<BudgetViolation> WorkBudget::check(const WorkLedger& ledger) const {
    std::vector<BudgetViolation> out;
    for (const BudgetRule& rule : rules_) {
        // A rule naming a counter nobody recorded FAILS rather than passing vacuously. Treating it
        // as "nothing to check" is how a gate quietly stops gating: rename a counter, and its
        // budget keeps reporting success over a value it can no longer see.
        if (!ledger.contains(rule.name)) {
            out.push_back({rule.name, rule.kind, rule.limit, 0, BudgetOutcome::Missing});
            continue;
        }
        const std::uint64_t value = ledger.value_or(rule.name);
        const bool ok = rule.kind == BudgetKind::AtMost ? value <= rule.limit : value >= rule.limit;
        if (!ok)
            out.push_back({rule.name, rule.kind, rule.limit, value, BudgetOutcome::Breach});
    }
    return out;
}

std::string WorkBudget::format(const std::vector<BudgetViolation>& violations) {
    std::string out;
    for (const BudgetViolation& v : violations) {
        out += "  ";
        out.append(v.name);
        if (v.outcome == BudgetOutcome::Missing) {
            out += ": NOT RECORDED (budget expected ";
            out += kind_word(v.kind);
            out += ' ';
            out += std::to_string(v.limit);
            out += ") — the counter is missing, so this budget could never have failed";
        } else {
            out += ": ";
            out += std::to_string(v.value);
            out += ", expected ";
            out += kind_word(v.kind);
            out += ' ';
            out += std::to_string(v.limit);
        }
        out += '\n';
    }
    return out;
}

} // namespace rime::core
