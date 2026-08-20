// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/diagnostics/perf_report.hpp"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "rime/core/diagnostics/profile.hpp"

namespace rime::core {
namespace {

// The schema version written into every report and required on read. A reader that shrugged at an
// unknown version would be guessing about fields it has never seen; this file would rather stop.
constexpr int kSchemaVersion = 1;

// ── A minimal, strict JSON value tree ────────────────────────────────────────────────────────
//
// Deliberately private to this translation unit. The engine does not offer a JSON facility, and
// this is not the beginning of one — it exists because the perf gate must READ the last committed
// report, and a gate that compares against a silently-defaulted zero is a gate that always passes.
// Two properties matter more than generality:
//
//   * A number keeps its RAW TOKEN, and integers are converted with strtoull rather than through a
//     double. The work ledger's own test pins 2^64-1 round-tripping exactly; routing a byte
//     counter through a double's 53-bit mantissa would quietly break that here, in the one place
//     nobody would think to look.
//   * Anything unexpected is an error with a position, never a default.
struct JsonValue {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string text; // Number: the raw token. String: the decoded contents.
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key)
                return &v;
        }
        return nullptr;
    }

    [[nodiscard]] double as_double() const { return std::strtod(text.c_str(), nullptr); }

    [[nodiscard]] std::uint64_t as_uint64() const {
        return std::strtoull(text.c_str(), nullptr, 10);
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    bool parse(JsonValue& out) {
        skip_space();
        if (!value(out, 0))
            return false;
        skip_space();
        if (pos_ != s_.size())
            return fail("trailing content after the top-level value");
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    // A depth cap so a corrupt or hostile file cannot recurse this parser off the stack. Reports
    // are three levels deep; 32 is room to spare and still nowhere near a stack frame's worth.
    static constexpr int kMaxDepth = 32;

    bool fail(std::string_view what) {
        if (error_.empty())
            error_ = fmt::format("JSON at byte {}: {}", pos_, what);
        return false;
    }

    void skip_space() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view lit) {
        if (s_.compare(pos_, lit.size(), lit) != 0)
            return fail(fmt::format("expected '{}'", lit));
        pos_ += lit.size();
        return true;
    }

    bool string_value(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"')
            return fail("expected a string");
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size())
                return fail("string ends inside an escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    // \u00XX is all this writer ever emits (work_ledger.cpp escapes the C0 range
                    // that way). Anything above 0x7F would need real UTF-16 surrogate handling,
                    // which this parser refuses rather than mangles.
                    if (pos_ + 4 > s_.size())
                        return fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[pos_ + static_cast<std::size_t>(i)];
                        const unsigned digit = (h >= '0' && h <= '9')   ? unsigned(h - '0')
                                               : (h >= 'a' && h <= 'f') ? unsigned(h - 'a' + 10)
                                               : (h >= 'A' && h <= 'F') ? unsigned(h - 'A' + 10)
                                                                        : 16u;
                        if (digit == 16u)
                            return fail("bad hex digit in \\u escape");
                        code = code * 16 + digit;
                    }
                    pos_ += 4;
                    if (code > 0x7F)
                        return fail("non-ASCII \\u escape is not supported by this reader");
                    out += static_cast<char>(code);
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool number_value(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+'))
            ++pos_;
        bool digits = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
            digits = true;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                digits = true;
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+'))
                ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                ++pos_;
        }
        if (!digits)
            return fail("expected a number");
        out.type = JsonValue::Type::Number;
        out.text.assign(s_.substr(start, pos_ - start));
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth)
            return fail("nesting too deep");
        if (pos_ >= s_.size())
            return fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{':
                return object_value(out, depth);
            case '[':
                return array_value(out, depth);
            case '"':
                out.type = JsonValue::Type::String;
                return string_value(out.text);
            case 't':
                out.type = JsonValue::Type::Bool;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.type = JsonValue::Type::Bool;
                out.boolean = false;
                return literal("false");
            case 'n':
                out.type = JsonValue::Type::Null;
                return literal("null");
            default:
                return number_value(out);
        }
    }

    bool object_value(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        ++pos_; // '{'
        skip_space();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (!string_value(key))
                return false;
            skip_space();
            if (pos_ >= s_.size() || s_[pos_] != ':')
                return fail("expected ':' after an object key");
            ++pos_;
            skip_space();
            JsonValue child;
            if (!value(child, depth + 1))
                return false;
            out.object.emplace_back(std::move(key), std::move(child));
            skip_space();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}' in an object");
        }
    }

    bool array_value(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        ++pos_; // '['
        skip_space();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            JsonValue child;
            if (!value(child, depth + 1))
                return false;
            out.array.push_back(std::move(child));
            skip_space();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']' in an array");
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

// ── Reading helpers: absent or mistyped is an ERROR, never a default ─────────────────────────
bool need_object(const JsonValue& parent,
                 std::string_view key,
                 const JsonValue*& out,
                 std::string& error) {
    const JsonValue* v = parent.find(key);
    if (!v || v->type != JsonValue::Type::Object) {
        error = fmt::format("missing or non-object field '{}'", key);
        return false;
    }
    out = v;
    return true;
}

bool need_number(const JsonValue& parent, std::string_view key, double& out, std::string& error) {
    const JsonValue* v = parent.find(key);
    if (!v || v->type != JsonValue::Type::Number) {
        error = fmt::format("missing or non-numeric field '{}'", key);
        return false;
    }
    out = v->as_double();
    return true;
}

bool need_string(const JsonValue& parent,
                 std::string_view key,
                 std::string& out,
                 std::string& error) {
    const JsonValue* v = parent.find(key);
    if (!v || v->type != JsonValue::Type::String) {
        error = fmt::format("missing or non-string field '{}'", key);
        return false;
    }
    out = v->text;
    return true;
}

bool need_uint(const JsonValue& parent,
               std::string_view key,
               std::uint64_t& out,
               std::string& error) {
    const JsonValue* v = parent.find(key);
    if (!v || v->type != JsonValue::Type::Number) {
        error = fmt::format("missing or non-numeric field '{}'", key);
        return false;
    }
    out = v->as_uint64();
    return true;
}

// ── Writing helpers ──────────────────────────────────────────────────────────────────────────
// Three decimals: a run-to-run difference in the fourth is thermal noise, and rounding is what
// keeps `git diff` between two committed reports readable instead of a wall of churn.
std::string ms_text(double ms) {
    return fmt::format("{:.3f}", ms);
}

std::string escape(std::string_view s) {
    std::string out;
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
                    out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// A tiny pretty-printer. `indent < 0` emits one dense line (what a log wants); `indent >= 0` is
// the committed form.
struct Writer {
    std::string out;
    int indent = 2;
    int level = 0;

    void open(char brace) {
        out += brace;
        ++level;
    }

    void close(char brace, bool had_items) {
        --level;
        if (indent >= 0 && had_items)
            line();
        out += brace;
    }

    void line() {
        if (indent < 0)
            return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * level), ' ');
    }

    void item(bool& first) {
        if (!first)
            out += ',';
        first = false;
        line();
    }

    void key(std::string_view k) {
        out += '"';
        out += escape(k);
        out += indent >= 0 ? "\": " : "\":";
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────

std::string_view perf_stat_name(PerfStat stat) {
    switch (stat) {
        case PerfStat::P50:
            return "p50";
        case PerfStat::P95:
            return "p95";
        case PerfStat::P99:
            return "p99";
        case PerfStat::Max:
            return "max";
    }
    return "?";
}

std::string_view baseline_status_name(BaselineStatus status) {
    switch (status) {
        case BaselineStatus::NotProvided:
            return "not-provided";
        case BaselineStatus::FingerprintMismatch:
            return "fingerprint-mismatch";
        case BaselineStatus::Compared:
            return "compared";
    }
    return "?";
}

double Distribution::stat(PerfStat s) const {
    switch (s) {
        case PerfStat::P50:
            return p50_ms;
        case PerfStat::P95:
            return p95_ms;
        case PerfStat::P99:
            return p99_ms;
        case PerfStat::Max:
            return max_ms;
    }
    return 0.0;
}

Distribution DurationSamples::summarize() const {
    Distribution d;
    d.count = ms_.size();
    if (ms_.empty())
        return d;

    std::vector<double> sorted = ms_;
    std::sort(sorted.begin(), sorted.end());

    // NEAREST-RANK: the value at ceil(q*N), 1-based — a duration that actually occurred, rather
    // than an interpolation between two frames that invents a number nothing measured.
    const auto rank = [&sorted](double q) {
        const auto n = static_cast<double>(sorted.size());
        auto idx = static_cast<std::size_t>(std::ceil(q * n));
        if (idx == 0)
            idx = 1;
        if (idx > sorted.size())
            idx = sorted.size();
        return sorted[idx - 1];
    };

    d.min_ms = sorted.front();
    d.p50_ms = rank(0.50);
    d.p95_ms = rank(0.95);
    d.p99_ms = rank(0.99);
    d.max_ms = sorted.back();
    return d;
}

bool MachineFingerprint::comparable_to(const MachineFingerprint& other) const {
    return gpu == other.gpu && driver == other.driver && os == other.os && build == other.build &&
           sanitizer == other.sanitizer && preset == other.preset && width == other.width &&
           height == other.height;
}

std::string MachineFingerprint::describe() const {
    return fmt::format("{} / {} / {} / {} / san={} / {}x{} / {}",
                       gpu.empty() ? "?" : gpu,
                       driver.empty() ? "?" : driver,
                       os.empty() ? "?" : os,
                       build.empty() ? "?" : build,
                       sanitizer.empty() ? "?" : sanitizer,
                       width,
                       height,
                       preset.empty() ? "?" : preset);
}

MachineFingerprint MachineFingerprint::detect() {
    MachineFingerprint m;
#if defined(_WIN32)
    m.os = "windows";
#elif defined(__APPLE__)
    m.os = "macos";
#else
    m.os = "linux";
#endif
    // Both of these are baked in by engine/core/CMakeLists.txt so the report describes THE BINARY,
    // not the shell variable someone believes they configured with. #125 landed because a stale
    // CMake cache meant those two disagreed; a perf report is the last place that should be
    // possible, since an instrumented or unoptimized build is several times slower and would read
    // as a catastrophic regression.
#ifdef RIME_BUILD_CONFIG
    m.build = RIME_BUILD_CONFIG;
#endif
#ifdef RIME_BUILD_SANITIZER
    m.sanitizer = RIME_BUILD_SANITIZER;
#endif
    if (m.build.empty())
        m.build = "unknown";
    if (m.sanitizer.empty())
        m.sanitizer = "unknown";
    return m;
}

RunInfo RunInfo::detect(std::string_view sample_name) {
    RunInfo r;
    r.sample = std::string(sample_name);
    const char* commit = std::getenv("RIME_PERF_COMMIT");
    r.commit = (commit && *commit) ? commit : "unknown";
    r.date = fmt::format("{:%Y-%m-%d}", fmt::gmtime(std::time(nullptr)));
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────

PerfReport::Timeline& PerfReport::timeline_for(std::string_view name) {
    for (Timeline& t : timelines_) {
        if (t.name == name)
            return t;
    }
    timelines_.push_back(Timeline{std::string(name), {}, {}, false});
    return timelines_.back();
}

void PerfReport::observe(std::string_view timeline, double ms) {
    Timeline& t = timeline_for(timeline);
    t.samples.add(ms);
    t.measured = true;
}

void PerfReport::observe_frame(std::uint64_t index, double ms, std::span<const PassTiming> passes) {
    observe("frame", ms);

    for (const PassTiming& p : passes) {
        auto it = std::find_if(pass_acc_.begin(), pass_acc_.end(), [&p](const PassAccumulator& a) {
            return a.name == p.name;
        });
        if (it == pass_acc_.end()) {
            pass_acc_.push_back(PassAccumulator{p.name, {}});
            it = std::prev(pass_acc_.end());
        }
        it->samples.add(p.ms);
    }

    // The worst frame keeps its own breakdown, so a failed gate can say WHICH pass blew the frame
    // rather than only that some frame did. The first frame always wins outright — otherwise a run
    // whose every frame took 0 ms would report a worst frame it never observed.
    const Timeline* frames = nullptr;
    for (const Timeline& t : timelines_) {
        if (t.name == "frame")
            frames = &t;
    }
    const bool first = frames && frames->samples.count() == 1;
    if (first || ms > worst_.ms) {
        worst_.index = index;
        worst_.ms = ms;
        worst_.passes.assign(passes.begin(), passes.end());
    }
}

void PerfReport::set_ledger(const WorkLedger& ledger) {
    ledger_.clear();
    ledger_.reserve(ledger.size());
    for (const WorkCounter& c : ledger.counters())
        ledger_.emplace_back(std::string(c.name), c.value);
}

std::optional<Distribution> PerfReport::distribution(std::string_view timeline) const {
    for (const Timeline& t : timelines_) {
        if (t.name != timeline)
            continue;
        return t.measured ? t.samples.summarize() : t.parsed;
    }
    return std::nullopt;
}

std::vector<std::string_view> PerfReport::timelines() const {
    std::vector<std::string_view> names;
    names.reserve(timelines_.size());
    for (const Timeline& t : timelines_)
        names.emplace_back(t.name);
    return names;
}

std::vector<PassCost> PerfReport::passes() const {
    if (pass_acc_.empty())
        return parsed_passes_;
    std::vector<PassCost> out;
    out.reserve(pass_acc_.size());
    for (const PassAccumulator& a : pass_acc_) {
        const Distribution d = a.samples.summarize();
        out.push_back(PassCost{a.name, d.p50_ms, d.max_ms});
    }
    return out;
}

bool PerfReport::ledger_contains(std::string_view name) const {
    return std::any_of(
        ledger_.begin(), ledger_.end(), [name](const auto& kv) { return kv.first == name; });
}

std::uint64_t PerfReport::ledger_value_or(std::string_view name, std::uint64_t fallback) const {
    for (const auto& [k, v] : ledger_) {
        if (k == name)
            return v;
    }
    return fallback;
}

std::string PerfReport::to_json(int indent) const {
    Writer w;
    w.indent = indent;
    bool first = true;

    w.open('{');

    w.item(first);
    w.key("schema");
    w.out += std::to_string(kSchemaVersion);

    w.item(first);
    w.key("run");
    {
        bool f = true;
        w.open('{');
        w.item(f);
        w.key("sample");
        w.out += '"' + escape(run_.sample) + '"';
        w.item(f);
        w.key("commit");
        w.out += '"' + escape(run_.commit) + '"';
        w.item(f);
        w.key("date");
        w.out += '"' + escape(run_.date) + '"';
        w.close('}', !f);
    }

    w.item(first);
    w.key("machine");
    {
        bool f = true;
        w.open('{');
        const auto str_field = [&](std::string_view k, const std::string& v) {
            w.item(f);
            w.key(k);
            w.out += '"' + escape(v) + '"';
        };
        str_field("gpu", machine_.gpu);
        str_field("driver", machine_.driver);
        str_field("os", machine_.os);
        str_field("build", machine_.build);
        str_field("sanitizer", machine_.sanitizer);
        str_field("preset", machine_.preset);
        w.item(f);
        w.key("width");
        w.out += std::to_string(machine_.width);
        w.item(f);
        w.key("height");
        w.out += std::to_string(machine_.height);
        w.close('}', !f);
    }

    w.item(first);
    w.key("distributions");
    {
        bool f = true;
        w.open('{');
        for (const Timeline& t : timelines_) {
            const Distribution d = t.measured ? t.samples.summarize() : t.parsed;
            w.item(f);
            w.key(t.name);
            bool g = true;
            w.open('{');
            w.item(g);
            w.key("count");
            w.out += std::to_string(d.count);
            const auto ms_field = [&](std::string_view k, double v) {
                w.item(g);
                w.key(k);
                w.out += ms_text(v);
            };
            ms_field("min_ms", d.min_ms);
            ms_field("p50_ms", d.p50_ms);
            ms_field("p95_ms", d.p95_ms);
            ms_field("p99_ms", d.p99_ms);
            ms_field("max_ms", d.max_ms);
            w.close('}', !g);
        }
        w.close('}', !f);
    }

    w.item(first);
    w.key("passes");
    {
        bool f = true;
        w.open('{');
        for (const PassCost& p : passes()) {
            w.item(f);
            w.key(p.name);
            bool g = true;
            w.open('{');
            w.item(g);
            w.key("p50_ms");
            w.out += ms_text(p.p50_ms);
            w.item(g);
            w.key("max_ms");
            w.out += ms_text(p.max_ms);
            w.close('}', !g);
        }
        w.close('}', !f);
    }

    w.item(first);
    w.key("worst_frame");
    {
        bool f = true;
        w.open('{');
        w.item(f);
        w.key("index");
        w.out += std::to_string(worst_.index);
        w.item(f);
        w.key("ms");
        w.out += ms_text(worst_.ms);
        w.item(f);
        w.key("passes");
        {
            bool g = true;
            w.open('{');
            for (const PassTiming& p : worst_.passes) {
                w.item(g);
                w.key(p.name);
                w.out += ms_text(p.ms);
            }
            w.close('}', !g);
        }
        w.close('}', !f);
    }

    w.item(first);
    w.key("ledger");
    {
        bool f = true;
        w.open('{');
        for (const auto& [name, value] : ledger_) {
            w.item(f);
            w.key(name);
            w.out += std::to_string(value); // exact integer, never through a double
        }
        w.close('}', !f);
    }

    w.close('}', !first);
    if (indent >= 0)
        w.out += '\n';
    return w.out;
}

bool PerfReport::parse(std::string_view text, PerfReport& out, std::string& error) {
    error.clear();
    JsonValue root;
    JsonParser parser(text);
    if (!parser.parse(root)) {
        error = parser.error();
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = "the top-level value is not an object";
        return false;
    }

    std::uint64_t schema = 0;
    if (!need_uint(root, "schema", schema, error))
        return false;
    if (schema != static_cast<std::uint64_t>(kSchemaVersion)) {
        error = fmt::format(
            "perf report schema {} — this engine reads schema {}", schema, kSchemaVersion);
        return false;
    }

    PerfReport r;

    const JsonValue* run = nullptr;
    if (!need_object(root, "run", run, error))
        return false;
    if (!need_string(*run, "sample", r.run_.sample, error) ||
        !need_string(*run, "commit", r.run_.commit, error) ||
        !need_string(*run, "date", r.run_.date, error)) {
        return false;
    }

    const JsonValue* machine = nullptr;
    if (!need_object(root, "machine", machine, error))
        return false;
    std::uint64_t width = 0, height = 0;
    if (!need_string(*machine, "gpu", r.machine_.gpu, error) ||
        !need_string(*machine, "driver", r.machine_.driver, error) ||
        !need_string(*machine, "os", r.machine_.os, error) ||
        !need_string(*machine, "build", r.machine_.build, error) ||
        !need_string(*machine, "sanitizer", r.machine_.sanitizer, error) ||
        !need_string(*machine, "preset", r.machine_.preset, error) ||
        !need_uint(*machine, "width", width, error) ||
        !need_uint(*machine, "height", height, error)) {
        return false;
    }
    r.machine_.width = static_cast<std::uint32_t>(width);
    r.machine_.height = static_cast<std::uint32_t>(height);

    const JsonValue* dists = nullptr;
    if (!need_object(root, "distributions", dists, error))
        return false;
    for (const auto& [name, node] : dists->object) {
        if (node.type != JsonValue::Type::Object) {
            error = fmt::format("distribution '{}' is not an object", name);
            return false;
        }
        Timeline t;
        t.name = name;
        t.measured = false;
        std::uint64_t count = 0;
        if (!need_uint(node, "count", count, error) ||
            !need_number(node, "min_ms", t.parsed.min_ms, error) ||
            !need_number(node, "p50_ms", t.parsed.p50_ms, error) ||
            !need_number(node, "p95_ms", t.parsed.p95_ms, error) ||
            !need_number(node, "p99_ms", t.parsed.p99_ms, error) ||
            !need_number(node, "max_ms", t.parsed.max_ms, error)) {
            error = fmt::format("distribution '{}': {}", name, error);
            return false;
        }
        t.parsed.count = static_cast<std::size_t>(count);
        r.timelines_.push_back(std::move(t));
    }

    const JsonValue* passes = nullptr;
    if (!need_object(root, "passes", passes, error))
        return false;
    for (const auto& [name, node] : passes->object) {
        if (node.type != JsonValue::Type::Object) {
            error = fmt::format("pass '{}' is not an object", name);
            return false;
        }
        PassCost p;
        p.name = name;
        if (!need_number(node, "p50_ms", p.p50_ms, error) ||
            !need_number(node, "max_ms", p.max_ms, error)) {
            error = fmt::format("pass '{}': {}", name, error);
            return false;
        }
        r.parsed_passes_.push_back(std::move(p));
    }

    const JsonValue* worst = nullptr;
    if (!need_object(root, "worst_frame", worst, error))
        return false;
    if (!need_uint(*worst, "index", r.worst_.index, error) ||
        !need_number(*worst, "ms", r.worst_.ms, error)) {
        return false;
    }
    const JsonValue* worst_passes = nullptr;
    if (!need_object(*worst, "passes", worst_passes, error))
        return false;
    for (const auto& [name, node] : worst_passes->object) {
        if (node.type != JsonValue::Type::Number) {
            error = fmt::format("worst_frame pass '{}' is not a number", name);
            return false;
        }
        r.worst_.passes.push_back(PassTiming{name, node.as_double()});
    }

    const JsonValue* ledger = nullptr;
    if (!need_object(root, "ledger", ledger, error))
        return false;
    for (const auto& [name, node] : ledger->object) {
        if (node.type != JsonValue::Type::Number) {
            error = fmt::format("ledger counter '{}' is not a number", name);
            return false;
        }
        // strtoull, not a double cast: the ledger's contract is exact 64-bit integers.
        r.ledger_.emplace_back(name, node.as_uint64());
    }

    out = std::move(r);
    return true;
}

bool PerfReport::load_file(const std::string& path, PerfReport& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = fmt::format("cannot open '{}'", path);
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    if (!parse(text, out, error)) {
        error = fmt::format("{}: {}", path, error);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────

ZoneTimelines::ZoneTimelines(PerfReport& report) : report_(&report) {
    // The lambda captures one pointer, so it fits a std::function's small-object buffer and the
    // sink copy inside report_zone() costs no allocation — which matters because this fires on
    // every stage of every tick of the run being measured.
    PerfReport* target = report_;
    set_zone_sink([target](std::string_view name, double ms) { target->observe(name, ms); });
}

void ZoneTimelines::stop() {
    if (report_) {
        set_zone_sink({});
        report_ = nullptr;
    }
}

ZoneTimelines::~ZoneTimelines() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────

PerfGate& PerfGate::at_most(std::string_view timeline, PerfStat stat, double limit_ms) {
    rules_.push_back(Rule{std::string(timeline), stat, limit_ms});
    return *this;
}

PerfGate& PerfGate::require_samples(std::string_view timeline, std::size_t min_count) {
    sample_rules_.push_back(SampleRule{std::string(timeline), min_count});
    return *this;
}

PerfGate& PerfGate::max_regression(double relative) {
    regression_ = relative;
    return *this;
}

PerfGate::Result PerfGate::check(const PerfReport& report, const PerfReport* baseline) const {
    Result result;

    for (const SampleRule& sr : sample_rules_) {
        const auto d = report.distribution(sr.timeline);
        if (!d) {
            result.violations.push_back(PerfViolation{
                sr.timeline, PerfStat::P99, PerfOutcome::Missing, 0.0, 0.0, 0.0, 0, sr.min_count});
            continue;
        }
        if (d->count < sr.min_count) {
            result.violations.push_back(PerfViolation{sr.timeline,
                                                      PerfStat::P99,
                                                      PerfOutcome::TooFewSamples,
                                                      0.0,
                                                      0.0,
                                                      0.0,
                                                      d->count,
                                                      sr.min_count});
        }
    }

    for (const Rule& rule : rules_) {
        const auto d = report.distribution(rule.timeline);
        // Same ruling as WorkBudget's `Missing`: a rule naming a timeline nobody recorded FAILS.
        // Rename a timeline, forget to rename its rule, and a skipping gate would keep reporting
        // success over a number it can no longer see.
        if (!d) {
            result.violations.push_back(PerfViolation{
                rule.timeline, rule.stat, PerfOutcome::Missing, 0.0, rule.limit_ms, 0.0, 0, 0});
            continue;
        }
        const double value = d->stat(rule.stat);
        if (value > rule.limit_ms) {
            result.violations.push_back(PerfViolation{rule.timeline,
                                                      rule.stat,
                                                      PerfOutcome::Breach,
                                                      value,
                                                      rule.limit_ms,
                                                      0.0,
                                                      d->count,
                                                      0});
        }
    }

    // The vacuity guard. A borrowed view over the report's owned counter names — safe because the
    // view dies at the end of this function while the report outlives the call.
    if (!work_.rules().empty()) {
        WorkLedger view;
        for (const auto& [name, value] : report.ledger_counters())
            view.set(name, value);
        result.work_violations = work_.check(view);
    }

    if (regression_ < 0.0) {
        result.baseline = BaselineStatus::NotProvided;
        result.baseline_note = "no regression check requested";
        return result;
    }
    if (!baseline) {
        result.baseline = BaselineStatus::NotProvided;
        result.baseline_note = "no committed baseline was supplied — this run establishes one, it "
                               "does not confirm one";
        return result;
    }
    if (!report.machine().comparable_to(baseline->machine())) {
        result.baseline = BaselineStatus::FingerprintMismatch;
        result.baseline_note =
            fmt::format("baseline is for a different configuration, so no "
                        "comparison was made\n      this run: {}\n      baseline: {}",
                        report.machine().describe(),
                        baseline->machine().describe());
        return result;
    }

    result.baseline = BaselineStatus::Compared;
    std::vector<std::string> absent;
    for (const Rule& rule : rules_) {
        const auto cur = report.distribution(rule.timeline);
        const auto base = baseline->distribution(rule.timeline);
        if (!cur)
            continue; // already reported as Missing above
        if (!base) {
            // A timeline this run measures and the baseline does not is NOT a failure — it is new.
            // It is still named out loud, because an unremarked skip is how a comparison rots.
            absent.push_back(rule.timeline);
            continue;
        }
        const double limit = base->stat(rule.stat) * (1.0 + regression_);
        const double value = cur->stat(rule.stat);
        if (value > limit) {
            result.violations.push_back(PerfViolation{rule.timeline,
                                                      rule.stat,
                                                      PerfOutcome::Regressed,
                                                      value,
                                                      limit,
                                                      base->stat(rule.stat),
                                                      cur->count,
                                                      0});
        }
    }
    result.baseline_note = fmt::format("compared against {} ({}), tolerance {:.0f}%",
                                       baseline->run().commit,
                                       baseline->run().date,
                                       regression_ * 100.0);
    if (!absent.empty()) {
        result.baseline_note += "\n      new in this run (nothing to compare against): ";
        for (std::size_t i = 0; i < absent.size(); ++i) {
            if (i)
                result.baseline_note += ", ";
            result.baseline_note += absent[i];
        }
    }
    return result;
}

std::string PerfGate::format(const Result& result) {
    std::string out;
    for (const PerfViolation& v : result.violations) {
        out += "  ";
        switch (v.outcome) {
            case PerfOutcome::Breach:
                out += fmt::format("{} {}: {:.3f} ms, budget {:.3f} ms  ({} samples)\n",
                                   v.timeline,
                                   perf_stat_name(v.stat),
                                   v.value_ms,
                                   v.limit_ms,
                                   v.count);
                break;
            case PerfOutcome::Regressed:
                out += fmt::format(
                    "{} {}: {:.3f} ms vs baseline {:.3f} ms — REGRESSED past {:.3f} ms\n",
                    v.timeline,
                    perf_stat_name(v.stat),
                    v.value_ms,
                    v.baseline_ms,
                    v.limit_ms);
                break;
            case PerfOutcome::Missing:
                out += fmt::format("{}: NOT RECORDED (a rule reads {} of it) — the timeline is "
                                   "missing, so this rule could never have failed\n",
                                   v.timeline,
                                   perf_stat_name(v.stat));
                break;
            case PerfOutcome::TooFewSamples:
                out += fmt::format("{}: {} samples, need {} — a percentile over this few frames is "
                                   "not evidence about the tail\n",
                                   v.timeline,
                                   v.count,
                                   v.required_count);
                break;
        }
    }
    out += WorkBudget::format(result.work_violations);
    out += fmt::format(
        "  baseline: {} — {}\n", baseline_status_name(result.baseline), result.baseline_note);
    return out;
}

} // namespace rime::core
