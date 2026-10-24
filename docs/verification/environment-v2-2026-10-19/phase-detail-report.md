# Phase detail V2 (Claude, base b5bf53b)
Owned files only. README/ADR not edited (outside ownership): README still says "64 frames,
~6.5 MB, static frost normal" — stale after this change; integrator should update.

Design: normal atlas = 1 static frost map + 8 liquid levels x 32 phase frames (257 maps), baked as
slope-space blend (1-w)*frost + w*water. Tints: 9 maps. 266 maps x 87,380 B (128^2 RGBA8, 8 mips)
= 23,243,... B exactly 266*87380 = 23,243,080 B (22.2 MiB). normal_scale is now constant 1.
Pure helper: engine/fluids_render/{include/.../phase_maps.hpp,src/phase_maps.cpp}.
Endpoint: liquid rounds to k/8; k==0 -> the single static map, so eps-liquid == frozen (test).
Tests: standalone g++ run of tests/fluids_render/phase_maps_test.cpp: 6 cases pass. GPU test
unchanged and NOT run (deferred to integrator).
