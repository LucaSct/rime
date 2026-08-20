// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The WORK LEDGER — how much work a run did, as exact integer counts, never as clocks.
//
// The technique, and why it exists (ADR-0035 §2). Rime's proofs are structural: properties the
// system guarantees, checked with margins, never golden images. Performance resisted that for
// eleven milestones because CI renders on lavapipe, a CPU rasterizer, where absolute time means
// nothing — so "is it fast?" could only be asked on hardware that CI does not have.
//
// The way out is to split the question in two. "How FAST does the work run" genuinely needs a GPU
// and a wall clock, and lives in a hardware report. "How MUCH work does the frame do" does not:
// draw calls, awake bodies, shadow slots re-rendered, bytes replicated, SDF regions re-stamped.
// Those are integers a machine computes identically everywhere, so **lavapipe can gate them in CI,
// deterministically, forever**. And they are what actually regresses: most "the demo got slow"
// commits do not make the same work slower, they quietly make the frame do more of it — a cull
// that stops culling, a cache that stops hitting, a sleeping pile that stops sleeping. A ledger
// catches that class on a machine with no GPU at all.
//
// Two deliberate constraints, both load-bearing:
//
//   * **Integers only.** There is no `set(name, double)` and that is the point, not an oversight.
//     A float in here would be a duration in disguise, and one machine's milliseconds are not
//     another's — the moment a timing enters the ledger, the ledger stops being comparable across
//     machines, which is the single property it exists to have. Timings live in the hardware
//     report, which is honest about being machine-specific.
//   * **The ledger records; it does not judge.** Budgets live in `WorkBudget` below, so the code
//     that produces a number never also decides whether the number is acceptable.
namespace rime::core {

// One named count. `name` is a borrowed string_view: entries are expected to be named by string
// literals (`"draws.submitted"`), which outlive the ledger. Storing a std::string per counter
// would allocate once per entry per frame for names that never change.
struct WorkCounter {
    std::string_view name;
    std::uint64_t value = 0;
};

// An ordered set of named counts for one run (or one frame).
//
// Entries keep INSERTION order rather than sorted order, because a ledger is read by a human
// diffing two runs and grouping matters — every physics counter together, then every render
// counter — while a sorted list interleaves subsystems alphabetically into noise. Order is stable
// for a given binary because the assembling code runs in a fixed sequence. Tools that compare two
// ledgers must still match entries **by name**, never by position: a run that skips a subsystem
// legitimately omits its rows, and position-matching would silently compare unrelated counters.
class WorkLedger {
public:
    // Record `name = value`, replacing any previous value for that name. Last write wins, so a
    // caller may overwrite a provisional figure without first checking whether it exists.
    void set(std::string_view name, std::uint64_t value);

    // Accumulate into `name`, creating it at `delta` if absent. For counters summed across ticks.
    void add(std::string_view name, std::uint64_t delta);

    // The value recorded for `name`, or `fallback` when nothing was recorded. Prefer `contains()`
    // when the difference between "absent" and "zero" matters — and it usually does: a counter
    // that is absent because nobody wired it up looks exactly like a subsystem that did no work,
    // and those two deserve very different reactions.
    [[nodiscard]] std::uint64_t value_or(std::string_view name, std::uint64_t fallback = 0) const;

    [[nodiscard]] bool contains(std::string_view name) const;

    [[nodiscard]] const std::vector<WorkCounter>& counters() const noexcept { return counters_; }

    [[nodiscard]] std::size_t size() const noexcept { return counters_.size(); }

    [[nodiscard]] bool empty() const noexcept { return counters_.empty(); }

    void clear() noexcept { counters_.clear(); }

    // Serialize as a JSON object, in insertion order: {"a":1,"b":2}. Hand-written rather than
    // pulled from a library because the shape is flat integers and borrowed names — a dependency
    // would have to earn its place here, and a dozen lines of writer does not clear that bar (the
    // same call this engine made on its UI and its physics solver). `indent` >= 0 pretty-prints
    // with that many spaces per level; a negative value emits one dense line.
    [[nodiscard]] std::string to_json(int indent = 2) const;

private:
    // Linear scan on lookup. A ledger holds tens of counters, not thousands, and is written far
    // more often than it is searched; a hash map would cost more in allocation and cache misses
    // than the scan it replaces, and would lose the insertion order the format depends on.
    [[nodiscard]] WorkCounter* find(std::string_view name);
    [[nodiscard]] const WorkCounter* find(std::string_view name) const;

    std::vector<WorkCounter> counters_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Budgets — the policy half, kept separate from the recording half.

enum class BudgetKind : std::uint8_t {
    AtMost,  // value must be <= limit (a ceiling: draws, bytes, awake bodies)
    AtLeast, // value must be >= limit (a floor: culled counts, cache reuse)
};

// Why a rule failed. `Missing` is a distinct outcome from a numeric breach and is treated as a
// failure rather than a skip, which is the whole reason this type has three states instead of a
// bool. A budget naming a counter nobody records is a budget that can never fail, and a renamed
// counter would otherwise disable its own gate **silently** — the exact shape of bug this engine
// has repeatedly caught only by counting the things it skipped.
enum class BudgetOutcome : std::uint8_t { Pass, Breach, Missing };

struct BudgetRule {
    std::string_view name;
    BudgetKind kind = BudgetKind::AtMost;
    std::uint64_t limit = 0;
};

struct BudgetViolation {
    std::string_view name;
    BudgetKind kind = BudgetKind::AtMost;
    std::uint64_t limit = 0;
    std::uint64_t value = 0; // meaningless when outcome == Missing
    BudgetOutcome outcome = BudgetOutcome::Breach;
};

// A set of rules to check a ledger against. Held apart from the ledger so that the code producing
// a number never also decides whether that number is acceptable — and so a sample can keep all its
// budgets in one readable block instead of scattering thresholds through its frame loop.
class WorkBudget {
public:
    WorkBudget& at_most(std::string_view name, std::uint64_t limit);
    WorkBudget& at_least(std::string_view name, std::uint64_t limit);

    [[nodiscard]] const std::vector<BudgetRule>& rules() const noexcept { return rules_; }

    // Every rule that did not pass, in rule order. Empty means the ledger is within budget.
    [[nodiscard]] std::vector<BudgetViolation> check(const WorkLedger& ledger) const;

    // A human-readable multi-line report of `violations`, one per line, suitable for printing
    // straight to stderr before a non-zero exit. Empty string when there are none.
    [[nodiscard]] static std::string format(const std::vector<BudgetViolation>& violations);

private:
    std::vector<BudgetRule> rules_;
};

} // namespace rime::core
