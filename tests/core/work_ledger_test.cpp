// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for m12.0-perf's work ledger (ADR-0035 §2a): the CI-gateable half of performance
// governance. The ledger records how much work a run did as exact integer counts, and a budget
// decides whether those counts are acceptable — a split that keeps the code producing a number
// from also judging it.
//
// The load-bearing case here is the LAST one. A budget naming a counter nobody recorded must
// FAIL, not pass vacuously: that is the difference between a gate and a decoration, and it is the
// failure this engine has hit repeatedly in other guises — a skip path with no counter reads
// exactly like a system doing its job. Everything above it is ordinary bookkeeping; that case is
// the reason the type has three outcomes instead of a bool.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "rime/core/diagnostics/work_ledger.hpp"

using rime::core::BudgetKind;
using rime::core::BudgetOutcome;
using rime::core::WorkBudget;
using rime::core::WorkLedger;

TEST_CASE("a ledger records, overwrites, and accumulates counts") {
    WorkLedger ledger;
    CHECK(ledger.empty());

    ledger.set("draws.submitted", 120);
    ledger.set("draws.culled", 80);
    CHECK(ledger.size() == 2);
    CHECK(ledger.value_or("draws.submitted") == 120);

    SUBCASE("set replaces rather than appending a duplicate") {
        ledger.set("draws.submitted", 200);
        CHECK(ledger.size() == 2);
        CHECK(ledger.value_or("draws.submitted") == 200);
    }

    SUBCASE("add accumulates, and creates the counter when absent") {
        ledger.add("draws.submitted", 5);
        CHECK(ledger.value_or("draws.submitted") == 125);

        ledger.add("physics.awake_bodies", 7); // absent: starts at the delta
        CHECK(ledger.value_or("physics.awake_bodies") == 7);
        CHECK(ledger.size() == 3);
    }

    SUBCASE("absent is distinguishable from zero") {
        ledger.set("shadow.rendered", 0);
        CHECK(ledger.contains("shadow.rendered"));
        CHECK_FALSE(ledger.contains("shadow.reused"));
        // Both read as 0 through value_or — which is exactly why contains() exists. A counter
        // nobody wired up must never be mistaken for a subsystem that did no work.
        CHECK(ledger.value_or("shadow.rendered") == 0);
        CHECK(ledger.value_or("shadow.reused") == 0);
        CHECK(ledger.value_or("shadow.reused", 99) == 99);
    }
}

TEST_CASE("insertion order is preserved, so a human diff groups by subsystem") {
    WorkLedger ledger;
    ledger.set("zeta", 1);
    ledger.set("alpha", 2);
    ledger.set("mid", 3);
    ledger.set("alpha", 4); // updating must not reorder

    const auto& counters = ledger.counters();
    REQUIRE(counters.size() == 3);
    CHECK(counters[0].name == "zeta");
    CHECK(counters[1].name == "alpha");
    CHECK(counters[2].name == "mid");
    CHECK(counters[1].value == 4);
}

TEST_CASE("the ledger serializes as flat JSON") {
    WorkLedger ledger;

    SUBCASE("empty is a valid empty object") {
        CHECK(ledger.to_json(-1) == "{}");
        CHECK(ledger.to_json(2) == "{}");
    }

    SUBCASE("dense form is one line, in insertion order") {
        ledger.set("b", 2);
        ledger.set("a", 1);
        CHECK(ledger.to_json(-1) == R"({"b":2,"a":1})");
    }

    SUBCASE("pretty form indents and stays parseable") {
        ledger.set("draws.submitted", 120);
        ledger.set("draws.culled", 80);
        CHECK(ledger.to_json(2) == "{\n  \"draws.submitted\": 120,\n  \"draws.culled\": 80\n}");
    }

    SUBCASE("names needing escapes do not produce invalid JSON") {
        ledger.set("odd\"name\\here", 1);
        CHECK(ledger.to_json(-1) == R"({"odd\"name\\here":1})");
    }

    SUBCASE("a 64-bit count survives the round trip without narrowing") {
        // Byte counters are the realistic path to large values; a double-backed JSON writer would
        // lose the low bits here, which is one more reason the ledger is integers all the way.
        const std::uint64_t big = 18446744073709551615ULL; // 2^64 - 1
        ledger.set("net.bytes", big);
        CHECK(ledger.to_json(-1) == R"({"net.bytes":18446744073709551615})");
    }
}

TEST_CASE("budgets check ceilings and floors") {
    WorkLedger ledger;
    ledger.set("draws.after_cull", 900);
    ledger.set("draws.culled", 3100);
    ledger.set("physics.awake_bodies", 0);

    SUBCASE("a satisfied budget reports nothing") {
        WorkBudget budget;
        budget.at_most("draws.after_cull", 1000)
            .at_least("draws.culled", 1)
            .at_most("physics.awake_bodies", 0);
        CHECK(budget.check(ledger).empty());
        CHECK(WorkBudget::format(budget.check(ledger)).empty());
    }

    SUBCASE("a ceiling breach is reported with both numbers") {
        WorkBudget budget;
        budget.at_most("draws.after_cull", 500);
        const auto violations = budget.check(ledger);
        REQUIRE(violations.size() == 1);
        CHECK(violations[0].outcome == BudgetOutcome::Breach);
        CHECK(violations[0].kind == BudgetKind::AtMost);
        CHECK(violations[0].value == 900);
        CHECK(violations[0].limit == 500);
        CHECK(WorkBudget::format(violations).find("900") != std::string::npos);
    }

    SUBCASE("a floor catches the counting rule's harder half") {
        // The cull counter exists so that culling degrading to "culled 0 of 4000" is a red number
        // rather than a warm frame. A ceiling alone cannot see that: 0 is under every ceiling.
        WorkLedger degraded;
        degraded.set("draws.after_cull", 4000);
        degraded.set("draws.culled", 0);

        WorkBudget budget;
        budget.at_least("draws.culled", 1);
        const auto violations = budget.check(degraded);
        REQUIRE(violations.size() == 1);
        CHECK(violations[0].kind == BudgetKind::AtLeast);
        CHECK(violations[0].value == 0);
    }

    SUBCASE("violations are reported in rule order, and passes are omitted") {
        WorkBudget budget;
        budget
            .at_most("draws.after_cull", 100)    // breach
            .at_most("physics.awake_bodies", 10) // pass
            .at_least("draws.culled", 999999);   // breach
        const auto violations = budget.check(ledger);
        REQUIRE(violations.size() == 2);
        CHECK(violations[0].name == "draws.after_cull");
        CHECK(violations[1].name == "draws.culled");
    }
}

TEST_CASE("a budget over a counter nobody recorded FAILS rather than passing vacuously") {
    // The case the whole design turns on. Rename a counter and forget to rename its budget, and a
    // "skip the check" policy would keep reporting success over a value it can no longer see — a
    // gate that cannot fail, indistinguishable from a gate that passes.
    WorkLedger ledger;
    ledger.set("draws.after_frustum_cull", 900); // note: renamed

    WorkBudget budget;
    budget.at_most("draws.after_cull", 1000); // the old name

    const auto violations = budget.check(ledger);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].outcome == BudgetOutcome::Missing);
    CHECK(violations[0].name == "draws.after_cull");

    // And the message says WHY this is a failure, because "not recorded" reads like a non-event
    // unless the report spells out that the budget could never have fired.
    const std::string report = WorkBudget::format(violations);
    CHECK(report.find("NOT RECORDED") != std::string::npos);
    CHECK(report.find("could never have failed") != std::string::npos);
}
