// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
// External diagnostic context, shared by the three generated translation units.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include "rime/core/diagnostics/profile.hpp"

namespace city_profile {
using Clock = std::chrono::steady_clock;
struct CpuRow { std::uint64_t frame; std::string name; double ms; std::uint64_t candidates; std::uint64_t draws; };
struct GpuRow { std::uint64_t frame; std::string name; double ms; };
struct StageRow { std::uint64_t frame; std::string name; double ms; };
struct CasterRow { std::uint64_t frame, cascade, considered, submitted, culled; const char* view; };
struct PhaseRow { std::uint64_t frame, step; std::string world, name; double ms; };
struct IslandRow {
    std::uint64_t frame, step;
    std::string world;
    std::uint32_t island, bodies, contacts;
    std::uint64_t ns, warm_ns, velocity_ns, store_ns, position_ns;
};
struct StepKey { std::uint64_t frame, step; const char* world; };
struct ContactRow {
    std::uint64_t frame, step;
    std::string world;
    std::array<std::uint64_t, 26> values;
};

// The zone sink runs on both the caller and the server job. Keep its frame window,
// per-world sequence, and output buffers behind one lock; never consult thread_local config here.
class PhaseRecorder {
public:
    void begin_frame(std::uint64_t frame) {
        const std::lock_guard lock(mutex_);
        frame_ = frame; steps_ = {}; open_ = true;
    }
    void end_frame() {
        const std::lock_guard lock(mutex_);
        open_ = false;
    }
    std::optional<StepKey> zone(std::string_view name, double ms) {
        const std::string_view full_name = name;
        constexpr std::string_view prefix = "physics.";
        if (!name.starts_with(prefix)) return std::nullopt;
        name.remove_prefix(prefix.size());
        const bool server = name.starts_with("server.");
        if (!server && !name.starts_with("client.")) return std::nullopt;
        name.remove_prefix(7);
        const std::lock_guard lock(mutex_);
        if (!open_) return std::nullopt;
        auto& step = steps_[server ? 0 : 1];
        const StepKey key{frame_, step, server ? "server" : "client"};
        phases_.push_back({frame_, step, key.world, std::string(full_name), ms});
        if (name == "step") { ++step; return key; }
        return std::nullopt;
    }
    template <class Costs> void islands(StepKey key, const Costs& costs) {
        const std::lock_guard lock(mutex_);
        for (const auto& cost : costs)
            islands_.push_back({key.frame, key.step, key.world, cost.island, cost.bodies,
                                cost.constraints, cost.solve_ns, cost.warm_ns, cost.velocity_ns,
                                cost.store_ns, cost.position_ns});
    }
    template <class Profile> void contact(StepKey key, const Profile& p) {
        // Copy on the world's stepping thread, while its last build still belongs to this step.
        // The sink's mutex serializes publication with the other world's callback.
        const std::lock_guard lock(mutex_);
        contacts_.push_back({key.frame, key.step, key.world,
            {p.raw_candidate_keys, p.unique_pairs, p.duplicate_keys, p.plain_pairs,
             p.compound_pairs, p.posed_children, p.tested_child_pairs, p.culled_child_pairs,
             p.exact_narrowphase_calls, p.speculative_calls, p.heightfield_pairs,
             p.produced_manifolds, p.pair_query_ns, p.pair_canonicalize_ns, p.plain_exact_ns,
             p.plain_speculative_ns, p.compound_pose_ns, p.compound_midphase_ns,
             p.compound_exact_ns, p.compound_speculative_ns, p.heightfield_ns, p.warmcache_ns, p.plain_bounds_tests,
             p.plain_bounds_rejected, p.plain_bounds_bodies, p.plain_bounds_ns}});
    }
    std::size_t contact_rows() const {
        const std::lock_guard lock(mutex_);
        return contacts_.size();
    }
    void write(const std::filesystem::path& dir, bool contact_profile) const {
        // Called only after app.step has joined all work and the sink has been removed.
        auto open = [&](const char* name) { std::ofstream file(dir / name); if (!file) throw std::runtime_error(name); return file; };
        auto phases = open("physics-phases.csv");
        phases << "frame,world,step,name,ms\n";
        for (const auto& row : phases_)
            phases << row.frame << ',' << row.world << ',' << row.step << ',' << row.name << ',' << row.ms << '\n';
        phases.flush(); if (!phases) throw std::runtime_error("physics-phases.csv write failed");
        auto islands = open("physics-islands.csv");
        islands << "frame,world,step,island,bodies,contacts,ns,warm_ns,velocity_ns,store_ns,position_ns\n";
        for (const auto& row : islands_)
            islands << row.frame << ',' << row.world << ',' << row.step << ',' << row.island
                    << ',' << row.bodies << ',' << row.contacts << ',' << row.ns << ',' << row.warm_ns
                    << ',' << row.velocity_ns << ',' << row.store_ns << ',' << row.position_ns << '\n';
        islands.flush(); if (!islands) throw std::runtime_error("physics-islands.csv write failed");
        if (contact_profile) {
            const auto step_rows = std::count_if(phases_.begin(), phases_.end(), [](const auto& row) {
                return row.name == "physics.server.step" || row.name == "physics.client.step";
            });
            if (contacts_.size() != static_cast<std::size_t>(step_rows))
                throw std::runtime_error("contact profile rows do not match physics step zones");
            auto contacts = open("contact-profile.csv");
            contacts << "frame,world,step,raw_candidate_keys,unique_pairs,duplicate_keys,plain_pairs,compound_pairs,posed_children,tested_child_pairs,culled_child_pairs,exact_narrowphase_calls,speculative_calls,heightfield_pairs,produced_manifolds,pair_query_ns,pair_canonicalize_ns,plain_exact_ns,plain_speculative_ns,compound_pose_ns,compound_midphase_ns,compound_exact_ns,compound_speculative_ns,heightfield_ns,warmcache_ns,plain_bounds_tests,plain_bounds_rejected,plain_bounds_bodies,plain_bounds_ns\n";
            for (const auto& row : contacts_) {
                const auto& v = row.values;
                if (v[0] != v[1] + v[2] || v[1] != v[3] + v[4] + v[10] ||
                    v[7] > v[6] || v[22] > v[3] || v[23] > v[22] || v[24] > 2*v[22])
                    throw std::runtime_error("contact profile pair accounting failed");
                contacts << row.frame << ',' << row.world << ',' << row.step;
                for (auto value : v) contacts << ',' << value;
                contacts << '\n';
            }
            contacts.flush(); if (!contacts) throw std::runtime_error("contact-profile.csv write failed");
        }
    }
private:
    mutable std::mutex mutex_;
    bool open_ = false;
    std::uint64_t frame_ = 0;
    std::array<std::uint64_t, 2> steps_{};
    std::vector<PhaseRow> phases_;
    std::vector<IslandRow> islands_;
    std::vector<ContactRow> contacts_;
};

struct PhaseSinkGuard {
    explicit PhaseSinkGuard(rime::core::ZoneSink sink) { rime::core::set_zone_sink(std::move(sink)); }
    ~PhaseSinkGuard() { rime::core::set_zone_sink({}); }
    PhaseSinkGuard(const PhaseSinkGuard&) = delete;
    PhaseSinkGuard& operator=(const PhaseSinkGuard&) = delete;
};
struct PhysicsRow {
    std::uint32_t bodies = 0, dynamic = 0, awake = 0, sleeping = 0;
    std::uint32_t pairs = 0, manifolds = 0, contact_points = 0;
    std::uint32_t islands = 0, active_islands = 0, largest_active_island = 0;
};
template <class Stats> inline void physics(PhysicsRow& row, const Stats& s) {
    row.bodies = s.body_count; row.dynamic = s.dynamic_bodies;
    row.awake = s.awake_bodies; row.sleeping = s.sleeping_bodies;
    row.pairs = s.broadphase_pairs; row.manifolds = s.manifolds;
    row.contact_points = s.contact_points; row.islands = s.islands;
    row.active_islands = s.active_islands;
    row.largest_active_island = s.largest_active_island;
}
struct FrameRow { std::uint64_t frame;
    std::uint64_t ticks = 0, damage_ops = 0, committed_ops = 0;
    std::uint64_t server_debris = 0, client_debris = 0;
    std::uint64_t charge_targets = 0;
    bool charge_requested = false, charge_fired = false, peers_agree = true;
    std::uint32_t charge_building = 0;
    std::uint64_t charge_tick = 0;
    double frame_cpu_ms = 0, pre_sim_ms = 0, post_session_ms = 0;
    double session_post_sim_ms = 0, server_ms = 0, client_ms = 0, join_ms = 0;
    PhysicsRow server_physics{}, client_physics{}; std::uint64_t main_considered, main_submitted, main_culled;
    std::uint64_t optic_considered, optic_submitted, optic_culled;
    std::uint64_t main_shadow_rendered, main_shadow_reused, optic_shadow_rendered, optic_shadow_reused;
    std::uint64_t graph_passes, executed_passes, timed_passes; bool overflow, gpu_present; };
struct Context {
    std::thread::id owner{};
    std::uint64_t frame = 0, warmup = 60;
    bool recording = false, ads = false, control = false, collapse = false, multi_collapse = false;
    bool physics_phases = false, contact_profile = false;
    bool multi_missing_target = false;
    std::array<std::uint64_t, 3> multi_targets{};
    std::array<std::uint64_t, 3> multi_alive_before{}, multi_alive_after{};
    std::array<bool, 3> multi_effects{};
    bool shadow_caster_culling = true, ppm_written = false;
    std::filesystem::path ppm;
    std::uint64_t charge_targets = 0;
    double session_post_sim_ms = 0, post_session_ms = 0;
    const char* prefix = "";
    CpuRow* current = nullptr;
    std::vector<CpuRow> cpu;
    std::vector<GpuRow> gpu;
    std::vector<StageRow> stages;
    std::vector<CasterRow> casters;
    std::vector<FrameRow> frames;
    Context() { cpu.reserve(60000); gpu.reserve(20000); stages.reserve(1000); frames.reserve(256); }
};
inline thread_local Context config;
inline bool active() { return config.recording && std::this_thread::get_id() == config.owner; }
inline double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Prefix {
    const char* old;
    explicit Prefix(const char* value) : old(config.prefix) { config.prefix = value; }
    ~Prefix() { config.prefix = old; }
};
struct PassTimer {
    Clock::time_point start;
    CpuRow* old;
    PassTimer(const std::string& name) : start(Clock::now()), old(config.current) {
        if (active()) { config.cpu.push_back({config.frame, name, 0, 0, 0}); config.current = &config.cpu.back(); }
    }
    ~PassTimer() {
        if (active() && config.current) config.current->ms = elapsed_ms(start);
        config.current = old;
    }
};
inline void draws(std::uint64_t candidates, std::uint64_t actual) {
    if (active() && config.current) { config.current->candidates += candidates; config.current->draws += actual; }
}
template <class StatsArray> inline void capture_casters(std::uint64_t frame,
                                                     const char* view,
                                                     const StatsArray& stats) {
    for (std::size_t cascade = 0; cascade < stats.size(); ++cascade) {
        const auto& s = stats[cascade];
        config.casters.push_back({frame, cascade, s.considered(), s.submitted, s.culled, view});
    }
}
inline void stage(const char* name, Clock::time_point start) {
    if (active()) config.stages.push_back({config.frame, name, elapsed_ms(start)});
}
inline bool parse_count(std::string_view value, std::uint64_t& result, bool positive) {
    if (value.empty()) return false;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && (!positive || result > 0);
}
inline bool parse_cli(int argc, char** argv, std::filesystem::path& output, std::uint64_t& frames) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--ads") config.ads = true;
        else if (flag == "--hip") config.ads = false;
        else if (flag == "--control") config.control = true;
        else if (flag == "--city") config.control = false;
        else if (flag == "--collapse") config.collapse = true;
        else if (flag == "--multi-collapse") { config.collapse = true; config.multi_collapse = true; }
        else if (flag == "--physics-phases") config.physics_phases = true;
        else if (flag == "--contact-profile") config.contact_profile = true;
        else if (flag == "--shadow-culling-off") config.shadow_caster_culling = false;
        else if (flag == "--shadow-culling-on") config.shadow_caster_culling = true;
        else if (flag == "--ppm") {
            if (++i >= argc || !*argv[i] || std::string_view(argv[i]).starts_with("--")) return false;
            config.ppm = argv[i];
        }
        else if (flag == "--out-dir") {
            if (++i >= argc || !*argv[i] || std::string_view(argv[i]).starts_with("--")) return false;
            output = argv[i];
        } else if (flag == "--warmup") {
            if (++i >= argc || !parse_count(argv[i], config.warmup, false)) return false;
        } else if (flag == "--frames") {
            if (++i >= argc || !parse_count(argv[i], frames, true)) return false;
        } else return false;
    }
    if (config.multi_collapse && (frames < 180 || frames > 360 || config.warmup > 600)) return false;
    if (config.contact_profile && !config.physics_phases) return false;
    if (config.collapse && !config.multi_collapse && (frames < 120 || frames > 360 || config.warmup > 600)) return false;
    if (frames > std::numeric_limits<std::uint64_t>::max() - config.warmup) return false;
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error || !std::filesystem::is_directory(output)) return false;
    return true;
}
inline void write(const std::filesystem::path& dir) {
    auto open = [&](const char* name) { std::ofstream f(dir / name); if (!f) throw std::runtime_error(name); return f; };
    auto finish = [](std::ofstream& file, const char* name) {
        file.flush(); if (!file) throw std::runtime_error(name);
    };
    auto frames = open("frames.csv");
    frames << "frame,main_considered,main_submitted,main_culled,optic_considered,optic_submitted,optic_culled,main_shadow_rendered,main_shadow_reused,optic_shadow_rendered,optic_shadow_reused,graph_passes,executed_passes,timed_passes,overflow,gpu_present,ticks,frame_cpu_ms,pre_sim_ms,post_session_ms,session_post_sim_ms,server_ms,client_ms,join_ms,charge_requested,charge_fired,charge_targets,charge_building,charge_tick,peers_agree,damage_ops,committed_ops,server_debris,client_debris";
    for (const char* side : {"server", "client"})
        for (const char* field : {"bodies", "dynamic", "awake", "sleeping", "pairs", "manifolds", "contact_points", "islands", "active_islands", "largest_active_island"})
            frames << ',' << side << '_' << field;
    frames << '\n';
    auto write_physics = [&](const PhysicsRow& p) {
        frames << ',' << p.bodies << ',' << p.dynamic << ',' << p.awake << ',' << p.sleeping
               << ',' << p.pairs << ',' << p.manifolds << ',' << p.contact_points
               << ',' << p.islands << ',' << p.active_islands << ',' << p.largest_active_island;
    };
    for (const auto& r : config.frames) {
        frames << r.frame << ',' << r.main_considered << ',' << r.main_submitted << ',' << r.main_culled
               << ',' << r.optic_considered << ',' << r.optic_submitted << ',' << r.optic_culled
               << ',' << r.main_shadow_rendered << ',' << r.main_shadow_reused
               << ',' << r.optic_shadow_rendered << ',' << r.optic_shadow_reused << ',' << r.graph_passes
               << ',' << r.executed_passes << ',' << r.timed_passes << ',' << r.overflow << ',' << r.gpu_present
               << ',' << r.ticks << ',' << r.frame_cpu_ms << ',' << r.pre_sim_ms
               << ',' << r.post_session_ms << ',' << r.session_post_sim_ms << ',' << r.server_ms
               << ',' << r.client_ms << ',' << r.join_ms << ',' << r.charge_requested
               << ',' << r.charge_fired << ',' << r.charge_targets << ',' << r.charge_building
               << ',' << r.charge_tick << ',' << r.peers_agree << ',' << r.damage_ops
               << ',' << r.committed_ops << ',' << r.server_debris << ',' << r.client_debris;
        write_physics(r.server_physics); write_physics(r.client_physics);
        frames << '\n';
    }
    finish(frames, "frames.csv");
    auto cpu = open("cpu-passes.csv"); cpu << "frame,name,ms,mesh_candidates,mesh_indexed_draws\n";
    for (const auto& r : config.cpu) cpu << r.frame << ',' << r.name << ',' << r.ms << ',' << r.candidates << ',' << r.draws << '\n';
    finish(cpu, "cpu-passes.csv");
    auto gpu = open("gpu-passes.csv"); gpu << "frame,name,ms\n";
    for (const auto& r : config.gpu) gpu << r.frame << ',' << r.name << ',' << r.ms << '\n';
    finish(gpu, "gpu-passes.csv");
    auto stages = open("graph-stages.csv"); stages << "frame,name,ms\n";
    for (const auto& r : config.stages) stages << r.frame << ',' << r.name << ',' << r.ms << '\n';
    finish(stages, "graph-stages.csv");
    auto casters = open("shadow-casters.csv");
    casters << "frame,view,cascade,considered,submitted,culled\n";
    for (const auto& r : config.casters)
        casters << r.frame << ',' << r.view << ',' << r.cascade << ',' << r.considered
                << ',' << r.submitted << ',' << r.culled << '\n';
    finish(casters, "shadow-casters.csv");
}
} // namespace city_profile
