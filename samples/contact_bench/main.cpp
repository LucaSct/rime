// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <fmt/core.h>

#include <array>
#include <chrono>
#include <fstream>
#include <string_view>

#include "fixed_world.hpp"
#include "rime/core/hash.hpp"
#include "workload.hpp"

namespace cw = contact_workload;
namespace physics = rime::physics;
namespace core = rime::core;

namespace {
// Hash only semantic fields, with explicit float bits; object padding is not contact state.
void fold(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        hash ^= (value >> (i * 8)) & 255u;
        hash *= core::kFnv1a64Prime;
    }
}

void fold_float(std::uint64_t& hash, float value) {
    fold(hash, std::bit_cast<std::uint32_t>(value));
}

void fold_vec(std::uint64_t& hash, core::Vec3 v) {
    fold_float(hash, v.x);
    fold_float(hash, v.y);
    fold_float(hash, v.z);
}

void fold_manifold(std::uint64_t& hash, bool hit, const physics::Manifold& m) {
    fold(hash, hit ? 1u : 0u);
    if (!hit)
        return;
    fold(hash, m.a.index);
    fold(hash, m.a.generation);
    fold(hash, m.b.index);
    fold(hash, m.b.generation);
    fold_vec(hash, m.normal);
    fold(hash, m.count);
    fold(hash, m.child_a);
    fold(hash, m.child_b);
    fold(hash, m.patch);
    for (unsigned i = 0; i < m.count; ++i) {
        const auto& p = m.points[i];
        fold_vec(hash, p.position);
        fold_float(hash, p.penetration);
        fold(hash, p.feature_id);
        fold_float(hash, p.normal_impulse);
        fold_float(hash, p.tangent_impulse);
    }
}

struct Results {
    std::uint64_t exact = 0, speculative = 0, mismatch = 0, skipped = 0, bound_builds = 0;
    std::uint64_t hash = core::kFnv1a64OffsetBasis;
};

Results replay(const cw::Workload& w, bool verify, cw::Candidate candidate, bool cached = false) {
    Results r;
    physics::contact_bounds_detail::Cache cache;
    std::string_view world;
    std::uint64_t step = 0;
    std::size_t slots = 0;
    if (cached)
        for (const auto& p : w.pairs)
            slots = std::max(slots, static_cast<std::size_t>(p.b.index) + 1);
    for (const auto& p : w.pairs) {
        cw::Evaluation e;
        if (cached) {
            if (world != p.world || step != p.step) {
                r.bound_builds += cache.computed();
                cache.begin(slots);
                world = p.world;
                step = p.step;
            }
            const auto* ha = cw::hull(w, p.sa);
            const auto* hb = cw::hull(w, p.sb);
            bool skip = false;
            if (physics::contact_bounds_detail::eligible(p.sa.desc, ha) &&
                physics::contact_bounds_detail::eligible(p.sb.desc, hb)) {
                const auto& a = cache.get(p.a.index, p.sa.desc, p.pa, p.qa, ha);
                const auto& b = cache.get(p.b.index, p.sb.desc, p.pb, p.qb, hb);
                skip = !physics::overlaps(a.padded, b.tight);
            }
            e = cw::evaluate_decision(w, p, skip);
        } else
            e = cw::evaluate(w, p, candidate);
        const bool hit = e.exact;
        const auto& m = e.manifold;
        if (e.skipped)
            ++r.skipped;
        bool differs = verify && (hit != p.exact || (hit && !cw::same_manifold(m, p.expected)));
        if (verify && candidate != cw::Candidate::None) {
            const auto baseline = cw::evaluate(w, p, cw::Candidate::None);
            differs = differs || e.contact != baseline.contact || e.exact != baseline.exact ||
                      e.speculative != baseline.speculative ||
                      (e.contact && !cw::same_manifold(e.manifold, baseline.manifold));
        }
        if (differs) {
            if (r.mismatch < 8)
                fmt::print(stderr,
                           "exact mismatch: {} step {} pair {}:{} / {}:{}\n",
                           p.world,
                           p.step,
                           p.a.index,
                           p.a.generation,
                           p.b.index,
                           p.b.generation);
            ++r.mismatch;
        }
        if (hit)
            ++r.exact;
        if (e.speculative)
            ++r.speculative;
        fold_manifold(r.hash, e.contact, m);
    }
    if (cached)
        r.bound_builds += cache.computed();
    return r;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--fixed-world")
        return fixed_contact_world::run(argc, argv);
    bool verify_only = true;
    cw::Candidate candidate = cw::Candidate::None;
    bool cached = false;
    std::string_view candidate_name = "none";
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--time")
            verify_only = false;
        else if (arg == "--candidate" && i + 1 < argc) {
            candidate_name = argv[++i];
            cached = false;
            candidate = cw::Candidate::None;
            if (candidate_name == "cached") {
                candidate = cw::Candidate::Child;
                cached = true;
            } else if (candidate_name == "tight")
                candidate = cw::Candidate::Tight;
            else if (candidate_name == "child")
                candidate = cw::Candidate::Child;
            else if (candidate_name != "none") {
                fmt::print(stderr, "unknown candidate\n");
                return 2;
            }
        } else if (arg == "--verify-only")
            verify_only = true;
        else if (arg == "--input" && i + 1 < argc)
            path = argv[++i];
        else {
            fmt::print(stderr,
                       "usage: contact_bench --input FILE [--candidate none|tight|child|cached] "
                       "[--time|--verify-only]\n");
            return 2;
        }
    }
    if (path == nullptr) {
        fmt::print(stderr, "--input is required\n");
        return 2;
    }
    cw::Workload w;
    std::ifstream file(path);
    std::string error;
    if (!file || !cw::load(file, w, error)) {
        fmt::print(stderr, "contact_bench: {}\n", file ? error : "cannot open input");
        return 2;
    }
    // Identify all immutable input bytes, including separated pairs that emit no contact.
    file.clear();
    file.seekg(0);
    std::array<char, 8192> buffer{};
    std::uint64_t input_hash = core::kFnv1a64OffsetBasis;
    while (file.read(buffer.data(), buffer.size()) || file.gcount() != 0) {
        input_hash = core::fnv1a_64(
            std::string_view(buffer.data(), static_cast<std::size_t>(file.gcount())), input_hash);
    }
    if (!file.eof()) {
        fmt::print(stderr, "input read failed\n");
        return 2;
    }
    if (cached && std::any_of(w.pairs.begin(), w.pairs.end(), [](const auto& p) {
            return p.b.index >= 1000000;
        })) {
        fmt::print(stderr, "cache slot capacity exceeds diagnostic limit\n");
        return 2;
    }
    const auto reference = replay(w, true, cw::Candidate::None);
    if (reference.mismatch != 0) {
        fmt::print(stderr,
                   "{} of {} captured exact results differ; refusing timing\n",
                   reference.mismatch,
                   w.pairs.size());
        return 1;
    }
    const auto proposed =
        candidate == cw::Candidate::None ? reference : replay(w, true, candidate, cached);
    if (proposed.mismatch != 0 || proposed.hash != reference.hash) {
        fmt::print(stderr,
                   "candidate {}: {} exact mismatches, {} skipped calls; refusing timing\n",
                   candidate_name,
                   proposed.mismatch,
                   proposed.skipped);
        return 1;
    }
    fmt::print("{{\"rows\":{},\"hulls\":{},\"stride\":{},\"total_queries\":{},\"exact_contacts\":{}"
               ",\"speculative_contacts\":{},\"contact_hash\":\"{:016x}\",\"mismatches\":0",
               w.pairs.size(),
               w.hulls.size(),
               w.stride,
               w.ledger.queries,
               reference.exact,
               reference.speculative,
               reference.hash);
    fmt::print(",\"candidate\":\"{}\",\"exact_calls\":{},\"skipped_exact\":{}",
               candidate_name,
               w.pairs.size() - proposed.skipped,
               proposed.skipped);
    fmt::print(",\"cached_bound_builds\":{}", proposed.bound_builds);
    fmt::print(",\"input_fnv1a64\":\"{:016x}\"", input_hash);
    if (!verify_only) {
        // Each repeat starts from the SAME immutable inputs. No integration, destruction, settling
        // or sleep can change this workload. Whole-corpus distributions include dispatch and the
        // semantic checksum, but exclude parsing/verification; they are not per-query percentiles.
        std::array<double, 21> ms{};
        for (auto& value : ms) {
            const auto start = std::chrono::steady_clock::now();
            const auto r = replay(w, false, candidate, cached);
            value =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (r.hash != reference.hash || r.exact != reference.exact ||
                r.speculative != reference.speculative || r.skipped != proposed.skipped ||
                r.bound_builds != proposed.bound_builds) {
                fmt::print(stderr, "nonrepeatable workload; refusing report\n");
                return 1;
            }
        }
        std::sort(ms.begin(), ms.end());
        fmt::print(",\"repeats\":{},\"corpus_ms\":{{\"min\":{:.6f},\"p50\":{:.6f},\"p99\":{:.6f},"
                   "\"max\":{:.6f}}}",
                   ms.size(),
                   ms.front(),
                   ms[10],
                   ms.back(),
                   ms.back());
    }
    fmt::print("}}\n");
    return 0;
}
