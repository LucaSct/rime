// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

// One named CPU cost inside one frame: a profile zone's total for that frame. Deliberately its
// own type rather than a reuse of `PassTiming` — a pass is GPU time read from a timestamp pair, a
// zone is CPU wall time from a stopwatch, and a struct whose name lies about which one it holds is
// how the two get compared as if they were the same clock.
struct ZoneTotal {
    std::string name;
    double ms = 0.0;
};

// The single worst frame of the run — the one frame a human actually wants to look at after a gate
// fails, because "p99 went up" does not say what did it.
//
// It carries BOTH breakdowns (m17.3c). It used to carry only `passes`, which meant the frame a
// reader opens after a failure had its GPU story and none of its CPU one — and since the zone
// totals are zeroed at every frame boundary, the information was gone by the time anyone asked.
// Captured before the flush, and only when this frame actually takes the record, so the cost is
// paid a handful of times per run rather than 600.
struct WorstFrame {
    std::uint64_t index = 0;
    double ms = 0.0;
    std::vector<PassTiming> passes;
    std::vector<ZoneTotal> zones; // non-zero zone totals only; a zero explains nothing here
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

    // One CLOSED PROFILE ZONE, which is not the same shape of measurement as `observe` (m17.3b).
    //
    // A zone fires once per SCOPE ENTRY, and the scopes that matter run many times per frame: a
    // frame steps the simulation several times, and each step runs every physics stage. So the
    // timeline a zone feeds is a distribution over CALLS — `sim.tick p99` is "the 99th-percentile
    // tick", which is exactly right against ADR-0035's ratified per-tick budget and exactly wrong
    // for the question M17 has to answer, which is where a FRAME's 35.6 ms went. Multiplying a
    // per-call percentile by a call count does not give a per-frame percentile.
    //
    // So a zone is recorded twice, under two names that cannot be confused:
    //
    //   `<name>`            — every call, unchanged, still the ratified per-tick meaning
    //   `<name>.per_frame`  — the SUM of that zone's calls within one frame, one sample per frame
    //
    // The per-frame half is flushed by `observe_frame`, which is the only thing in this class that
    // knows where a frame ends. A run that never calls `observe_frame` (a pure-sim app) simply
    // accumulates and never flushes: bounded by the number of distinct zone names, and reported by
    // nobody, which is the honest outcome for a run that has no frames.
    void observe_zone(std::string_view name, double ms);

    // ── Accounting: "the parts account for the whole", made checkable (m17.3c) ────────────────
    //
    // A frame is attributable only if the named parts add up to it, and nothing in this report
    // could state that relationship. It also cannot be recovered afterwards from the committed
    // summaries, for two reasons that are worth knowing before anyone tries: percentiles are not
    // subadditive in either direction — p99 of a sum is neither the sum of the p99s nor bounded by
    // it, because the frames that are worst for one part need not be worst for another — and
    // decision 1 above deliberately makes a mean unrepresentable, so the one summary-level check
    // that WOULD have been sound is the one this schema refuses to store. The residual therefore
    // has to be computed per frame, at record time, or never.
    //
    //     declare_accounting("frame", {"sim.block", "frame.render"});
    //
    // says the frame's wall clock should be explained by those two, and makes `observe_frame`
    // record `frame.unaccounted = frame − (sim.block + frame.render)` as a timeline of its own,
    // one sample per frame. Giving the remainder a NAME is the whole point: a name is what a gate
    // can hold (`at_most("frame.unaccounted", …)`), what a percentile can be taken over, and what
    // a human reads in `docs/perf/` — where an arithmetic identity nobody evaluates would be a
    // comment.
    //
    // Two rules for children. Each must be recorded BEFORE the `observe_frame` that closes the
    // frame — a `<zone>.per_frame` total qualifies automatically, since the flush banks it first —
    // and each must genuinely be nested inside its parent, or the residual is measuring the
    // difference between two unrelated things. A child that recorded nothing this frame counts
    // zero and increments `accounting_gaps()`: the residual is then inflated by exactly the
    // missing part, which fails loudly rather than quietly, and the counter says it happened.
    //
    // The residual is NOT clamped at zero. Negative means the declared tree is wrong — a child
    // that is not inside its parent, or double-counted — and a max() would launder a modelling
    // error into a tidy zero. `Distribution::min_ms` is where it shows.
    void declare_accounting(std::string_view parent,
                            std::initializer_list<std::string_view> children);

    // How many times a declared child contributed nothing because it had no sample for that frame.
    // Non-zero means every residual above is an overestimate by an unknown amount — read it before
    // believing a residual, and print it beside them.
    [[nodiscard]] std::uint64_t accounting_gaps() const noexcept { return accounting_gaps_; }

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
        // Which frame index last wrote here, for accounting (m17.3c). "Was this recorded during
        // the frame now closing?" is the exact question, and it is not the same as comparing
        // sample counts: a zone discovered at frame 50 is one sample short of `frame` forever
        // afterwards while still being perfectly current.
        std::uint64_t last_frame = kNeverRecorded;
    };

    static constexpr std::uint64_t kNeverRecorded = ~std::uint64_t{0};

    struct AccountingRule {
        std::string parent;
        std::vector<std::string> children;
        std::string residual_name; // "<parent>.unaccounted", built once at declaration
    };

    // The value a timeline recorded for the frame now closing, or 0.0 with a counted gap.
    [[nodiscard]] double value_this_frame(const std::string& name);

    struct PassAccumulator {
        std::string name;
        DurationSamples samples;
    };

    [[nodiscard]] Timeline& timeline_for(std::string_view name);

    // The in-flight frame's zone totals (m17.3b). `total_ms` accumulates every close of that zone
    // since the last `observe_frame`; `per_frame_name` is `<name>.per_frame`, built once per name
    // rather than once per frame so the flush allocates nothing.
    // A frame in which a known zone never ran records a ZERO, not nothing. "This frame spent no
    // time in physics.solve" is a true and useful statement — it is how a skipped sim shows up —
    // and it keeps every per-frame timeline's sample count aligned with `frame`'s, which is the
    // precondition for ever comparing them frame-for-frame. Zones discovered late carry fewer
    // samples than `frame`; that asymmetry is visible in `count` rather than hidden.
    struct ZoneAccumulator {
        std::string name;
        std::string per_frame_name;
        double total_ms = 0.0;
    };

    std::vector<Timeline> timelines_;
    std::vector<PassAccumulator> pass_acc_;  // recording side
    std::vector<ZoneAccumulator> zone_acc_;  // recording side, flushed per frame
    std::vector<AccountingRule> accounting_; // recording side, evaluated per frame
    std::vector<PassCost> parsed_passes_;    // parse side
    std::uint64_t frames_closed_ = 0;        // how many observe_frame calls have completed
    std::uint64_t accounting_gaps_ = 0;
    bool accounting_gap_warned_ = false;
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
// would write into a destroyed object. One constraint is documented rather than defended against —
// the collector must not be nested with another, since the sink is a single global slot and the
// inner one would silently replace the outer.
//
// THE THREAD CONSTRAINT IS ENFORCED, not documented, because the failure mode is undefined
// behaviour and the guardrail it violates ("assume a data-parallel world") is one the engine takes
// seriously. `report_zone` invokes its sink WITH THE LOCK RELEASED, on purpose; `PerfReport` has
// no synchronization of its own; so a zone closing on a job-system worker would race on the
// report's vectors and, at the first reallocation, corrupt or crash the very numbers a milestone
// is deciding from. This collector therefore pins itself to the thread that constructed it and
// DROPS foreign zones — and counts them, because the rule that runs through this engine is that a
// skip nobody counted is indistinguishable from work that never happened.
//
// The pin itself is race-free by construction: `owner_` is written before `set_zone_sink`, which
// takes the sink mutex, and a foreign thread can only reach `on_zone` by taking that same mutex to
// fetch the sink — so the write happens-before every read of it.
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

    // Zones dropped because they closed on a thread other than this collector's. Non-zero means
    // the report is INCOMPLETE by exactly that many zone closes — read it, print it, and if it is
    // large the answer is a per-thread sink, not a lock around this one.
    [[nodiscard]] std::uint64_t foreign_zones() const noexcept {
        return foreign_.load(std::memory_order_relaxed);
    }

private:
    void on_zone(std::string_view name, double ms);

    PerfReport* report_;
    std::thread::id owner_;
    std::atomic<std::uint64_t> foreign_{0};
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
