// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rime/core/diagnostics/work_ledger.hpp"

// The PERF REPORT — how FAST the work ran, on one named machine, honestly.
//
// The other half of ADR-0035 §2. The work ledger next door answers "how much work did the frame
// do" in integers every machine computes identically, so lavapipe gates it in CI forever. This
// file answers the question the ledger deliberately cannot: *how long did it take*. That answer is
// a property of one GPU, one driver and one build, so it does not belong in CI at all — it belongs
// in a fingerprinted report, committed to `docs/perf/`, compared only against reports from a
// machine configured the same way.
//
// Four decisions carry the design:
//
//   1. **A distribution, never a mean.** There is no `mean_ms` field anywhere below, and that is
//      deliberate rather than an oversight — the same move the ledger makes by refusing to store a
//      double. Destruction is bursty, and the fracture tick is precisely the frame that must stay
//      smooth; a hitch storm of ten 40 ms frames in a 600-frame run moves the mean by half a
//      millisecond and vanishes. p99 and max cannot hide it. Making the mean unrepresentable means
//      nobody can gate on it by accident later.
//
//   2. **Percentiles are NEAREST-RANK**, i.e. an order statistic of the samples actually observed:
//      p99 of 600 frames is the 594th-slowest frame, a duration that genuinely occurred. The
//      interpolating definition would invent a number between two real frames, which for "how bad
//      does it get" is the wrong flavour of answer. One consequence to expect rather than be
//      surprised by: with fewer than 100 samples, p99 IS max, so a report over 40 frames is not
//      evidence about tail latency — which is what `PerfGate::require_samples` exists to enforce.
//
//   3. **A fingerprint decides what may be compared.** Two reports are comparable when the
//      machine, the driver, the resolution, the preset, the build config AND the sanitizer agree —
//      never merely because they measure the same sample. Commit and date deliberately do NOT
//      participate: they are what changes between the two runs being compared. The sanitizer is in
//      there because an ASan build is three times slower and would otherwise silently "regress"
//      against a clean baseline (the #125 lesson: the configuration you think you are running is
//      not necessarily the one the binary was built with).
//
//   4. **The run carries its own work ledger, and the gate reads it.** A fast run on a scene that
//      did no work is not a pass, it is a broken measurement — the vacuity guard m11.7 learned the
//      hard way, applied to performance. `PerfGate::work()` is a full `WorkBudget` over the
//      embedded ledger, so "the frame was quick" must be accompanied by "…and it drew the scene".
namespace rime::core {

// Which order statistic a rule reads. No `Mean` — see decision 1 above.
enum class PerfStat : std::uint8_t { P50, P95, P99, Max };

[[nodiscard]] std::string_view perf_stat_name(PerfStat stat);

// A summarized timeline, in milliseconds. `count` is part of the data, not bookkeeping: a
// percentile without its sample size is not a claim about anything (see decision 2).
struct Distribution {
    std::size_t count = 0;
    double min_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;

    [[nodiscard]] double stat(PerfStat s) const;
};

// Collects raw durations and summarizes them. Kept separate from `Distribution` because the raw
// samples are large and transient while the summary is small and committed to git.
class DurationSamples {
public:
    void reserve(std::size_t n) { ms_.reserve(n); }

    void add(double ms) { ms_.push_back(ms); }

    [[nodiscard]] std::size_t count() const noexcept { return ms_.size(); }

    [[nodiscard]] const std::vector<double>& raw() const noexcept { return ms_; }

    // Nearest-rank percentiles over a sorted copy. Sorting a copy rather than keeping the vector
    // sorted keeps `add` O(1) on the measured path — the harness must not perturb what it measures.
    [[nodiscard]] Distribution summarize() const;

private:
    std::vector<double> ms_;
};

// One render pass in one frame. Names are owned (not the `string_view` the ledger uses) because a
// report outlives the RenderGraph it was read from, and is serialized after that graph has reset.
struct PassTiming {
    std::string name;
    double ms = 0.0;
};

// One render pass across the whole run. p50 and max for the same reason the frame timeline has
// them and no mean: a pass that is usually cheap and occasionally catastrophic is the interesting
// case, and averaging is how you fail to notice it.
struct PassCost {
    std::string name;
    double p50_ms = 0.0;
    double max_ms = 0.0;
};

// The single worst frame of the run, with its per-pass breakdown — the one frame a human actually
// wants to look at after a gate fails, because "p99 went up" does not say which pass did it.
struct WorstFrame {
    std::uint64_t index = 0;
    double ms = 0.0;
    std::vector<PassTiming> passes;
};

// What must agree before two reports may be compared. See decision 3.
struct MachineFingerprint {
    std::string gpu;       // adapter name, e.g. "NVIDIA GeForce RTX 3060"
    std::string driver;    // driver/API version string as the RHI reports it
    std::string os;        // "linux" | "macos" | "windows"
    std::string build;     // CMake config: "Release", "Debug", "RelWithDebInfo"
    std::string sanitizer; // "off" | "address" | "thread"
    std::string preset;    // what the sample was configured to do ("all-lighting-gates")
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Every field above must match. Note what is NOT here: the commit and the date, which are the
    // things that differ between a baseline and the run being judged against it.
    [[nodiscard]] bool comparable_to(const MachineFingerprint& other) const;

    // A one-line human summary, used in the "why was no baseline comparison made" message.
    [[nodiscard]] std::string describe() const;

    // Fills the fields the engine can know by itself: `os`, `build` and `sanitizer` come from how
    // this translation unit was compiled, so they describe the binary rather than the intent. The
    // caller supplies `gpu`/`driver` (from the RHI adapter) and the extent/preset (from its own
    // configuration).
    [[nodiscard]] static MachineFingerprint detect();
};

// Which run this was — metadata for a human reading `docs/perf/` a year later, and explicitly not
// part of comparability.
struct RunInfo {
    std::string sample; // "11-lit-rooms"
    std::string commit; // git SHA; see `detect_commit()` below for why it is not read from git here
    std::string date;   // ISO-8601 date (UTC) the report was written

    // `date` from the system clock, and `commit` from the RIME_PERF_COMMIT environment variable
    // when set. The commit is not obtained by shelling out to git on purpose: the engine does not
    // spawn processes to describe itself, and a compile-time bake goes stale the moment you commit
    // without reconfiguring — which is exactly when a wrong answer would be most convincing. The
    // wrapper (`scripts/perf.sh`) sets the variable; an empty commit is recorded as "unknown", not
    // guessed.
    [[nodiscard]] static RunInfo detect(std::string_view sample_name);
};

// One run's measurements: named timelines, the per-pass table, the worst frame, the fingerprint,
// and the run's own work ledger.
//
// The report OWNS every string it holds. `WorkLedger` borrows its counter names because it is a
// hot-path recorder fed by string literals; a report is a serialization artifact that must survive
// being parsed back from a file, where the names live in a buffer the parse discards. Copying at
// the boundary is the price of that, and it is paid once per run.
class PerfReport {
public:
    // ── Recording ────────────────────────────────────────────────────────────────────────────
    // One rendered frame: its wall-clock cost, and optionally the per-pass GPU breakdown resolved
    // for it. Feeds the `frame` timeline, the per-pass table and the worst-frame record in a
    // single call, because all three describe the same frame and splitting them into three calls
    // invites a caller to update two of the three.
    void observe_frame(std::uint64_t index, double ms, std::span<const PassTiming> passes = {});

    // One sample on any other timeline: "sim_tick", "frame.collapse", "gpu.total".
    void observe(std::string_view timeline, double ms);

    void set_machine(MachineFingerprint machine) { machine_ = std::move(machine); }

    void set_run(RunInfo run) { run_ = std::move(run); }

    // Copies the ledger's counters into owned storage. Call once, after the run.
    void set_ledger(const WorkLedger& ledger);

    // ── Reading ──────────────────────────────────────────────────────────────────────────────
    // `nullopt` when nothing was ever observed under that name — absent and zero are different
    // answers, and every consumer here is required to tell them apart. Returned by value rather
    // than as a pointer into the report: summarizing is lazy, and handing out a pointer to a cache
    // that a later `observe()` invalidates is a dangling-reference bug waiting for its first
    // long-running caller.
    [[nodiscard]] std::optional<Distribution> distribution(std::string_view timeline) const;

    [[nodiscard]] std::vector<std::string_view> timelines() const;

    // The per-pass table, summarized on demand for the same reason.
    [[nodiscard]] std::vector<PassCost> passes() const;

    [[nodiscard]] const WorstFrame& worst_frame() const noexcept { return worst_; }

    [[nodiscard]] const MachineFingerprint& machine() const noexcept { return machine_; }

    [[nodiscard]] const RunInfo& run() const noexcept { return run_; }

    // The embedded ledger, as owned name/value pairs. Named `ledger_counters` rather than `ledger`
    // because it is deliberately not a `WorkLedger` — see the ownership note above.
    [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>&
    ledger_counters() const noexcept {
        return ledger_;
    }

    [[nodiscard]] bool ledger_contains(std::string_view name) const;

    [[nodiscard]] std::uint64_t ledger_value_or(std::string_view name,
                                                std::uint64_t fallback = 0) const;

    // ── Serialization ────────────────────────────────────────────────────────────────────────
    // The committed form. Durations are written to three decimals — a run-to-run difference in the
    // fourth is thermal noise, and rounding keeps a `git diff` of two reports readable. Counter
    // values are written as exact integers, never through a double, so a 2^64-1 byte counter
    // survives the round trip (the property the ledger's own test pins down).
    [[nodiscard]] std::string to_json(int indent = 2) const;

    // Strict reader for the format `to_json` writes: a missing or mistyped required field is an
    // error, never a default. That strictness is load-bearing rather than fastidious — a reader
    // that quietly yielded 0.0 for a field it could not find would make the regression gate
    // compare against zero and pass everything, which is the "gate that cannot fail" the work
    // ledger's `Missing` outcome exists to prevent.
    //
    // Deliberately scoped to this schema: the JSON parsing behind it is an implementation detail
    // of this file, not a general facility the engine offers. If a second caller ever needs JSON,
    // promote it then, with its own tests.
    [[nodiscard]] static bool parse(std::string_view text, PerfReport& out, std::string& error);

    // Convenience: read `path`, then `parse`. False (with `error` set) when the file is absent —
    // which callers must distinguish from "the baseline said we are fine".
    [[nodiscard]] static bool
    load_file(const std::string& path, PerfReport& out, std::string& error);

private:
    // A timeline is either MEASURED (raw samples, summarized on demand) or PARSED (a summary read
    // back from a committed report, with no samples behind it). Both answer `distribution()`; only
    // a measured one can grow.
    struct Timeline {
        std::string name;
        DurationSamples samples;
        Distribution parsed;
        bool measured = false;
    };

    struct PassAccumulator {
        std::string name;
        DurationSamples samples;
    };

    [[nodiscard]] Timeline& timeline_for(std::string_view name);

    std::vector<Timeline> timelines_;
    std::vector<PassAccumulator> pass_acc_; // recording side
    std::vector<PassCost> parsed_passes_;   // parse side
    WorstFrame worst_;
    MachineFingerprint machine_;
    RunInfo run_;
    std::vector<std::pair<std::string, std::uint64_t>> ledger_;
};

// Routes every RIME_PROFILE_ZONE that closes while this object lives into a same-named timeline on
// a report — which is how the per-stage CPU breakdown (`sim.tick`, `sim.schedule`, `frame.submit`,
// …) reaches the hardware report without any sample writing per-stage plumbing of its own.
//
// RAII because installing a zone sink is a global side effect, and one that outlived its report
// would write into a destroyed object. Two constraints, documented rather than defended against:
// the collector must outlive every zone that could still fire (today all zones are on the main
// thread, between `Application` stages), and it must not be nested with another collector — the
// sink is a single global slot, so the inner one would silently replace the outer.
class ZoneTimelines {
public:
    explicit ZoneTimelines(PerfReport& report);
    ~ZoneTimelines();

    ZoneTimelines(const ZoneTimelines&) = delete;
    ZoneTimelines& operator=(const ZoneTimelines&) = delete;
    ZoneTimelines(ZoneTimelines&&) = delete;
    ZoneTimelines& operator=(ZoneTimelines&&) = delete;

    // Stop collecting early (the destructor calls this). Idempotent — useful when a run wants to
    // summarize while the app it measured is still alive.
    void stop();

private:
    PerfReport* report_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
// The gate — the policy half, kept apart from the measurement half exactly as `WorkBudget` is
// kept apart from `WorkLedger`.

// Why a rule failed. `Missing` and `TooFewSamples` are failures rather than skips for the reason
// that runs through this whole engine: a rule that cannot fire is indistinguishable in a log from
// a rule that passed.
enum class PerfOutcome : std::uint8_t {
    Breach,        // over an absolute budget
    Regressed,     // within budget, but materially worse than the committed baseline
    Missing,       // the rule names a timeline nobody recorded
    TooFewSamples, // the timeline exists but is too short for the statistic to mean anything
};

struct PerfViolation {
    std::string timeline;
    PerfStat stat = PerfStat::P99;
    PerfOutcome outcome = PerfOutcome::Breach;
    double value_ms = 0.0;
    double limit_ms = 0.0;    // for Regressed: baseline * (1 + tolerance)
    double baseline_ms = 0.0; // for Regressed only
    std::size_t count = 0;
    std::size_t required_count = 0;
};

// What happened to the baseline comparison. This is reported rather than inferred because a
// comparison that silently did not happen is the most comfortable way for a perf gate to rot:
// every skip path gets a name and gets printed (CLAUDE.md's counting rule, applied to a
// comparison instead of to a packet).
enum class BaselineStatus : std::uint8_t {
    NotProvided,         // no committed report was passed in — the first run on a machine
    FingerprintMismatch, // a report exists, but for a different machine/build: comparing is invalid
    Compared,            // the regression rules actually ran
};

[[nodiscard]] std::string_view baseline_status_name(BaselineStatus status);

class PerfGate {
public:
    // An absolute ceiling: `at_most("frame", PerfStat::P99, 16.6)`.
    PerfGate& at_most(std::string_view timeline, PerfStat stat, double limit_ms);

    // A floor on sample count, so a percentile is backed by enough frames to mean something. A
    // timeline with fewer samples FAILS rather than passing on a lucky short run.
    PerfGate& require_samples(std::string_view timeline, std::size_t min_count);

    // Relative regression tolerance against the committed baseline, applied to every statistic an
    // `at_most` rule already names — one list of statistics, two ways to fail. 0.10 means "more
    // than 10% slower than the baseline fails, even though it is still under the absolute budget",
    // which is the check that catches a slow slide long before it crosses a ceiling.
    PerfGate& max_regression(double relative);

    // The vacuity guard: a `WorkBudget` over the report's embedded ledger. Add floors here ("the
    // run drew something", "the wall actually broke") so a fast run on an empty scene cannot pass.
    [[nodiscard]] WorkBudget& work() noexcept { return work_; }

    [[nodiscard]] const WorkBudget& work() const noexcept { return work_; }

    struct Result {
        std::vector<PerfViolation> violations;
        std::vector<BudgetViolation> work_violations;
        BaselineStatus baseline = BaselineStatus::NotProvided;
        std::string baseline_note; // why, when `baseline` is not Compared

        [[nodiscard]] bool ok() const noexcept {
            return violations.empty() && work_violations.empty();
        }
    };

    // `baseline` may be null: the first run on a new machine has nothing to compare against, and
    // that is a legitimate state — reported as `NotProvided`, never as a pass.
    [[nodiscard]] Result check(const PerfReport& report,
                               const PerfReport* baseline = nullptr) const;

    // Multi-line, one finding per line, ready for stderr ahead of a non-zero exit.
    [[nodiscard]] static std::string format(const Result& result);

private:
    struct Rule {
        std::string timeline;
        PerfStat stat = PerfStat::P99;
        double limit_ms = 0.0;
    };

    struct SampleRule {
        std::string timeline;
        std::size_t min_count = 0;
    };

    std::vector<Rule> rules_;
    std::vector<SampleRule> sample_rules_;
    WorkBudget work_;
    double regression_ = -1.0; // < 0 => no regression check requested
};

} // namespace rime::core
