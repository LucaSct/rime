// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for m12.0-perf's hardware report (ADR-0035 §2b/§2c): the half of performance governance
// that CI cannot run, because it measures a wall clock on one machine's GPU. What CI *can* prove is
// everything around the measurement — that the statistics are the ones claimed, that a report
// survives the round trip to `docs/perf/` unchanged, and above all that the gate can fail.
//
// Four cases carry their weight beyond ordinary bookkeeping:
//
//   * NEAREST-RANK percentiles. p99 must be a frame that actually happened, and with fewer than
//     100 samples it must equal max — a property to state and pin rather than discover later while
//     wondering why a 40-frame run looks so smooth.
//   * The MISSING timeline. A rule reading a timeline nobody recorded fails, exactly as a work
//     budget naming an unrecorded counter fails. Same lesson, same reason.
//   * The STRICT reader. A parse that quietly defaulted a field to 0.0 would make the regression
//     gate compare against zero and pass everything — the gate-that-cannot-fail, arriving through
//     the back door of a lenient parser.
//   * The VACUITY guard. A run that was fast because it did nothing must fail; m11.7 learned that
//     about correctness proofs, and a performance number deserves it more, not less.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"

using rime::core::BaselineStatus;
using rime::core::BudgetOutcome;
using rime::core::Distribution;
using rime::core::DurationSamples;
using rime::core::MachineFingerprint;
using rime::core::PassTiming;
using rime::core::PerfGate;
using rime::core::PerfOutcome;
using rime::core::PerfReport;
using rime::core::PerfStat;
using rime::core::RunInfo;
using rime::core::WorkLedger;

namespace {

// A report shaped like a real run, so the tests below argue about behaviour rather than about
// fixture construction. 200 frames of 4 ms with a single 20 ms hitch at frame 100.
PerfReport make_report(double hitch_ms = 20.0) {
    PerfReport r;
    MachineFingerprint m;
    m.gpu = "NVIDIA GeForce RTX 3060";
    m.driver = "610.43.03";
    m.os = "linux";
    m.build = "Release";
    m.sanitizer = "off";
    m.preset = "all-lighting-gates";
    m.width = 1920;
    m.height = 1080;
    r.set_machine(m);

    RunInfo run;
    run.sample = "11-lit-rooms";
    run.commit = "abc1234";
    run.date = "2026-08-20";
    r.set_run(run);

    for (std::uint64_t i = 0; i < 200; ++i) {
        const double ms = (i == 100) ? hitch_ms : 4.0;
        const std::vector<PassTiming> passes = {{"gbuffer", ms * 0.5}, {"lighting", ms * 0.25}};
        r.observe_frame(i, ms, passes);
    }

    WorkLedger ledger;
    ledger.set("draws.submitted", 1200);
    ledger.set("sdf.stamps_on_break", 3);
    r.set_ledger(ledger);
    return r;
}

} // namespace

TEST_CASE("percentiles are nearest-rank order statistics, not interpolations") {
    DurationSamples s;
    for (int i = 1; i <= 100; ++i)
        s.add(static_cast<double>(i)); // 1.0 … 100.0 ms

    const Distribution d = s.summarize();
    CHECK(d.count == 100);
    CHECK(d.min_ms == doctest::Approx(1.0));
    CHECK(d.max_ms == doctest::Approx(100.0));
    // ceil(0.50 * 100) = 50 -> the 50th smallest, a value that genuinely occurred. An
    // interpolating definition would answer 50.5, which nothing measured.
    CHECK(d.p50_ms == doctest::Approx(50.0));
    CHECK(d.p95_ms == doctest::Approx(95.0));
    CHECK(d.p99_ms == doctest::Approx(99.0));

    SUBCASE("under 100 samples, p99 IS max — which is why sample floors exist") {
        DurationSamples few;
        for (int i = 1; i <= 40; ++i)
            few.add(static_cast<double>(i));
        const Distribution fd = few.summarize();
        CHECK(fd.p99_ms == doctest::Approx(fd.max_ms));
        CHECK(fd.count == 40);
    }

    SUBCASE("an empty timeline summarizes to zeros with a zero count, never to a silent pass") {
        const Distribution empty = DurationSamples{}.summarize();
        CHECK(empty.count == 0);
        CHECK(empty.max_ms == doctest::Approx(0.0));
    }
}

TEST_CASE("a report records frames, per-pass cost, and the single worst frame") {
    const PerfReport r = make_report();

    const auto frame = r.distribution("frame");
    REQUIRE(frame.has_value());
    CHECK(frame->count == 200);
    CHECK(frame->p50_ms == doctest::Approx(4.0));
    CHECK(frame->max_ms == doctest::Approx(20.0));

    // The hitch is 1 frame in 200, so it sits above p99 (the 198th of 200) and is invisible to
    // every statistic except max — the case the "never the mean" rule is really about.
    CHECK(frame->p99_ms == doctest::Approx(4.0));

    SUBCASE("the worst frame keeps its own per-pass breakdown") {
        CHECK(r.worst_frame().index == 100);
        CHECK(r.worst_frame().ms == doctest::Approx(20.0));
        REQUIRE(r.worst_frame().passes.size() == 2);
        CHECK(r.worst_frame().passes[0].name == "gbuffer");
        CHECK(r.worst_frame().passes[0].ms == doctest::Approx(10.0));
    }

    SUBCASE("per-pass costs carry p50 and max, and no average to hide behind") {
        const auto passes = r.passes();
        REQUIRE(passes.size() == 2);
        CHECK(passes[0].name == "gbuffer");
        CHECK(passes[0].p50_ms == doctest::Approx(2.0));
        CHECK(passes[0].max_ms == doctest::Approx(10.0));
    }

    SUBCASE("a timeline nobody recorded is absent, which is not the same as zero") {
        CHECK_FALSE(r.distribution("sim.tick").has_value());
    }
}

TEST_CASE("m17.3: two passes with one name are two keys, not one key written twice") {
    // THE BUG THIS PINS SHIPPED, AND IT IS IN THE REPOSITORY. A CSM declares the depth pre-pass
    // once per cascade, so `docs/perf/2026-08-30-99-the-block-*.json` contains FOUR
    // `"depth-prepass"` keys in one object. Every ordinary JSON reader keeps one of them, so the
    // committed artifact under-reports its own worst frame by 0.32 ms and no reader can tell.
    // Meanwhile the fold below put four different renders into one distribution, making
    // `depth-prepass p50` the median of individual cascades rather than a per-frame cost — a number
    // that reads as the latter.
    PerfReport r;
    const PassTiming frame_passes[] = {{"shadow", 1.0}, {"forward", 4.0}, {"shadow", 2.0}};
    r.observe_frame(0, 9.0, frame_passes);

    SUBCASE("the worst frame keeps BOTH, under names that differ") {
        REQUIRE(r.worst_frame().passes.size() == 3);
        CHECK(r.worst_frame().passes[0].name == "shadow");
        CHECK(r.worst_frame().passes[1].name == "forward");
        CHECK(r.worst_frame().passes[2].name == "shadow#1");
        CHECK(r.worst_frame().passes[2].ms == doctest::Approx(2.0));
    }

    SUBCASE("the two renders get two distributions, so neither is a median of the other") {
        const auto passes = r.passes();
        REQUIRE(passes.size() == 3);
        CHECK(passes[0].name == "shadow");
        CHECK(passes[0].max_ms == doctest::Approx(1.0));
        CHECK(passes[2].name == "shadow#1");
        CHECK(passes[2].max_ms == doctest::Approx(2.0));
    }

    SUBCASE("the emitted JSON has unique keys — the property the committed file violates") {
        const std::string json = r.to_json();
        // Count occurrences of the quoted key. Two would mean the artifact is still unreadable by
        // anything that is not this file's own parser, which is the whole failure.
        std::size_t at = 0;
        int shadow_keys = 0;
        while ((at = json.find("\"shadow\":", at)) != std::string::npos) {
            ++shadow_keys;
            at += 9;
        }
        CHECK(shadow_keys == 2); // once in `passes`, once in `worst_frame.passes`
        CHECK(json.find("\"shadow#1\":") != std::string::npos);

        PerfReport back;
        std::string error;
        REQUIRE_MESSAGE(PerfReport::parse(json, back, error), error);
        CHECK(back.worst_frame().passes.size() == 3);
        CHECK(back.to_json() == json);
    }
}

TEST_CASE(
    "m17.3b: a zone is measured per call AND per frame, because they answer different questions") {
    // A profile zone fires once per SCOPE ENTRY, and the scopes that matter run many times per
    // frame — a frame steps the simulation several times and each step runs every physics stage.
    // So the timeline a zone feeds is a distribution over CALLS. That is exactly right against
    // ADR-0035's ratified per-TICK budget and exactly wrong for M17's question, which is where a
    // frame's 35.6 ms went: a per-call percentile times a call count is not a per-frame percentile.
    PerfReport r;

    // Frame 0: the zone runs three times, 1 + 2 + 3 ms.
    r.observe_zone("physics.solve", 1.0);
    r.observe_zone("physics.solve", 2.0);
    r.observe_zone("physics.solve", 3.0);
    r.observe_frame(0, 10.0);

    // Frame 1: once, 4 ms.
    r.observe_zone("physics.solve", 4.0);
    r.observe_frame(1, 10.0);

    // Frame 2: the zone does not run at all.
    r.observe_frame(2, 10.0);

    SUBCASE("the per-call timeline keeps its old meaning, one sample per call") {
        const auto per_call = r.distribution("physics.solve");
        REQUIRE(per_call.has_value());
        CHECK(per_call->count == 4);
        CHECK(per_call->max_ms == doctest::Approx(4.0));
    }

    SUBCASE("the per-frame timeline sums the calls, one sample per frame") {
        const auto per_frame = r.distribution("physics.solve.per_frame");
        REQUIRE(per_frame.has_value());
        // Three frames, three samples: 6.0, 4.0 and — the one that is easy to get wrong — 0.0.
        CHECK(per_frame->count == 3);
        CHECK(per_frame->max_ms == doctest::Approx(6.0));
        CHECK(per_frame->min_ms == doctest::Approx(0.0));
        // The distinction the whole thing exists for: the worst CALL was 4 ms, the worst FRAME
        // spent 6. Reading the per-call max as a frame cost understates it by a third here, and by
        // far more in the block, where a frame runs several steps of a many-stage pipeline.
        CHECK(per_frame->max_ms > r.distribution("physics.solve")->max_ms);
    }

    SUBCASE("a frame in which the zone never ran records a zero, not nothing") {
        // Dropping it would bias the percentile upward by discarding exactly the cheap frames, and
        // would leave the per-frame timeline with a different sample count from `frame` — which is
        // the precondition for ever comparing them frame-for-frame.
        const auto per_frame = r.distribution("physics.solve.per_frame");
        const auto frames = r.distribution("frame");
        REQUIRE(per_frame.has_value());
        REQUIRE(frames.has_value());
        CHECK(per_frame->count == frames->count);
    }

    SUBCASE("the per-frame totals survive the round trip to a committed report") {
        std::string json = r.to_json();
        PerfReport back;
        std::string error;
        REQUIRE_MESSAGE(PerfReport::parse(json, back, error), error);
        REQUIRE(back.distribution("physics.solve.per_frame").has_value());
        CHECK(back.distribution("physics.solve.per_frame")->max_ms == doctest::Approx(6.0));
        CHECK(back.to_json() == json);
    }
}

TEST_CASE("a zone closing on another thread is DROPPED and counted, not raced (m17.3c)") {
    // `report_zone` copies the sink out under a mutex and then invokes it with the lock RELEASED —
    // deliberately, so a sink doing real work cannot serialize whatever called it. The consequence
    // is easy to state wrongly: the hazard for a zone on a job-system worker is not contention, it
    // is a DATA RACE, because `PerfReport` synchronizes nothing and two threads inside
    // `observe_zone` share its vectors. The m17.3b review caught `PhysicsWorld::step`'s comment
    // naming the cheap hazard instead of the real one — which is how someone later accepts "just a
    // bit of lock contention" for a debug session and gets undefined behaviour.
    //
    // So `ZoneTimelines` pins itself to the thread that installed it. Dropping is the safe answer;
    // COUNTING the drop is what stops a short report from reading like a quiet one.
    PerfReport r;
    std::uint64_t foreign = 0;
    {
        rime::core::ZoneTimelines zones(r);
        rime::core::report_zone("owner.stage", 1.0);
        std::thread worker([] { rime::core::report_zone("worker.stage", 99.0); });
        worker.join();
        rime::core::report_zone("owner.stage", 2.0);
        foreign = zones.foreign_zones();
    }

    CHECK(foreign == 1);
    // The owning thread's zones landed…
    REQUIRE(r.distribution("owner.stage").has_value());
    CHECK(r.distribution("owner.stage")->count == 2);
    // …and the worker's did not, under any name. Absent rather than zero: nothing measured it.
    CHECK_FALSE(r.distribution("worker.stage").has_value());
    CHECK_FALSE(r.distribution("worker.stage.per_frame").has_value());
}

TEST_CASE("accounting: the remainder gets a NAME, computed per frame (m17.3c)") {
    // "The frame is attributable" was an unchecked belief until this: every pass had a name and
    // every physics stage had one, and nothing said the named parts add up to the whole. A
    // subsystem added without a zone was simply absent — and absent is indistinguishable from free.
    //
    // Why per frame and not from the summaries afterwards: percentiles are not subadditive in
    // either direction (the frames that are worst for one part need not be worst for another), and
    // this schema deliberately cannot store a mean, so the one summary-level check that would have
    // been sound is the one it refuses to hold. Record time or never.
    PerfReport r;
    r.declare_accounting("frame", {"sim", "render"});

    // Frame 0: 10 = 4 + 5 + 1 unnamed.
    r.observe("sim", 4.0);
    r.observe("render", 5.0);
    r.observe_frame(0, 10.0);

    // Frame 1: 10 = 4 + 6, nothing left over.
    r.observe("sim", 4.0);
    r.observe("render", 6.0);
    r.observe_frame(1, 10.0);

    SUBCASE("the residual is exact for each frame, not a difference of percentiles") {
        const auto d = r.distribution("frame.unaccounted");
        REQUIRE(d.has_value());
        CHECK(d->count == 2);
        CHECK(d->max_ms == doctest::Approx(1.0));
        CHECK(d->min_ms == doctest::Approx(0.0));
        CHECK(r.accounting_gaps() == 0);
    }

    SUBCASE("a part recorded for no frame counts a gap and INFLATES the residual, never hides it") {
        // The failure this guards: name a child that never records and the residual quietly grows
        // by its whole cost, which reads as unattributed work rather than as a broken declaration.
        PerfReport g;
        g.declare_accounting("frame", {"sim", "typo.render"});
        g.observe("sim", 4.0);
        g.observe("render", 5.0);
        g.observe_frame(0, 10.0);

        const auto d = g.distribution("frame.unaccounted");
        REQUIRE(d.has_value());
        CHECK(d->max_ms == doctest::Approx(6.0)); // 10 - 4, with `render` never claimed
        CHECK(g.accounting_gaps() == 1);
    }

    SUBCASE("a part recorded AFTER observe_frame belongs to the next frame, and is counted") {
        // The ordering contract, stated as a test because it is the easy mistake: the sample's
        // `frame.render` was observed one line after `observe_frame` before m17.3c.
        PerfReport late;
        late.declare_accounting("frame", {"render"});
        late.observe_frame(0, 10.0);
        late.observe("render", 9.0);
        late.observe_frame(1, 10.0);

        const auto d = late.distribution("frame.unaccounted");
        REQUIRE(d.has_value());
        // Frame 0 saw nothing (gap, residual 10) and frame 1 saw the sample recorded between them.
        CHECK(d->max_ms == doctest::Approx(10.0));
        CHECK(d->min_ms == doctest::Approx(1.0));
        CHECK(late.accounting_gaps() == 1);
    }

    SUBCASE("a wrong tree goes NEGATIVE rather than clamping to a tidy zero") {
        // A child that is not really inside its parent is a modelling error, and max(0, …) would
        // launder it into a number that looks like perfect attribution. `min_ms` is where it shows.
        PerfReport bad;
        bad.declare_accounting("frame", {"not-inside"});
        bad.observe("not-inside", 12.0);
        bad.observe_frame(0, 10.0);

        const auto d = bad.distribution("frame.unaccounted");
        REQUIRE(d.has_value());
        CHECK(d->min_ms == doctest::Approx(-2.0));
        CHECK(bad.accounting_gaps() == 0); // the part WAS there; the tree is what is wrong
    }

    SUBCASE("zones qualify as parts without any per-frame plumbing of their own") {
        PerfReport z;
        z.declare_accounting("frame", {"stage.per_frame"});
        z.observe_zone("stage", 3.0);
        z.observe_zone("stage", 2.0); // twice in one frame — the per-call/per-frame split (m17.3b)
        z.observe_frame(0, 10.0);
        const auto d = z.distribution("frame.unaccounted");
        REQUIRE(d.has_value());
        CHECK(d->max_ms == doctest::Approx(5.0));
        CHECK(z.accounting_gaps() == 0);
    }
}

TEST_CASE("accounting: the gate FAILS on the last committed block report (m17.3c)") {
    // The falsification, run against the real artifact rather than an invented one. The
    // 2026-08-30 block baseline is the file ADR-0041 quotes throughout — `frame` p99 35.598 over
    // 600 frames — and it contains no residual timeline, because none existed when it was written.
    //
    // Nothing new was needed to make that fail: a rule naming a timeline nobody recorded is
    // `Missing`, and `Missing` is a failure for the same reason a work budget's is. So the gate
    // this brick adds cannot pass on a report that predates the instrumentation — which is exactly
    // the property that makes it a gate rather than a decoration.
    const std::string path = std::string(RIME_PERF_BASELINE_DIR) +
                             "/2026-08-30-99-the-block-nvidia-geforce-rtx-3060.json";
    PerfReport committed;
    std::string error;
    REQUIRE_MESSAGE(PerfReport::load_file(path, committed, error), error);
    // The file is the one the ADR quotes — assert that before asserting anything about it, or a
    // renamed baseline would make every check below pass over the wrong numbers.
    REQUIRE(committed.distribution("frame").has_value());
    CHECK(committed.distribution("frame")->count == 600);
    CHECK(committed.distribution("frame")->p99_ms == doctest::Approx(35.598));

    PerfGate gate;
    gate.at_most("frame.unaccounted", PerfStat::P99, 1.0)
        .at_most("sim.collapse", PerfStat::Max, 12.0);
    const PerfGate::Result result = gate.check(committed);
    CHECK_FALSE(result.ok());
    REQUIRE(result.violations.size() == 2);
    CHECK(result.violations[0].outcome == PerfOutcome::Missing);
    CHECK(result.violations[1].outcome == PerfOutcome::Missing);

    SUBCASE("and an old report still LOADS — the new worst-frame field is optional on read") {
        // The one optional field in an otherwise strict reader. Absent means "this file predates
        // m17.3c", which is a true statement about the file rather than a guess about a number;
        // making it required would have meant bumping the schema and orphaning every baseline.
        CHECK(committed.worst_frame().zones.empty());
        CHECK(committed.worst_frame().ms > 0.0);
    }

    SUBCASE("the same shape, recorded through the new path, breaches QUANTITATIVELY") {
        // `Missing` proves the rule fires on an old file; it does not prove the number means
        // anything. So replay that file's shape — ~35.6 ms frames with a sim and a render half
        // that do not add up — and watch the residual itself breach.
        PerfReport replay;
        replay.declare_accounting("frame", {"sim.block", "frame.render"});
        for (int i = 0; i < 200; ++i) {
            replay.observe("sim.block", 25.0);
            replay.observe("frame.render", 8.0);
            replay.observe_frame(static_cast<std::uint64_t>(i), 35.6);
        }
        PerfGate quantitative;
        quantitative.at_most("frame.unaccounted", PerfStat::P99, 1.0);
        const PerfGate::Result r2 = quantitative.check(replay);
        REQUIRE(r2.violations.size() == 1);
        CHECK(r2.violations[0].outcome == PerfOutcome::Breach);
        CHECK(r2.violations[0].value_ms == doctest::Approx(2.6));
    }
}

TEST_CASE("the worst frame carries its CPU story, not only its GPU one (m17.3c)") {
    // The worst frame is the one frame a human opens after a gate fails, and it used to arrive
    // with a per-pass GPU breakdown and no answer to "what was the CPU doing" — because the zone
    // totals are zeroed at every frame boundary and the worst-frame check ran after the flush.
    PerfReport r;
    const PassTiming passes[] = {{"forward-pbr", 2.0}};

    r.observe_zone("physics.solve", 1.0);
    r.observe_frame(0, 5.0, passes);

    r.observe_zone("physics.solve", 7.0);
    r.observe_zone("physics.contacts", 3.0);
    r.observe_frame(1, 40.0, passes); // the worst

    r.observe_zone("physics.solve", 1.0);
    r.observe_frame(2, 5.0, passes);

    CHECK(r.worst_frame().index == 1);
    REQUIRE(r.worst_frame().zones.size() == 2);
    double solve = 0.0;
    for (const rime::core::ZoneTotal& z : r.worst_frame().zones) {
        if (z.name == "physics.solve")
            solve = z.ms;
    }
    // 7.0, the worst frame's own total — not 1.0 from a later frame and not 9.0 for the run.
    CHECK(solve == doctest::Approx(7.0));

    SUBCASE("and survives the round trip to a committed report") {
        const std::string json = r.to_json();
        PerfReport back;
        std::string error;
        REQUIRE_MESSAGE(PerfReport::parse(json, back, error), error);
        REQUIRE(back.worst_frame().zones.size() == 2);
        CHECK(back.to_json() == json);
    }
}

TEST_CASE("the committed JSON round-trips exactly") {
    const PerfReport r = make_report();
    const std::string first = r.to_json();

    PerfReport parsed;
    std::string error;
    REQUIRE_MESSAGE(PerfReport::parse(first, parsed, error), error);

    // Serialization is idempotent: write -> parse -> write reproduces the file byte for byte. That
    // is the strongest statement available here, because it covers every field at once — including
    // the ones no assertion below happens to name.
    CHECK(parsed.to_json() == first);

    CHECK(parsed.machine().gpu == "NVIDIA GeForce RTX 3060");
    CHECK(parsed.run().commit == "abc1234");
    REQUIRE(parsed.distribution("frame").has_value());
    CHECK(parsed.distribution("frame")->count == 200);
    CHECK(parsed.ledger_value_or("draws.submitted") == 1200);

    SUBCASE("ledger counters survive as exact 64-bit integers, not as doubles") {
        // The same 2^64-1 case the work ledger's own test pins, carried through the report's
        // reader: a JSON number parsed into a double loses the low bits of a byte counter, and it
        // would do so silently, in a file nobody re-reads by hand.
        PerfReport big = make_report();
        WorkLedger ledger;
        ledger.set("net.bytes_sent", 18446744073709551615ULL);
        big.set_ledger(ledger);

        PerfReport back;
        std::string err;
        REQUIRE_MESSAGE(PerfReport::parse(big.to_json(), back, err), err);
        CHECK(back.ledger_value_or("net.bytes_sent") == 18446744073709551615ULL);
    }
}

TEST_CASE("the reader refuses malformed or foreign reports instead of defaulting") {
    PerfReport out;
    std::string error;

    CHECK_FALSE(PerfReport::parse("{", out, error));
    CHECK_FALSE(error.empty());

    CHECK_FALSE(PerfReport::parse(R"({"schema":99,"run":{}})", out, error));
    CHECK(error.find("schema") != std::string::npos);

    SUBCASE("a missing required field is an error, never a zero") {
        // The whole report minus `distributions`. A lenient reader would hand back a report with
        // no timelines, every gate rule would then report Missing... which is at least loud. The
        // dangerous version is a reader that invents 0.0 for a percentile, so the contract is:
        // absent is an error, full stop.
        const std::string text = R"({"schema":1,
            "run":{"sample":"s","commit":"c","date":"d"},
            "machine":{"gpu":"g","driver":"d","os":"linux","build":"Release","sanitizer":"off",
                       "preset":"p","width":1,"height":1},
            "passes":{},"worst_frame":{"index":0,"ms":0.0,"passes":{}},"ledger":{}})";
        CHECK_FALSE(PerfReport::parse(text, out, error));
        CHECK(error.find("distributions") != std::string::npos);
    }

    SUBCASE("a percentile of the wrong type is an error too") {
        PerfReport r = make_report();
        std::string text = r.to_json();
        const std::size_t at = text.find("\"p99_ms\": ");
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string("\"p99_ms\": ").size(), "\"p99_ms\": \"");
        CHECK_FALSE(PerfReport::parse(text, out, error));
    }
}

TEST_CASE("the gate fails on an absolute breach, and says which frame did it") {
    const PerfReport r = make_report();

    PerfGate gate;
    gate.at_most("frame", PerfStat::Max, 16.6).require_samples("frame", 100);

    const PerfGate::Result result = gate.check(r);
    CHECK_FALSE(result.ok());
    REQUIRE(result.violations.size() == 1);
    CHECK(result.violations[0].outcome == PerfOutcome::Breach);
    CHECK(result.violations[0].value_ms == doctest::Approx(20.0));
    CHECK(PerfGate::format(result).find("frame max") != std::string::npos);

    SUBCASE("the same run passes a budget it fits") {
        PerfGate ok_gate;
        ok_gate.at_most("frame", PerfStat::P99, 16.6).at_most("frame", PerfStat::Max, 33.0);
        CHECK(ok_gate.check(r).ok());
    }
}

TEST_CASE("a rule naming a timeline nobody recorded FAILS rather than passing vacuously") {
    const PerfReport r = make_report();

    PerfGate gate;
    gate.at_most("sim.tick", PerfStat::P99, 6.0); // this sample never ticked a simulation

    const PerfGate::Result result = gate.check(r);
    CHECK_FALSE(result.ok());
    REQUIRE(result.violations.size() == 1);
    CHECK(result.violations[0].outcome == PerfOutcome::Missing);
    // The message has to say the rule could never have fired, because "sim.tick: ok" and
    // "sim.tick: unmeasured" are indistinguishable in a log otherwise.
    CHECK(PerfGate::format(result).find("NOT RECORDED") != std::string::npos);
}

TEST_CASE("too few samples fails: a percentile over 12 frames is not evidence") {
    PerfReport r;
    for (std::uint64_t i = 0; i < 12; ++i)
        r.observe_frame(i, 4.0);

    PerfGate gate;
    gate.require_samples("frame", 200);

    const PerfGate::Result result = gate.check(r);
    CHECK_FALSE(result.ok());
    REQUIRE(result.violations.size() == 1);
    CHECK(result.violations[0].outcome == PerfOutcome::TooFewSamples);
    CHECK(result.violations[0].count == 12);
}

TEST_CASE("the vacuity guard: a fast run that did no work is not a pass") {
    // The scene rendered in 0.1 ms per frame, which is wonderful news until you notice the ledger
    // says it submitted no draws. m11.7's lesson, applied to a stopwatch.
    PerfReport empty_scene;
    for (std::uint64_t i = 0; i < 200; ++i)
        empty_scene.observe_frame(i, 0.1);
    WorkLedger ledger;
    ledger.set("draws.submitted", 0);
    empty_scene.set_ledger(ledger);

    PerfGate gate;
    gate.at_most("frame", PerfStat::P99, 16.6);
    gate.work().at_least("draws.submitted", 1);

    const PerfGate::Result result = gate.check(empty_scene);
    CHECK(result.violations.empty()); // it really was fast
    REQUIRE(result.work_violations.size() == 1);
    CHECK(result.work_violations[0].outcome == BudgetOutcome::Breach);
    CHECK_FALSE(result.ok()); // …and that is not a pass

    SUBCASE("a counter the run never recorded fails the same way") {
        PerfGate strict;
        strict.work().at_least("physics.awake_peak", 1);
        const PerfGate::Result r2 = strict.check(empty_scene);
        REQUIRE(r2.work_violations.size() == 1);
        CHECK(r2.work_violations[0].outcome == BudgetOutcome::Missing);
    }
}

TEST_CASE("regression against a committed baseline") {
    const PerfReport baseline = make_report(20.0);
    PerfGate gate;
    gate.at_most("frame", PerfStat::Max, 33.0).max_regression(0.10);

    SUBCASE("a 50% slower run fails even though it is still under the absolute budget") {
        const PerfReport slower = make_report(30.0);
        const PerfGate::Result result = gate.check(slower, &baseline);
        CHECK(result.baseline == BaselineStatus::Compared);
        REQUIRE(result.violations.size() == 1);
        CHECK(result.violations[0].outcome == PerfOutcome::Regressed);
        CHECK(result.violations[0].baseline_ms == doctest::Approx(20.0));
        CHECK(result.violations[0].limit_ms == doctest::Approx(22.0));
    }

    SUBCASE("noise inside the tolerance passes") {
        const PerfReport noisy = make_report(21.0);
        CHECK(gate.check(noisy, &baseline).ok());
    }

    SUBCASE("a faster run passes, and so does an equal one") {
        const PerfReport faster = make_report(12.0);
        CHECK(gate.check(faster, &baseline).ok());
    }

    SUBCASE("a baseline from another machine is NOT compared, and says so out loud") {
        PerfReport other = make_report(1.0);
        MachineFingerprint m = other.machine();
        m.gpu = "llvmpipe (LLVM 19)"; // the CI software rasterizer: comparing would be nonsense
        other.set_machine(m);

        const PerfReport slower = make_report(30.0);
        const PerfGate::Result result = gate.check(slower, &other);
        CHECK(result.baseline == BaselineStatus::FingerprintMismatch);
        CHECK(result.violations.empty()); // no regression claim can be made…
        CHECK(PerfGate::format(result).find("fingerprint-mismatch") != std::string::npos);
    }

    SUBCASE("a sanitizer build never compares against a clean baseline") {
        PerfReport instrumented = make_report(60.0);
        MachineFingerprint m = instrumented.machine();
        m.sanitizer = "address";
        instrumented.set_machine(m);
        const PerfGate::Result result = gate.check(instrumented, &baseline);
        CHECK(result.baseline == BaselineStatus::FingerprintMismatch);
    }

    SUBCASE("no baseline at all is reported, never silently treated as a pass") {
        const PerfGate::Result result = gate.check(baseline, nullptr);
        CHECK(result.baseline == BaselineStatus::NotProvided);
        CHECK(result.baseline_note.find("establishes one") != std::string::npos);
    }
}

TEST_CASE("a fingerprint compares the machine, not the run") {
    MachineFingerprint a;
    a.gpu = "RTX 3060";
    a.driver = "610.43.03";
    a.os = "linux";
    a.build = "Release";
    a.sanitizer = "off";
    a.preset = "p";
    a.width = 1920;
    a.height = 1080;

    MachineFingerprint b = a;
    CHECK(a.comparable_to(b));

    SUBCASE("resolution is part of it — a 540p run says nothing about a 1080p budget") {
        b.height = 540;
        CHECK_FALSE(a.comparable_to(b));
    }

    SUBCASE("the driver is part of it — a driver update is a new baseline, not a regression") {
        b.driver = "615.00.00";
        CHECK_FALSE(a.comparable_to(b));
    }

    SUBCASE("commit and date are NOT part of it — they are what differs between compared runs") {
        // Nothing to flip: RunInfo is not consulted by comparable_to at all. Asserted here as the
        // statement of intent, so a later refactor that folds the commit into the fingerprint
        // breaks a test instead of quietly disabling every comparison.
        PerfReport r1 = make_report();
        PerfReport r2 = make_report();
        RunInfo other;
        other.sample = "11-lit-rooms";
        other.commit = "deadbee";
        other.date = "2027-01-01";
        r2.set_run(other);
        CHECK(r1.machine().comparable_to(r2.machine()));
    }
}
