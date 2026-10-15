# Final production native pacing proof

Success:451native renders, exit0, using CURRENT production main/AppConfig/application/render libraries. No report-only Application override is linked. Production cfg.vsync=false reaches the RHI; the actual swapchain log confirms MAILBOX. The source snapshot exactly matched production main.cpp at compilation:fdacd713fddbedf5bf15ada04323a461f0d548d5e1e712a62f88d471295f8d9e.

The root's30idle handshake ticks and warm_scope_view() ran unchanged before the native loop. The external copy only schedules aim/yaw, records frame-ID CPU zones and real intervals, and stops automatically. Actual optical activity is visuals->last_scope_ldr.is_valid(), not scope-renderer object existence.

The compositor initially configured522×1029, then1038×1025; all steady measurements use1038×1025. Two startup resize events occurred. This is a larger pixel workload than the earlier945×1025 FIFO/MAILBOX comparison; do not present results as a matched-resolution speed ratio.

| Phase | Intervals | Mean ms | p50 ms | p95 ms | p99 ms | Max ms | >16.7ms |
|---|---:|---:|---:|---:|---:|---:|---:|
|Hip|120|3.418|3.247|4.067|4.100|4.249|0|
|Requested ADS phase|120|6.359|6.447|7.136|7.896|8.165|0|
|Turning|120|3.226|3.159|4.008|4.058|4.068|0|

Fully feed-visible ADS:113intervals, mean6.540ms, p957.177ms, max8.165ms. The first7requested-ADS intervals still have the pupil closed because30settling renders cover little real time at the faster loop rate; all states remain in the raw data.

First actual feed opens at render187 (aim0.873). Its following real interval is6.137ms: declaration1.456ms, command execution4.566ms, acquisition0.004ms, presentation0.083ms. The prior cold MAILBOX entry was28.030ms. Production optical warmup therefore removes the observed entry hitch in this capture. Return-to-hip's first closed-feed interval is3.848ms. Every hip/ADS/turn interval is below16.7ms, with0/360above33.3ms.

Acquisition means: hip0.005ms, requestedADS0.139ms, turning0.027ms; no recurring FIFO-scale wait remains. Initial presented-loop warmup still has one221.590ms interval, mostly213.157ms declaration. Optical prewarming occurs before this loop's timer and its loading duration is not measured here; this report does not claim shorter total startup.

All451renders retain304terrain tiles; no shots, detonation, or production errors occurred. Frame-ID zones align intervalN to prior render's sim/acquire/declare/execute/present plus current event pump; no nested simulation child zones are double-counted. Recording writes occur after the loop.

Limits: these are native loop intervals, not compositor displayed-frame timestamps or input-to-display latency. Windowed GPU query resolution remains unsupported, so no live GPU duration is claimed. MAILBOX may replace pending frames. Clocks vary and unrelated resident llama-server/desktop activity remains; this is a bounded diagnostic proof, not a clock-controlled benchmark. No user-preview process was relaunched and no repository files were edited.

Evidence: window.production.zones.frames.csv (raw indexed execution zones), window.production.zones.intervals.csv (correctly joined intervals), window.production.zones.summary.json, window-production.log (actual mode), window-production-gpu-clocks.csv, window-production-provenance.json, window-production-source-check.json, plus preserved source and compile/link commands. GPU/CPU are free.
