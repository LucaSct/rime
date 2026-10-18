# `99-the-block` — Milestone 13's "done when"

> a destructible urban block (M8 + M10 + M11 + M12) runs at a playable frame rate and *feels* right

The vision demo. Every piece of that sentence had been built and proved on its own; this is the
first process that runs them together — a cooked destructible city block, the M10 lighting stack, a
replicated server, a predicted client, a first-person camera, a window and a mixer, in one frame.

Open the editable expanded city and launch the full weapon game from its toolbar:

```sh
./scripts/city-editor.sh
# In the editor: Run game (save succeeds first, then a separate player window opens).
./scripts/export-city.sh "$HOME/rime-exports/my-city"
"$HOME/rime-exports/my-city/the_block"
```

The default saved map is `build/release/city-editor.rscene`; relaunching preserves
it. Supply `--scene /absolute/path/map.rscene` to either script for another saved
map. The generator `the_block --write-city NEWFILE` refuses overwrites. Saved
maps own their sun/sky lighting; the generated no-scene Play path retains sunset.
The editor previews intact slabs plus the same procedural lamp/barrier volumes;
the full runtime owns player input, weapons and fracture parts. Runtime draw IDs
are excluded from editor saves. See the [creator workflow](../../docs/creator-workflow.md)
for navigation, effects, maps/events/animation, Unity/Frostbite comparisons and
current limitations. The city export is a Linux development folder with prepared
weapons/arms and attribution, not a production installer or closed performance gate.

```bash
build/dev/bin/the_block --headless          # the CI-gated proof
build/dev/bin/the_block --play              # walk around it and shoot
build/dev/bin/the_block --world-smoke       # streamed collision, fast travel and recovery
build/dev/bin/the_block --world-gpu-smoke   # streamed drawing, uploads and evictions
build/dev/bin/the_block --viewmodel-smoke   # grips, held aim, rendered scope zoom and residency
build/dev/bin/the_block --environment-smoke # native scenery, water reflections and firing feedback
build/dev/bin/the_block --headless --ppm frame.ppm   # also write the intact frame as an image
build/dev/bin/the_block --headless --idle   # the control: nobody touches the block
build/dev/bin/the_block --headless --ticks 1100   # run the script past where CI stops caring
```

`--play` controls: **WASD** move · **mouse** to look, or hold **right-drag** when the window could
not lock the cursor · **left click** fires · **F** detonates the charge on the hero building ·
**1** selects the SCAR-H · **2** selects the M4A1 · **3** selects the MSR ·
**hold right mouse or Q** aims (also during drag-look) · **Esc** exits.
Releasing aim returns to the hip pose; losing focus releases held input. Short clicks survive
render frames with no simulation tick. Accepted shots produce muzzle flash, a tracer and hit
sparks; cooldown-refused inputs produce no feedback. Damage remains server-authoritative.

Live play starts at sunset and waits for **F** before detonating the scripted charge. Seeded
terrain extends beyond the original street: nearby collision is installed before simulation,
and six moving LOD rings keep ground visible in the distance. Generated tiles are released as
they leave the resident region and are never cached to disk. The GPU ring holds 304 tiles
(about 9 MiB of dressed tile resources); this excludes the rest of the renderer and physics memory.

The aimed MSR ocular sits 50 mm from the eye and fills about 85% of frame height.
A real rendered target measures 11 → 54 pixels through it (4.91× on the RTX 3060),
while the surrounding view retains its normal field of view. The aim pose passed
5,376 runtime arm poses with no unreachable grip or near-plane clipping.

The reported disappearing dark blob under trees is **still under investigation**.
Normal-correct foliage fixes white undersides; it did not fix that ground symptom.
Controlled captures reproduced a moving dark shape on a reflective puddle. A denser
reflection march preserved the transition; an incorrect intersection is not established.
Shortening the shadow range did not improve the ground controls.

Live play draws the MSR by default. Keys **1/2/3**, including quick taps, switch between the three
prepared viewmodels without changing the replicated weapon or movement input. The MSR's German
Schmidt & Bender optic uses a separate camera for apparent 5× magnification inside its lens;
the outer view keeps its field of view. The SCAR has a transparent collimated 1× sight;
the M4 aligns its rear aperture and front post. Stocks seat at the right shoulder and
the camera represents the right eye. Weighted arms
solve full wrist frames after weapon motion; fingers retain separately authored grip poses. Their
meshes and normal/metal-roughness textures are cooked once from
`assets/viewmodel/held-scar-h.glb`, `held-m4a1.glb` and `held-msr.glb` into the build tree; building `the_block`
automatically runs `the_block_viewmodel_cook` when either source changes. Source downloads under
`assets/viewmodel/source/` are references only and are not cooked. `--viewmodel-smoke` renders
all three guns offscreen, checks held input/aim, real apparent zoom, visible coverage and swaps, and checks that mesh, material
and texture counts stay resident. Headless/world-smoke paths do not load the models.

The playable environment needs no external art: weathered wall maps, normal/roughness detail,
grass/dirt terrain and verges, 26 trees, grass tufts, seven puddles, a park pool and clouds are
generated once from fixed seeds. Water uses screen-space reflections and environment fallback;
it is opaque and has no refraction. Decorative trees do not add collision bodies. The local GI
volume fades continuously into sky lighting and retraces fixed world-probe ray sets to avoid
stationary noise and abrupt edge-probe relighting. See [ADR-0084](../../docs/adr/0084-local-gi-support-and-play-environment.md).

`--environment-smoke --ppm /path/preview` renders street, puddle and park captures, and checks
texture/reflection/cloud sensitivity above unchanged controls. This proof runs on clean clones
without the optional weapon art. Headless and performance workloads retain their authored scene.

Pass-name indexing avoids repeated linear suffix searches across hundreds of streamed terrain
passes. All 643 aimed passes fit the expanded timestamp pool. The sniper skips its invisible
world view outside the exit pupil while retaining glass reflections. Hardware diagnostics and
their limits are in [ADR-0086](../../docs/adr/0086-play-sight-calibration-and-render-graph-scale.md).

This prototype returns the player to a supported spawn beyond eight kilometres on either
horizontal axis or below its recovery floor. Recentring currently waits for old GPU readers,
so tile changes can hitch; unlimited coordinates and asynchronous streaming are later work.
See [ADR-0081](../../docs/adr/0081-playable-procedural-terrain.md).

`--cooked <dir>` points at the nine `.rdest` files; the viewmodels use their separate build-tree
manifest. CTest cooks the block patterns and ground material; the CMake viewmodel target cooks
weapon art as a build dependency.

## Status: 29 structural claims pass — the frame-rate clause remains open

It did not start that way. m13.5 shipped this demo with 20 claims passing and one **reported as a
known defect**: *the collapse does not stay local*. A charge on one building flattened the whole
block, across the 12 m gap to its neighbours and then across the street. `--idle` — the same block
with a player who never moves or shoots — reproduced it, so it was not the player, the rifle, or the
charge's size.

**The cause was damage tuning, not physics.** `damage_threshold` is cooked per pattern and the
fracturer's 5.0 default was set for M8's small test wall. A building slab part is two orders of
magnitude heavier: measured here, contact impulses reached **669 kg·m/s** against a threshold of 5
and a part health of 1.0, so **729 of 799 contact ops were instant kills** and the cascade could not
damp. m13.6 put `--damage-threshold` / `--damage-scale` on `rime fracture` — they had never been
reachable from the CLI at all — and gave each cook tuning matched to its part mass.

Now the same charge levels the hero building, seriously damages the two buildings beside it, and
leaves the far side of the street untouched. The `locality` line prints the per-building survivors
and `damage` prints the op population that made the defect legible.

## What the gate covers

| | |
|---|---|
| scale | 2,016 parts (floor 1,500) · 36 local lights (floor 32) |
| player | the avatar replicates, and the **predictor seeds** — the camera rides the predicted pose, the wiring m13.3a named and deferred to here |
| destruction | shots reach the block · parts fall · peak live debris ≥ 400 · the visual budget stays above the live one |
| networking | ops replicate · every composition check matches · nothing unresolved · both peers agree on `shared_state_hash`, **within a bounded settle** |
| audio | destruction drives the mixer · nothing clips |
| render | the cull considers every part and rejects **1,983 of 2,051** · every leaf finds its mesh · the frame comes back lit |
| collapse | the far side of the street is untouched · most of the block is still standing |

"A playable frame rate" is the one clause CI cannot judge, and it has not been established here.
Lavapipe is a CPU rasteriser, so a millisecond there is a statement about the runner's mood.
Counts stay in `--headless` where CI can
fail on them forever; the clock belongs in a `--perf` mode (m13.p), on hardware whose fingerprint
goes into the report. That is the 11-lit-rooms split, ADR-0035 §2b.

## Five things that only broke when composed

Each was invisible in isolation, and each is commented at its site:

1. **Register every module's components, not just the scene's.** The first draft registered the four
   the block's `.rscene` needs and none of the four the session needs. The block stood up and drew
   perfectly; the predictor never seeded (no `RigidBodyHandle` meant no body to predict against),
   destruction replicated zero ops, the peers disagreed, and the mixer heard nothing. Four symptoms,
   one cause.
2. **Never make a structural change inside `world.query()`.** `ServerReplicator::replicate` moves the
   entity to another archetype and reallocates the chunk vector the query is walking; the loop aborts
   on the next chunk index. Collect, then act.
3. **A server replicates its destructibles; a client must drop its own.** Damage ops are addressed by
   NetId, so the entities need one. If the client also loads them from the scene it ends up with 280
   instances — 140 taking damage and 140 standing there forever, which draws as a building that never
   falls behind the one that does.
4. **`apply_palette` runs when destructibles arrive, not once at load.** It derives the look from the
   reflected `SlabRole`; a client's destructibles arrive tick by tick and miss a single call at load.
   2,016 leaves existed and followed the sim perfectly while the renderer considered 35 entities — a
   leaf with no material is not a draw.
5. **The cross-peer witness is `destruction_net::shared_state_hash`.** Not
   `DestructionWorld::state_hash()`, which folds physics body ids and local instance indices: two
   peers in perfect agreement will still disagree on it.

## Two claims that were nearly true by accident

The m13.2d lesson — a proof pinned to an accident — in two new places.

**The render claims are made on a frame drawn while the block is standing**, chosen by the condition
*"the client holds every part"* rather than by a tick number. At tick 0 the leaves do not exist yet;
a few ticks later replication has still delivered only part of the block over a lossy 80 ms link. A
fixed tick measured 35 props and passed "the cull did work" while drawing no block at all.

**The tape is tight because the tail is expensive.** Every tick after the collapse simulates ~1,500
debris bodies, so 1,100 ticks took 226 s in Debug on a real GPU — under ASan on lavapipe that is a
timeout, not a test. It is 430 ticks at 37 s now, with every claim still reachable; `--ticks N` runs
it longer by hand.

## Shape

One `Session` owns a `ScriptedNetwork`, a server peer and a client peer, at 80 ms RTT and 5% loss —
in-process and deterministic, because the claim is about composition at scale and a real socket would
add a nondeterministic variable that says nothing about it. The server owns the block; the client
loads the level, is sent the destructibles, predicts its own player and draws. **What is on screen is
what a client holds**, which is the only honest thing to show in a networked demo.

One `Demo` serves all three modes. What differs is where the input comes from and who owns the loop —
never what is standing there.

Deliberately **not** re-proved here: prediction against a prediction-off control
(`13-networked-player`), two clients agreeing on a broken wall (`12-networked-destruction`), and GI
mechanism (`11-lit-rooms`, `tests/render/gi_thesis_test.cpp`).
