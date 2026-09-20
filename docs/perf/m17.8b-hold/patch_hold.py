#!/usr/bin/env python3
"""Scratch-only edit of the WORKTREE copy of 99-the-block/main.cpp: adds a render-only hold loop
(--hold N, --hold-out csv) after the measured loop, with the simulation frozen. Never touches the
main tree."""
import re, sys
p = sys.argv[1]
src = open(p).read()
assert '--hold' not in src, 'already patched'

# 1. options
src = src.replace(
"""    std::uint32_t pipelined = 1;
};""",
"""    std::uint32_t pipelined = 1;
    // SCRATCH (perf-methodology probe, 2026-09-16): render-only frames after the measured loop,
    // with the simulation frozen in its post-collapse state, and a per-frame CSV of what they cost.
    int hold = 0;
    const char* hold_out = nullptr;
};""", 1)

# 2. arg parsing
src = src.replace(
"""        } else if (a == "--warmup" && i + 1 < argc) {
            perf.warmup = std::atoi(argv[++i]);""",
"""        } else if (a == "--warmup" && i + 1 < argc) {
            perf.warmup = std::atoi(argv[++i]);
        } else if (a == "--hold" && i + 1 < argc) {
            perf.hold = std::atoi(argv[++i]);
        } else if (a == "--hold-out" && i + 1 < argc) {
            perf.hold_out = argv[++i];""", 1)

# 3. the hold loop, after the passes_queue drain
anchor = """    for (const auto& [index, timings] : passes_queue) {
        if (index >= frame_base)
            report.observe_passes(index - frame_base, timings);
    }
"""
assert anchor in src
hold = anchor + r"""
    // ── SCRATCH: the render-only hold loop ─────────────────────────────────────────────────
    // The simulation is not stepped, so every frame renders the same post-collapse scene. Frame
    // wall, the three render stage zones and every pass's GPU time are recorded PER FRAME, with a
    // wall-clock stamp so the frames can be aligned against an external clock trace.
    if (opt.hold > 0) {
        struct HoldFrame {
            double t = 0, frame_ms = 0, declare = 0, execute = 0, submit = 0;
            std::vector<std::pair<std::string, double>> passes;
        };
        std::vector<HoldFrame> hold(static_cast<std::size_t>(opt.hold));
        std::size_t cur = hold.size();
        core::set_zone_sink([&hold, &cur](std::string_view name, double ms) {
            if (cur >= hold.size())
                return;
            if (name == "frame.declare")
                hold[cur].declare += ms;
            else if (name == "frame.execute")
                hold[cur].execute += ms;
            else if (name == "frame.submit")
                hold[cur].submit += ms;
        });
        passes_queue.clear();
        const std::uint64_t hold_base = demo.app.frame_index();
        for (int i = 0; i < opt.hold; ++i) {
            cur = static_cast<std::size_t>(i);
            hold[cur].t = std::chrono::duration<double>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
            const core::Stopwatch w;
            demo.app.step(demo.app.fixed_dt());
            hold[cur].frame_ms = w.elapsed_ms();
        }
        cur = hold.size();
        core::set_zone_sink({});
        demo.app.finish_gpu();
        for (const auto& [index, timings] : passes_queue) {
            if (index >= hold_base && index - hold_base < hold.size()) {
                for (const core::PassTiming& t : timings)
                    hold[index - hold_base].passes.emplace_back(t.name, t.ms);
            }
        }
        if (opt.hold_out != nullptr) {
            if (std::FILE* f = std::fopen(opt.hold_out, "wb")) {
                std::fprintf(f, "i,t,frame_ms,declare,execute,submit,npasses,passes_sum,passes\n");
                for (std::size_t i = 0; i < hold.size(); ++i) {
                    const HoldFrame& h = hold[i];
                    double sum = 0;
                    std::string ps;
                    for (const auto& [n, v] : h.passes) {
                        sum += v;
                        ps += n + "=" + std::to_string(v) + ";";
                    }
                    std::fprintf(f, "%zu,%.6f,%.4f,%.4f,%.4f,%.4f,%zu,%.4f,\"%s\"\n",
                                 i, h.t, h.frame_ms, h.declare, h.execute, h.submit,
                                 h.passes.size(), sum, ps.c_str());
                }
                std::fclose(f);
                std::printf("  hold: %d render-only frames -> %s\n", opt.hold, opt.hold_out);
            }
        }
    }
"""
src = src.replace(anchor, hold, 1)
if '#include <chrono>' not in src:
    src = src.replace('#include <algorithm>', '#include <algorithm>\n#include <chrono>', 1)
open(p, 'w').write(src)
print('patched', p)
