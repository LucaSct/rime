# Native pacing attribution and MAILBOX counterfactual

The remaining repeated33ms stalls are acquisition waits on this native FIFO presentation path, not CPU rendering or the event pump. Selecting MAILBOX in an external diagnostic removes those stalls. This identifies presentation-mode/backpressure behavior on this Wayland/NVIDIA setup; it does not separately identify the Vulkan driver, compositor, GPU power policy, or an individual wait inside acquisition.

Both captures:451 native production-loop renders, explicit NVIDIA ICD, RTX3060, same945×1025 steady viewport,60Hz display,304 streamed tiles, environment and sunset, real fixed-tick simulation. Same hip/aim/turn schedule, no injected mouse/keyboard events, shots, or detonation. Only the second binary replaces Application's translation unit with an external identical copy adding sd.vsync=false before create_swapchain. The swapchain's actual selected-mode log reads vsync/FIFO versus mailbox. No repository edits.

CPU zones are keyed by Application.frame_index, not callback completion order. IntervalN is joined to prior-frame sim.tick/acquire/declare/execute/present plus current-frame pump, because run() samples dt after pumping. Only the parent sim.tick zone is summed. Distinct sequential regions are added once; residual includes work outside those regions.

| FIFO steady interval group | Count | Mean interval ms | Acquire ms | Sim ms | Declare ms | Execute ms | Present ms | Pump ms | Residual ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
|<20ms|240|16.627|11.277|1.139|0.954|3.125|0.084|0.0014|0.047|
|>33.3ms|87|33.481|28.347|0.772|1.005|3.206|0.090|0.0018|0.059|

The17ms long/short interval difference is almost entirely the17ms acquisition increase. Render CPU work stays similar. Acquisition includes ensure_extent, in-flight fence wait, and vkAcquireNextImage; this capture does not split those calls.

| Phase | FIFO mean / p95 / max ms | MAILBOX mean / p95 / max ms | FIFO acquire mean ms | MAILBOX acquire mean ms |
|---|---:|---:|---:|---:|
|Hip|22.225 / 33.625 / 33.963|3.567 / 4.267 / 7.945|17.777|0.007|
|ADS phase|22.220 / 33.550 / 34.219|6.516 / 7.038 / 28.030|14.997|0.143|
|Turning|22.215 / 33.547 / 34.425|3.381 / 4.088 / 4.198|18.139|0.011|

FIFO has87/360steady-phase intervals>33.3ms; MAILBOX has0/360. MAILBOX still has one28.030ms scope-entry interval (declare14.385ms, execute12.575ms), versus FIFO65.891ms entry (acquire28.305ms, declare23.213ms, execute13.220ms). Startup remains≈592–596ms in both. First scope construction therefore remains a separate one-time CPU/resource hitch worth warming before interactive ADS.

Timing caveat: the same30-frame aim settling period covers less real time when MAILBOX renders quickly. Its first6ADS-phase intervals still have the pupil closed; first opening occurs at frame186 and contributes the28.030ms interval at187. The remaining113feed-visible ADS intervals average6.489ms and max8.022ms. FIFO ADS has120/120feed-visible intervals. MAILBOX simulation ticks per rendered frame drop naturally because rendering is faster; simulation remains driven by the real60Hz accumulator. This is not reduced simulation work per second.

These are loop/CPU wait measurements, not measured displayed-frame cadence or input-to-display latency. MAILBOX may drop superseded frames; a300Hz rendering loop does not mean a60Hz monitor displays300FPS. Full GPU timings remain unavailable on production windowed present. No recommendation to change every platform's default follows from one machine. A bounded sample-level selectable low-latency mode, with FIFO fallback and an honest actual-mode log, is supported by these measurements; the root owns product changes.

No competing build/test/game ran. Unrelated resident llama-server was untouched. GPU clocks were not fixed: both monitored captures span210–1942MHz including startup/idle, and active frequencies vary. This is diagnostic evidence, not a ratified benchmark. External artifacts preserve exact copied source, commands, hashes, raw frame-ID zone CSV, correctly joined interval CSV, summaries, actual-mode logs, and500ms clock samples. Main files: window.zones.* (FIFO), window.mailbox.zones.* (MAILBOX), application-fifo.cpp/application-mailbox.cpp and window-mailbox-provenance.json. GPU is free; no unbounded preview relaunched.
