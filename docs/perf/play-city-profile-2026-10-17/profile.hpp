// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
// External diagnostic context, shared by the three generated translation units.
#pragma once
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <stdexcept>

namespace city_profile {
using Clock = std::chrono::steady_clock;
struct CpuRow { std::uint64_t frame; std::string name; double ms; std::uint64_t candidates; std::uint64_t draws; };
struct GpuRow { std::uint64_t frame; std::string name; double ms; };
struct StageRow { std::uint64_t frame; std::string name; double ms; };
struct FrameRow { std::uint64_t frame; std::uint64_t main_considered, main_submitted, main_culled;
    std::uint64_t optic_considered, optic_submitted, optic_culled;
    std::uint64_t main_shadow_rendered, main_shadow_reused, optic_shadow_rendered, optic_shadow_reused;
    std::uint64_t graph_passes, executed_passes, timed_passes; bool overflow, gpu_present; };
struct Context {
    std::thread::id owner{};
    std::uint64_t frame = 0, warmup = 60;
    bool recording = false, ads = false, control = false;
    const char* prefix = "";
    CpuRow* current = nullptr;
    std::vector<CpuRow> cpu;
    std::vector<GpuRow> gpu;
    std::vector<StageRow> stages;
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
inline void stage(const char* name, Clock::time_point start) {
    if (active()) config.stages.push_back({config.frame, name, elapsed_ms(start)});
}
inline void write(const std::filesystem::path& dir) {
    auto open = [&](const char* name) { std::ofstream f(dir / name); if (!f) throw std::runtime_error(name); return f; };
    auto frames = open("frames.csv");
    frames << "frame,main_considered,main_submitted,main_culled,optic_considered,optic_submitted,optic_culled,main_shadow_rendered,main_shadow_reused,optic_shadow_rendered,optic_shadow_reused,graph_passes,executed_passes,timed_passes,overflow,gpu_present\n";
    for (const auto& r : config.frames) frames << r.frame << ',' << r.main_considered << ',' << r.main_submitted << ',' << r.main_culled << ',' << r.optic_considered << ',' << r.optic_submitted << ',' << r.optic_culled << ',' << r.main_shadow_rendered << ',' << r.main_shadow_reused << ',' << r.optic_shadow_rendered << ',' << r.optic_shadow_reused << ',' << r.graph_passes << ',' << r.executed_passes << ',' << r.timed_passes << ',' << r.overflow << ',' << r.gpu_present << '\n';
    auto cpu = open("cpu-passes.csv"); cpu << "frame,name,ms,mesh_candidates,mesh_indexed_draws\n";
    for (const auto& r : config.cpu) cpu << r.frame << ',' << r.name << ',' << r.ms << ',' << r.candidates << ',' << r.draws << '\n';
    auto gpu = open("gpu-passes.csv"); gpu << "frame,name,ms\n";
    for (const auto& r : config.gpu) gpu << r.frame << ',' << r.name << ',' << r.ms << '\n';
    auto stages = open("graph-stages.csv"); stages << "frame,name,ms\n";
    for (const auto& r : config.stages) stages << r.frame << ',' << r.name << ',' << r.ms << '\n';
}
} // namespace city_profile
