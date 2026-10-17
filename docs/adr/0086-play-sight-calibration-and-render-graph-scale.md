# ADR-0086: right-eye sights and render-graph naming at streamed-world scale

- Status: Accepted for the playable sample
- Date: 2026-10-15
- Extends ADR-0083, ADR-0084 and ADR-0041's pass-identity rules

## Context

The owner reported unusable rifle aiming and severe stutter while accepting the
sniper's larger scope. The existing performance tape does not load the play
environment, streamed terrain or optics; it cannot diagnose this workload.

An identical-scene hardware diagnostic found 324 passes for one view and 643–644
for scope plus parent. Each view declares 304 terrain passes. The name uniquifier
restarted at suffix `#1` for every repeat, scanning existing passes for every
candidate. This cubic pattern spent about 41.6 ms declaring an aimed frame;
the parent alone spent 37.3 ms after the scope had declared its tiles. The scope
also rendered at hip fire, despite its exit pupil being entirely closed. The
128-pass timestamp limit truncated even a single streamed view.

The SCAR's two opaque panes shared a housing material, and their painted reticle
was not collimated. The M4 rear peep was 14.976 mm above the eye line. Centring
only that peep still left its front post 4.890 mm low over a 370.56 mm sight radius.

## Decision

1. Index exact pass names and each base's next unchecked suffix. Names accumulate
   until reset, preserving the original smallest-free suffix, literal collision
   and opt-in folding rules. Clear both indices at reset. Do not drop, merge or
   rename existing rendering work.
2. Reserve 2,048 timestamps: 1,024 pass pairs, 16 KiB of query results per encoder.
   Read only the written prefix and retain the overflow warning. A partial sum
   must never be presented as total GPU time.
3. Test optical transmission against the shader's exit-pupil support after posing
   the weapon. Outside its support, use a black 1×1 HDR input in the SAME glass and
   reflection pass. Render the independent optical world whenever transmission
   can be visible. Parent projection, magnification and lens size stay intact.
   This supersedes ADR-0083's cost statement that mere selection needs a second
   scene rendering.
4. Pose each weapon relative to the right eye. Seat its measured butt on a fixed
   right shoulder and blade the arm frame by 60 degrees. Solve full wrist frames
   after hip/ADS motion, bob and recoil. Centre the M4 rear aperture and rotate
   the complete gun about it to align the front post as well.
5. Separate both SCAR glass hulls from the opaque gun. Recognize their measured
   geometry within one submesh and refuse changed artwork that does not match.
   Its original near hull supplies the physical mask. Collimated 1× transmission
   samples parent world HDR preserved BEFORE first-person colour writes, adding
   Fresnel reflection and an optic-axis red ring/dot. This needs no second world
   camera and never samples the attachment being written. Housing depth still
   occludes the glass.
6. Consume number-key down events as well as final levels: a complete tap between
   frames must select its weapon. Live play defaults to the requested MSR and
   shows the selected weapon's controls outside aiming.
7. Expose the RHI's existing presentation preference through `AppConfig::vsync`,
   defaulting to FIFO as before. The playable FPS opts into tear-free MAILBOX,
   with FIFO fallback where unsupported. Keep image acquisition and event pumping
   visible as separate CPU zones. This is a sample choice, not a universal platform
   default or a claim about displayed frame rate.
8. Warm the optical scene's resources before interactive input, using three
   blocking frames through the SAME application graph cache. A temporary graph
   would discard its warmed allocations. Keep optical TAA history empty so the
   first actual aim does not reuse a stale loading image. Mere selection still
   does no optical rendering during interactive hip fire.

## Evidence and limits

The real-art proof retains its rendered 11→54 pixel target (4.909×), optical-input
sensitivity and grip checks. Synthetic GPU tests cover 644 complete pass timings,
literal/fold/reset naming collisions, asymmetric screen-space transmission,
off-axis transparency, depth occlusion and one-frame borrow lifetime. They need
no downloaded art. The calibrated M4 passes 5,376 installed-rig poses without
clamping, with 28.938 mm minimum reach margin.

At 952×514, diagnostic sniper hip frames measured about 64.8→6.4 ms, aimed frames
66.0→10.8 ms, and aimed declaration 41.6→1.15 ms with the same 643 passes. Complete
corrected GPU intervals were about 2.1 ms hip and 3.8 ms aimed. These serialized
offscreen diagnostics are NOT a live FPS acceptance or clock-stable baseline:
desktop activity and resident GPU memory differed. See the committed diagnostic
under `docs/perf/play-sights-2026-10-15/`.

Native profiling then isolated repeated long intervals to acquisition, not CPU
rendering: long FIFO intervals averaged 33.48 ms with 28.35 ms acquiring; short
intervals averaged 16.63 ms with 11.28 ms acquiring. At the same 945×1025 viewport,
an actual-mode-verified MAILBOX control removed all 87 steady intervals over
33.3 ms across 360 hip/aim/turn samples. Scope initialization still cost one
28 ms interval, motivating the loading warmup. These observations identify
presentation backpressure on this Wayland/NVIDIA setup, not an individual driver
or compositor defect. Displayed cadence and input-to-display latency were not
measured. The standard hardware benchmark refused filing because of clock drift
and the existing authored-scene simulation budget; its rejected report is retained.

Astra and Fable independently advised live-scene attribution, complete timing,
separate view histories and geometric sight calibration. We agree. A broader
shared world-lighting architecture is deferred: the measured CPU defect needs no
renderer ownership rewrite. A proposed overwrite of an already-written timestamp
query was rejected before use because Vulkan requires resetting queries before
reuse. No such capture is evidence. Consultations cost $0.460315 total.

The transmitting scope still owns independent lighting resources. Streaming
recentres can still wait for retired GPU readers. The tree-side dark shape was
reproduced on reflective puddles, but a denser SSR march retained the transition;
an incorrect intersection is not established. These changes claim no shadow or
SSR fix.
