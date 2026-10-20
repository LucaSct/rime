// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>

#include "workload.hpp"

// Linked ONLY into the externally generated capture executable, never the production library.
// Contact generation can overlap between the named server/client worlds; file writes and hull
// dictionary admission are locked, while per-step accounting stays local to its owning thread.
// The final replay sorts by semantic identity, never by the nondeterministic append order.
namespace contact_capture {
using namespace contact_workload;

struct Recorder {
    std::mutex mutex;
    std::ofstream file, hash_file;
    std::map<std::string, std::uint64_t> hash_steps;
    std::map<std::string, std::uint64_t> world_steps;
    std::map<std::string, const void*> world_owners;
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::uint32_t> hull_ids;
    Ledger ledger{};
    std::uint32_t stride = 20;
    bool enabled = false;

    void open(const char* path) {
        file.open(path, std::ios::out | std::ios::trunc);
        if (!file)
            fail("cannot open RIME_CONTACT_CAPTURE");
        enabled = true;
        file << "RIME_CONTACTS 1 " << stride << '\n';
    }

    Recorder(const char* path, std::uint32_t sampling_stride) : stride(sampling_stride) {
        if (stride == 0 || stride > 1000000)
            fail("invalid sampling stride");
        open(path);
    }

    Recorder() {
        if (const char* hashes = std::getenv("RIME_CONTACT_HASHES")) {
            hash_file.open(hashes, std::ios::out | std::ios::trunc);
            if (!hash_file)
                fail("cannot open RIME_CONTACT_HASHES");
            hash_file << "world,step,hash\n";
        }
        const char* path = std::getenv("RIME_CONTACT_CAPTURE");
        if (path == nullptr || *path == '\0')
            return;
        if (const char* text = std::getenv("RIME_CONTACT_STRIDE")) {
            const std::string_view value(text);
            const auto [end, ec] =
                std::from_chars(value.data(), value.data() + value.size(), stride);
            if (ec != std::errc{} || end != value.data() + value.size() || stride == 0 ||
                stride > 1000000)
                fail("invalid RIME_CONTACT_STRIDE");
        }
        open(path);
    }

    [[noreturn]] static void fail(const char* reason) {
        std::fprintf(stderr, "contact capture: %s\n", reason);
        std::abort();
    }

    ~Recorder() {
        if (hash_file.is_open()) {
            hash_file.flush();
            if (!hash_file)
                fail("hash write failed");
        }
        if (!enabled)
            return;
        write_footer(file, ledger);
        file.flush();
        if (!file)
            fail("write failed (corpus is not valid)");
        std::fprintf(stderr,
                     "contact capture: %llu rows, %llu selected queries, %llu total queries, %llu "
                     "cap drops\n",
                     static_cast<unsigned long long>(ledger.rows),
                     static_cast<unsigned long long>(ledger.selected_queries),
                     static_cast<unsigned long long>(ledger.queries),
                     static_cast<unsigned long long>(ledger.drops));
    }

    std::uint32_t
    hull_id(const std::string& world, const physics::ShapeDesc& s, const physics::ConvexHull* h) {
        if (h == nullptr)
            return kNoHull;
        const auto key = std::make_tuple(world, s.hull.index, s.hull.generation);
        const auto it = hull_ids.find(key);
        if (it != hull_ids.end())
            return it->second;
        const auto id = static_cast<std::uint32_t>(hull_ids.size());
        hull_ids.emplace(key, id);
        write_hull(file, id, *h);
        return id;
    }
};

inline Recorder& recorder() {
    static Recorder r;
    return r;
}

struct Step {
    Recorder& owner;
    std::string world;
    std::uint64_t index = 0;
    // Range tasks share this build's recorder. A relaxed integer count needs no ordering; the
    // fork/join completes all records before Step's destructor folds the count into the ledger.
    std::atomic<std::uint64_t> queries{0};
    float dt;
    bool selected = false;

    Step(const void* identity, std::string_view label, float timestep)
        : Step(recorder(), identity, label, timestep) {}

    Step(Recorder& recording, const void* identity, std::string_view label, float timestep)
        : owner(recording), world(label), dt(timestep) {
        if (!owner.enabled)
            return;
        std::lock_guard lock(owner.mutex);
        if (world.empty() || world.find_first_of(" \t\n\r") != std::string::npos)
            Recorder::fail("world label is not a token");
        const auto [it, inserted] = owner.world_owners.emplace(world, identity);
        if (!inserted && it->second != identity)
            Recorder::fail("world labels must be unique");
        index = ++owner.world_steps[world];
        selected = index % owner.stride == 0;
        ++owner.ledger.steps;
        if (selected)
            ++owner.ledger.selected_steps;
    }

    Step(const Step&) = delete;
    Step& operator=(const Step&) = delete;

    ~Step() {
        if (!owner.enabled)
            return;
        std::lock_guard lock(owner.mutex);
        owner.ledger.queries += queries.load(std::memory_order_relaxed);
        if (selected)
            owner.ledger.selected_queries += queries.load(std::memory_order_relaxed);
    }
};

inline bool hashes_enabled() {
    return recorder().hash_file.is_open();
}

inline void hash_step(std::string_view world, std::uint64_t hash) {
    auto& r = recorder();
    if (!r.hash_file.is_open())
        return;
    std::lock_guard lock(r.mutex);
    const auto step = ++r.hash_steps[std::string(world)];
    r.hash_file << world << ',' << step << ',' << hash << '\n';
    if (!r.hash_file)
        Recorder::fail("hash write failed");
}

inline void record(Step& step,
                   physics::BodyId a,
                   physics::BodyId b,
                   const physics::ShapeDesc& sa,
                   core::Vec3 pa,
                   core::Quat qa,
                   core::Vec3 va,
                   const physics::ShapeDesc& sb,
                   core::Vec3 pb,
                   core::Quat qb,
                   core::Vec3 vb,
                   bool ccd,
                   const physics::ConvexHull* ha,
                   const physics::ConvexHull* hb,
                   bool touching,
                   const physics::Manifold& expected) {
    step.queries.fetch_add(1, std::memory_order_relaxed);
    if (!step.owner.enabled || !step.selected)
        return;
    auto& r = step.owner;
    std::lock_guard lock(r.mutex);
    if (r.ledger.rows >= kMaxRows) {
        ++r.ledger.drops;
        return;
    }
    Pair p;
    p.world = step.world;
    p.step = step.index;
    p.a = a;
    p.b = b;
    p.sa = {sa, r.hull_id(p.world, sa, ha)};
    p.sb = {sb, r.hull_id(p.world, sb, hb)};
    p.pa = pa;
    p.qa = qa;
    p.va = va;
    p.pb = pb;
    p.qb = qb;
    p.vb = vb;
    p.dt = step.dt;
    p.ccd = ccd;
    p.exact = touching;
    p.expected = expected;
    write_pair(r.file, p);
    if (!r.file)
        Recorder::fail("write failed (corpus is not valid)");
    ++r.ledger.rows;
}
} // namespace contact_capture
