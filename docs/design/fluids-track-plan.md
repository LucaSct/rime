# The fluids track — water, fire, smoke: the substrate adjudication, and three bricks

- Status: **working paper, superseded where it disagrees by [ADR-0042](../adr/0042-fluids-track-reopened.md).**
  The owner ratified this plan's recommendation (fork now, hybrid gated) on 2026-09-17; ADR-0042 is
  the decision record, and this file is kept for the brick-level detail the ADR deliberately does not
  repeat.
- Date: 2026-09-17
- Answers: the owner's 2026-09-16 ask for a water + fire + smoke track, and the open question of
  heightfield-vs-hybrid for Track FL. (It cites the session handoff of that date, which was never
  committed; the ask itself is recorded in ADR-0042's Context.)
- Methods note: everything below the flag icon is verified against the tree on 2026-09-17 (paths and lines given). Numbers marked **est.** are reasoned estimates, not measurements, and are labeled; measurements are cited to their `docs/perf/` file or ADR.

> **Read this first — what ADR-0042 corrects in the text below.** The paper is committed as written
> (pronouns neutralised, nothing else changed), so these claims stand in it and are wrong or out of
> date. ADR-0042 carries the corrected versions.
>
> 1. **Sequencing.** "All after m17.10" (§0, §3, §7) → the owner chose **strictly after M18
>    closes**, with no interleave (ADR-0042 Ruling 2).
> 2. **The GPU→frame coupling "~1:1"** (§1 table, §1's posture line, fx1b's budget answer) was taken
>    from ADR-0041's 2026-09-07 m17.8b table, which ADR-0041's **2026-09-17 amendment withdrew**:
>    the unpinned retake measured the material's render-only GPU cost (~0.11 ms) and found the
>    gameplay-frame effect *smaller than its own noise floor* — "not, on the data gathered so far,
>    shown to reach the gameplay frame". The expected `frame` "19.849 → ~20.2–20.7" rests on that
>    coupling and does not stand either.
> 3. **"The GPU is ~8% occupied"** → **11% at p50, 14% at max** (1.841 and 2.407 ms of 16.600).
>    ADR-0041's "8%" was an arithmetic slip, corrected 2026-09-17.
> 4. **The hybrid's trigger** is three clauses here (§4.1) and **four** in ADR-0042 Ruling 1 (a
>    `sim.block` headroom clause is added).
> 5. **"m12.p and m13.p both cut fx1b before measuring"** → it was left *contingent* at M12.0 and
>    again at the M12/M13 split (ADR-0036:80, :106), and never built or measured.
> 6. **"The two-argument M8.4-era call"** → `samples/10-destructible-wall` calls the three-argument,
>    family-less `emit_burst(min, max, intensity)` overload (`dust.hpp:97`).
> 7. **"m11.5's 400-debris rosters"** → no such witness exists under that name; the cross-peer
>    convergence witness is `destruction_net::shared_state_hash`, and the ≥400-debris floor is
>    ADR-0035 §1's, asserted in `tests/blockkit/block_standup_test.cpp`.
> 8. **The m17.8b harness concerns (§6)** are no longer open: ADR-0041's 2026-09-17 amendment dropped
>    the clock filter for an unfiltered paired estimator and narrowed the claim to what the hold loop
>    measures. Its evidence is in [`docs/perf/m17.8b-hold/`](../perf/m17.8b-hold/).
> 9. **Line citations into ADR-0041 and `docs/ROADMAP.md`** are as of 2026-09-17, before both files'
>    same-day edits; expect them to be a few lines off.

---

## 0. The one-paragraph answer

Land the **ratified two-substrate plan first, the unified particle-grid substrate second, gated**. Three bricks, all after m17.10, all cross-cutting (the tracks' own convention: "interleave under mainline-first", ROADMAP:1640): **fx1b** — the compute-sim scale-up, un-cut, which *finally plugs the 21-day-orphaned fx1a pass into the block* and puts the first smoke on anyone's screen; **fl1a** — the heightfield + buoyancy, Track FL reopened at exactly the scope ADR-0035 §5 ratified, consuming the ADR-0026:142 seam; **fx1c** — "fire drives lights", the deferred clause, which costs almost nothing because it adds *rows to a light buffer the engine already carries every frame*. The hybrid (PIC/FLIP/APIC, MPM above it — the owner's intuition) is **not cut**: it is the track's recorded destination, with its trigger written into one new ADR (0042), because its 3–8 ms GPU and its slot both belong *after* the ratified M18 question, and — the load-bearing fact — **fx1b is the hybrid's particle half either way**, so sequencing costs nothing that the hybrid would have bought.

---

## 1. What the repo actually says (verified, with the two facts the brief missed)

The brief's established facts check out. Two additions change the plan's shape:

1. ✅ **The RHI exposes compute, and — more than that — compute already runs *inside the render graph*.** `create_compute_pipeline(ComputePipelineDesc)` / `bind_compute_pipeline` / `dispatch` / `bind_storage_buffer` / `bind_storage_image` since M5.2 (ADR-0021; `engine/rhi/include/rime/rhi/device.hpp:55-57`, `command_buffer.hpp:116-127,182-195`, `resources.hpp:235`). Five `.comp` shaders ship in the engine — `cluster_cull.comp`, `sdf_compose.comp`, `ddgi_trace.comp`, `ddgi_blend_irradiance.comp`, `ddgi_blend_visibility.comp` — all dispatched by ordinary passes in `engine/render/src/lighting/{clustered,ddgi,sdf_clipmap}.cpp`, with SPIR-V baked by the ADR-0008 offline cook (the `.comp.spv.h` includes). **A particle-sim dispatch is the sixth member of an existing pattern, not a new mechanism.** In particular the sim→draw dependency (fx1b's particle buffers → fx1a's vertex read) is a *declared graph edge*: ADR-0019's barriers derive from the pass declarations, so the classic compute/graphics sync bug class is already owned by infrastructure.

2. ⚠️ **fx1a has never been consumed by anything.** Grep across `samples/` for `FxParticlePass|emit_burst|DustField|impact_dust|muzzle_flash|vfx::|ParticleField`: only `10-destructible-wall/main.cpp:166,200,205` — the M8.4-echo: it *emits* headless impact dust (the two-argument M8.4-era call) and checks `coverage()`, drawing nothing (it predates the pass). `99-the-block` matches **none** of those: the block does not even simulate, let alone draw, FX. The 27 structural claims contain no FX claim. So the owner — who "loves to watch smoke" — has *never seen this engine's smoke*, because the only consumers of the m13.1a/b machinery are `tests/render/fx_particle_test.cpp` and `tests/destruction/events_test.cpp`. This is m17.8b's recorded lesson verbatim — "a capability shipped without a consumer is a capability that has not been tested" (ADR-0041:809-811) — now 21 days old (m13.1a landed 2026-08-27, ROADMAP:621). **Brick 1's first deliverable is therefore wiring, not compute.**

The resource picture that decides every budget answer, both verified:

| resource | state | source |
|---|---|---|
| CPU `sim.block` p99 | **15.468 vs 6.000 ratified — 2.58× over, the largest breach** | ADR-0041:889, 2026-09-07 amendment |
| CPU sim composition | ~31% solve, ~28% contacts+broadphase, ~32% shared math; **~92% of awake bodies in ONE island**, so island-level parallelism's ceiling is 1.09× — the paydown (graph colouring / Jacobi, under the ADR-0026 determinism contract) is *ranked with M18, not schedulable here* | ADR-0041:457-467, 586-599 |
| GPU | **1.841 ms p50 / 2.407 ms max across all twelve passes — ~8% of the 16.6 budget** | ADR-0041:643-644 ("Ruling 6") |
| GPU→frame coupling | GPU cost **passes through to the gated frame ~1:1** (measured, disjoint ranges: +0.130 ms `frame.submit` → +0.132 ms `frame` p99) | ADR-0041:849-861, the m17.8b A/B |
| precedent for adding cost | m17.8's textured ground: landed *after* the budget bricks, priced by its own A/B, +0.67% frame p99, arms separated by a ledger counter, not memory | ADR-0041:820-870 |

So the track's posture writes itself: **the CPU is the binding resource and the GPU is 8% occupied — every simulation this track adds belongs on the compute side, and every added GPU millisecond passes honestly through to the gated frame, which is still 3.2 ms over.** That 3.2 ms is M18's assigned debt (ADR-0041:595-599); the track's costs are 3–10× smaller than that debt, each one measured and gated. That is exactly the covenant m17.8b established, and these bricks inherit it: **no brick lands without its own `docs/perf/` A/B** (ADR-0035 amendment A2; docs/perf/README.md:178).

One precondition needs an honest reading: ADR-0035 §5 made fx1b "contingent on the ledger showing the CPU sim binding" (fx_pass.hpp:47-50 repeats it: "if and only if the work ledger shows the CPU sim binding — not on appetite"). Taken at its 2026-08-20 wording, the condition is **not met and cannot be**: the CPU sim is capped at 200 particles (dust.hpp:87), it costs effectively nothing, and nothing ever measured it binding — m12.p and m13.p both cut fx1b before measuring. What binds today is the *feature*: 200 CPU particles against an ask of watching smoke, and a 4096 draw cap (`FxSettings::max_particles`, fx_pass.hpp:70) against the same. The honest resolution, which the ADR-0042 below records: the criterion transfers from "the clock binds" to "the *caps* bind the owner's ask" — with the one measurement the original wording wanted, taken *before* the compute work: the stub's drops are **counted silently** today (`emit_burst` "silently drops the rest", dust.hpp:91-92 — no counter, which itself violates the repo's now-standard guardrail-5 "give every skip/drop/defer path a counter"). fx1b's first hour adds that counter; whether the field saturates at 200 during a collapse is then a *ledger fact* rather than an appetite.

---

## 2. The adjudication: one substrate or two?

### 2.1 The two options, priced

**Option A — the ratified fork (status quo, reopened):** Track FX stays a GPU *particle* substrate (no pressure solve; buoyancy-velocity, curl noise, per-particle growth); Track FL stays a *heightfield* (shallow-water on h(x,z), two-way buoyancy into the CPU physics). Two modules, two machinery sets, no shared solver.

**Option B — the unified particle-grid substrate:** one hybrid solver — particles + background grid, PIC/FLIP/APIC for the momentum exchange, MPM above it for phase-change/granular (Stomakhin et al. 2014) — carrying water, smoke, fire-heat and granular dust as fields over one state.

| | A: particles + heightfield | B: hybrid particle-grid |
|---|---|---|
| **sim cost (GPU)** | fx1b: **est. 0.25–0.9 ms** at 64k particles (integration+curl ≈0.05; the *fill* of 64k additive billboards dominates). Water, if compute: **est. ≤0.1 ms** at 256² | **est. 3–8 ms**: P2G/G2P (1M APIC-class particles ≈ 2–4 ms), pressure/divergence solve at 128³ (est. 1–2 ms, Jacobi/GS in dense-DSM-class memory), advect + reseeding. 256³ (≈0.15 m cells over the 40 m block — enough to *fill a room* legibly) is **est. 8–20 ms: not 60 Hz** on the 3060. At 128³ a 1 m hose is 3 cells wide: pouring works, splash finesse does not |
| **sim cost (CPU)** | fx1b ≈ 0 (the 200-particle sim it removes costs more than the dispatch it adds); heightfield **est. +0.1–0.3 ms** into `sim.block` | **est. +0.2–0.5 ms** (dispatch, emission, witness readbacks) |
| **engineering** | fx1b: days-scale — every piece exists (pass, families, event glue in tests, the m17.4 host-visible ring, graph barriers). fl1a: one new removable module + the heightfield collider shape ADR-0041:496-497 *already carries as a named placeholder*. fx1c: rows in an existing buffer | a new subsystem class: 3D grid allocator + boundary conditions, particle↔grid-consistent reseeding, a divergence-free projector, GPU-desync surface (MC/point-splat), multiple barrier-dense dispatch chains. Call it **3–6 bricks, 1–2 milestones** at this repo's demonstrated pace |
| **what it buys** | all three asked domains visible within weeks; the buoyancy seam (ADR-0026:142) finally exercised; the heightfield machinery shares its *shape* with the M18 terrain question without coupling to it | the one-solver thesis — Stomakhin 2014 *is* the "common base": water, melting, heat, smoke in one formalism; 3D pouring/filling; smoke advected by a divergence-free field instead of curl-noise; the honest "highly advanced" label |
| **what it forecloses** | pour/splash/fill-a-room *in that system* (2.5D: one h(x,z) — waves, buoyancy, flood-filling yes; vertical structure no); and, if the hybrid comes later, **a second water stack to maintain** (~1 module + 1 surface pass + 1 collider variant) | if started now: **M18's ratified slot** (virtualized geometry, "Approved by Luca, 2026-09-03", ADR-0041:212) or the 09-06-precedent discipline of adding cost only after the budget bricks; *any* water for roughly 1–2 months. If deferred: nothing — see 2.2 |

The ms figures above are estimates and say so; nothing here has been built. But the *shape* of the comparison does not depend on their precision: A's total added cost (~0.3–1.0 GPU, ~0.1–0.3 CPU) is one third to one tenth of B's, and B's cost lands in a frame that is currently 3.2 ms *over* its ratified budget with its only listed paydown explicitly ranked into M18.

### 2.2 Recommendation: A first, B as the gated destination — and why that is not a dodge

Four reasons, in the order they bind:

1. **The budget asymmetry orders it.** ADR-0041 Ruling 1 ("the budget is earned before the bar is spent") plus the 09-06 facts: the CPU is 2.58× over with its paydown ranked into M18; the GPU idles at 8%. Option A's bricks are *shaped* by that asymmetry (everything heavy goes to compute); Option B *is* a 3–8 ms GPU subsystem — affordable *eventually*, but it bargains with M18's ratified slot, and Ruling 1's own note says what happens when a milestone absorbs an unrelated subsystem: it stops being a milestone (ADR-0041:326-331).

2. **The ask is three domains plus watchability, not one formalism.** The owner asked for water, fire, and particle/smoke, and especially to *watch* smoke. A delivers something watchable in all three within weeks — because every component (pass, families, event glue, light buffer, buoyancy seam) already exists and only needs wiring — while B delivers one domain deeply, months out.

3. **fx1b is the hybrid's particle half. The sequencing is therefore free.** This is the load-bearing continuity fact: PIC/FLIP/APIC = particles **plus** a grid. The particle side — SoA state, emission, witnesses, the draw consumption — is *exactly* what fx1b builds, and the tracker's own stub says so (dust.hpp:15-19: "the real GPU-driven FX system (track fx1) replaces this whole module"). fx1c's velocity/temperature fields and fx1b's SoA buffers are the deposit-side plumbing a FLIP/P2G stage consumes. Nothing in A is throwaway under B; B merely *joins* a grid stage to machinery that already exists. Conversely, building B's grid *first* saves nothing, because the particle half must be built either way and is the half the owner's #1 ask needs.

4. **The heightfield is the opposite of throwaway, and its cost is the cheap one.** Even a shipped hybrid keeps a heightfield for oceans/river/flood-scale water — that is what production engines do. The ratified 2026-08-20 scope (ADR-0035 §5) is precisely right for what it priced; the only thing that changed since is that the owner *asked*, which ADR-0035 itself named as the reopening condition ("the ADR-0026 substrate seams stay intact for whenever it opens", ROADMAP:1639).

**The verdict on the owner's intuition, plainly:** it is right about the *destination* and wrong about the *next move*. Water and smoke *do* share a base — the hybrid, and the 2014 phase-change MPM above it is literally that. But the shared base's **particle** half is the FIRE+SMOKE side, which the repo already correctly paired (fire+smoke on the particle substrate, ROADMAP:1630-32); it is *water* that forks off, and water's cheapest honest substrate remains the ratified heightfield. Racing to unify water+smoke first buys the label "common base" at the price of the only cheap, ratified, seams-intact path to actually watching any of it.

### 2.3 The literature, each assigned a slot (so no citation decorates)

- **Kim/Thürey/James/Gross, Wavelet Turbulence (SIGGRAPH 2008, 10.1145/1360612.1360649)** → **fx1b's detail mechanism, and the "highly advanced" discount.** Its trick — synthesize divergence-free high-frequency velocity as a *post-process* over any cheap base velocity, no linear solve, embarrassingly parallel — is exactly the right shape for a 60 Hz budget, and fx1b's base velocity (buoyancy + curl noise) satisfies the paper's only requirement: *a* base field, not a pressure solve. Cost: **est. +0.2–0.5 ms GPU** for 2–3 octaves. This is how 64k-particle smoke gets the look of a 256³ Eulerian sim.
- **Jiang/Schroeder/Selle/Teran/Stomakhin, APIC (TOG 34(4) 2015, 10.1145/2766996)** and **Stomakhin et al., Augmented MPM (TOG 33(4) art.138, 2014, 10.1145/2601097.2601176)** → the **ADR-0042's named substrate** for the gated hybrid: APIC's affine-velocity transfer is the momentum carrier; Stomakhin's heat/phase-change scalars ride the same particle state. Recorded as the design, *not* built here.
- **Solenthaler & Gross, Two-Scale Particle Simulation (SIGGRAPH 2011, 10.1145/1964921.1964976)** → the hybrid's later splash layer (coarse solver + fine detail particles) — listed in the 0042's alternatives, priced then, not now.
- **Ladický et al., Regression Forests (SIGGRAPH Asia 2015, 10.1145/2816795.2818129)** → **not doing** (§4): it buys 4×-coarser sims by *inference*, and this repo has no inference dependency to bolt it to; correct AFTER the hybrid exists to accelerate.
- (Added, flagged: **Tessendorf's FFT-ocean notes, 2001** — not on the brief's list, commonly cited for ship-scale water. Not doing: the block needs *interactive, disturbance-able* water, which is precisely what displacement-synthesis FFT oceans are bad at. Heightfield shallow-water is the ratified and correct choice; the citation exists so nobody "fixes" it later without this argument.)

---

## 3. The three bricks

Order: **fx1b → fl1a → fx1c**, all *after* m17.10 (the never-cut re-measurement — a track brick's baseline must be the settled one, per the m17.3d rule that a baseline measured before the change cannot judge it). All three are track bricks riding the cross-cutting latitude (ROADMAP:1640), *not* milestone entries: they do not touch the M17 ladder's own bricks, and they leave the M18 opening question (virtualized geometry vs terrain; ADR-0041:349-352) exactly as ADR-0041 wrote it. Sequencing vs M18 is the owner's call (§7) — the honest note is that fx1b is the cheapest thing on the board that makes the owner happy, and m13.1's precedent (a track brick as M13's *opening* brick when it carries the milestone's thesis) does not apply here, because fx1b carries nothing of M18's thesis. It just gets the owner their smoke.

### Brick 1 — **fx1b: "the compute-sim scale-up, un-cut — and finally plugged in"**

**Continuity:** the name *is* the continuity. fx1a (m13.1a) = the draw pass; fx1b (ADR-0035 §5, ROADMAP:2088-89) = the compute scale-up, specified 2026-08-20, cut by M13's closure (the `audio → fx1b → m13.p's tail` order, ADR-0036:106). Un-cutting it also discharges the stub's own outstanding promise, open since M8.4 (dust.hpp:15-17: "the real GPU-driven FX system (track fx1) replaces this whole module").

**Scope — two halves, the split being sequencing, not scope** (the house pattern: ROADMAP:649-651, "fx1a is sequenced in two halves… Both land in m13.1"):

- **fx1b.1 — wire + count (no compute).** The block's Demo consumes the *existing, tested* machinery: destruction/weapon events → `emit_burst` with the three families (the glue already written and proven in `tests/destruction/events_test.cpp` — PartDied→dust, IslandDetached→dust+smoke, shot→flash; copy the recorded authoring lesson too: multiply by detach magnitude, never floor, or the quietest collapses have no smoke, vfx/README.md:46-51) → `GpuParticle` conversion (consumer's glue, ~30 lines) → `FxParticlePass::add` after forward-PBR, before tonemap, `FxSettings::enabled = true` in the block's visuals. Plus the missing counters: **drop counts on the *sim* side** (the silent 200-cap) alongside the pass's existing `particles_dropped()`/`particles_drawn()` (fx_pass.hpp:121-125), surfaced into the work ledger, and one new structural claim (~#28): *a collapse draws LingeringSmoke* — `particles_drawn > 0` and `≤ cap` after a fracture, because a glue that drew nothing and a glue that was never called are the same test result. **Earliest visible smoke: the end of this half — days, not weeks — the owner watches ≤200 CPU-simulated, additive, *unlit* billboards rising off a collapsing slab.** Honest adjective: *glowing fog*, not smoke — the lighting read is the next half.
- **fx1b.2 — compute.** The simulation moves to a compute dispatch: SoA particle state in two StorageBuffers (position/velocity/size/age/params), an emission ring fed from the CPU-side deterministic SplitMix64 scatter (the *provable* half stays CPU: the families, burst placement, and initial state remain bit-reproducible and unit-testable GPU-free, exactly as the stub header promised), integration+retirement on GPU, curl-noise + buoyancy + the Wavelet-Turbulence detail octaves. The 200→~65k cap (the value is an A/B'd decision; the *mechanism* — count the drops, never truncate silently — is what the brick commits). The draw side reads the same buffers: the consumer's per-frame copy dies, and the sim→draw dependency becomes a declared graph edge. The witnesses move with the sim — count and Σsize²·alpha coverage computed by one tiny GPU reduction, read back as 8 bytes/frame — so the ledger keeps its current shape instead of silently going green-vacuous (the "all-zero row is the proof that a stage never ran" discipline, ADR-0041:486-489). The gate discipline extends *structurally*, not by care: with `enabled = false` there is no declare → no dispatch, no buffers, no barrier — the ADR-0032 §11 byte-identical argument, one level deeper, and the existing proof re-run over the new passes. Billboard *shading* joins here, because it is what makes it watchable as smoke: particle.frag samples the clustered-light buffer and one shadow cascade (both already exist as bound-able resources; ~30 lines of GLSL + two descriptors) — lighting×fade is still additive, so the unsorted, commutative, order-free design (fx_pass.hpp:41-44) survives *unchanged*. No atlas (fx_pass.hpp:45-46 reserves it as its own brick), no sorting, no depth-write: none of those this brick.

**Done when — each with the assertion that lets it fail** (ADR-0035 §1 table style):

| clause | the assertion that lets it fail |
|---|---|
| the owner sees smoke | in `--headless`, after the scripted collapse, `vfx.particles_drawn > 0` — against the negative control `enabled=false`, whose frame is byte-identical to pre-fx1a (the ADR-0032 §11 proof, re-run over the compute path) |
| the counts survive the GPU move | the GPU-computed count readback reconciles with emissions-minus-retirements to within the retirement model, *and* the 65k cap provably drops with a counted, never-silent, excess (0-drops-forever would fail the cap's purpose; unbounded growth fails the budget) |
| the sim did not get slower where it matters | the A/B (gate off/on — the *existing* structural off; same binary, cleaner than m17.8b's manifest trick) shows the GPU delta in `frame.submit`/`frame.render` with disjoint ranges, **est. +0.25–0.9 ms, expected ~0.4** — and `sim.block` p99 unchanged within its measured noise floor (the m17.8b control: +0.09% there) |
| the CPU debt was not increased | the 200-particle CPU `simulate()` is gone; the remaining CPU work is dispatch+emission+readback, **est. ≈0, budgeted ≤0.05 ms**; if `sim.block` regresses beyond noise, the brick explains it before it lands |
| the smoke reads as smoke | same-position radiance differs measurably between a lit and a shadowed region of the puff (the M13.1a coverage-delta pattern, now against lighting, not just age) — and the baselines' *relative* gate (the 10% regression rule, ADR-0035 amendment A2) still holds against the brick's own `--commit` run |

**Seams:** `engine/render/src/fx_pass.{hpp,cpp}` + `particle.vert/.frag` (draw-side; no interface change), one new sim pass + shaders beside it, `engine/vfx` (families/emission/witness *nouns* — the module stays `core`-only, guardrail 2: "nothing depends on it", README:53, so the FX=cosmetic property survives: fx is not in the replicated state, hence GPU-FP jitter between peers is provably harmless to every shared-state-hash proof, which never counted it), the block's visuals + one ledger wiring, the preset string if the A/B arms need naming. Compute rides ADR-0021's pattern and the graph's barriers (ADR-0019); buffers ring via the m17.4 `push_frame_buffer` seam — fx1b becomes that ring's second customer, which is the pleasantest possible evidence the m17.4 design was right ("ring what the CPU rewrites").

**Budget answer:** the GPU delta (est. 0.25–0.9) lands into the *measured* 2.407-of-16.6 occupancy and passes through 1:1 to the gated frame — expected 19.849 → ~20.2–20.7, into the gap M18's within-island work owns; the CPU delta ≈ 0 against the binding resource; the brick files its own `docs/perf/` A/B on its own commit, arms separated by the ledger, per the covenant. The measurement waits for the m17.8b harness concerns (handoff §2 items 1–2) to be settled first — those outrank this brick and are already §4.2 of the handoff.

### Brick 2 — **fl1a: "the heightfield and the buoy" (Track FL reopened, at its ratified scope)**

**Continuity:** this *is* ADR-0035 §5's Track FL, word for word — "CPU heightfield water with two-way buoyancy coupling" — reopening under its own recorded condition (the seams were kept "for whenever it opens"). Nothing about the 2026-08-20 ruling is reversed; it is *executed*, four weeks of repo-time later, by the ask.

**Scope:** new removable module `engine/fluids` (guardrail 2: depends on `core` + the ADR-0026 physics *interface*; physics never learns it exists — the dependency direction of ADR-0026:147-149). In:

- The field theory, GPU-free and unit-tested: 256² (est. 20 B/cell ≈ 1.3 MB) explicit shallow-water — two half-steps, 5-point stencil, no linear solve — as a *CPU reference implementation* that also carries the proof suite in the ADR-0026:134-136 house style: **mass conservation** (Σh·ΔA drift within a stated margin, checked with margins, not exactly), **energy decay bound** (no blow-up across N frames under the collapse's disturbance spectrum), and the **analytic buoy test** (a buoyant disc at rest displacement: net vertical force = 0 at h = displacement/area, the "analytic rest heights" pattern).
- **Two-way coupling, the 09-06-informed design:** deposit = bodies' velocities into the sim's uniforms (CPU→GPU, ~KB); retrieve = one small readback — N buoyant bodies × 16 B once per tick — riding the **m17.4 borrow seam** (`Device::wait_and_borrow`, built precisely for "the GPU finished, the submission not yet reclaimed"). Physics reads *last* tick's field: one frame (16 ms) stale, i.e. 0.1–1.6 m of wave travel — physically invisible for buoyancy, and *deterministic-input* by construction. Buoyancy never enters the prediction replay path: the player is a kinematic capsule (ADR-0035 §3, "a pure move function over physics queries", not solver-integrated), so the 12.4-style replay of `step_character` is untouched; the *debris* — which buoyancy does touch — is server-authoritative and covered *for free* by the existing bit-exact `shared_state_hash` convergence witness (m11.5's 400-debris rosters). The coupling inherits its proof; nothing new must be invented.
- The **heightfield collider shape** — the placeholder ADR-0041:496-497 explicitly carried into m17.8's wake: sphere/capsule/box-vs-heightfield contacts (8-corner sampling for the box). That closes a named, waited-on register entry, which is the cleanest kind of new work: someone already promised it.
- GPU compute for the production field (one dispatch, ~0.05 ms est.) — the *09-06 facts* amendment: the ratified "CPU" in "CPU heightfield water" was the M12-era GPU-free-proof convention; the ledger now says the CPU is the 2.58×-over binding resource and the GPU idles at 8%, so the production path computes and the CPU reference *is the proof oracle* (agreement within a stated relative margin on the fields; the structural invariants hold in both). Alternatively the reference *is* the production (all-CPU, +0.1–0.3 ms into `sim.block`) — the 0042 records the recommendation (compute) and the CPU fallback, because the honest cost difference is ~0.2 ms of the scarce resource vs ~0.05 of the idle one.
- Visible water = its own brick (fl1b): a displaced-grid surface entity + the metal-PBR-dusk trick (fresnel via metallic≈1/roughness≈0.05 — the palette trick that makes the road carry highlights, recorded at palette.cpp per ADR-0041:771-775, now amplifying the same physics: SSR reflects the water because the water writes depth; the water reflects the buffer). Vertex-stage storage-buffer read = the proven fx1a pattern. Out: refraction, foam, spray — §4.

**Done when:**

| clause | the assertion that lets it fail |
|---|---|
| the field conserves what it must | Σh·ΔA drifts < 1e-6 relative/frame over a 10 000-tick disturbance run, against a deliberately leaky integration failing the same bound |
| the buoyancy is two-way, provably | dropping a buoyant body from above the surface exchanges momentum with the field: the wave it makes returns and the *body* re-bobs — asserted as an energy/decay bound, not eyeballed; a one-way-coupled (deposit-only) control fails the re-bob |
| the convergence proofs still hold *with* coupling | 400-debris + buoyant bodies: `shared_state_hash` converges bit-exactly at quiescence — the m11.5 witness, now covering the buoyant subset; if the coupling broke determinism, this existing proof goes red *by itself* |
| the frame pays what the plan said | the A/B (fluids entity present/absent) shows `sim.block` +0.1–0.3 ms (CPU path) or ≈0 (compute path) and the readback's cost visible and bounded; the 27-claim battery stays green, +2 claims (mass, buoy) |

**Seams:** `engine/fluids` (new, removable), `engine/physics` (the heightfield shape — interface level, per ADR-0026:142's register), the block (a flooded street district = the water's worldbox — this is 2.5D: a bounded basin, not the whole 76 m ground), one ledger wiring, the m17.4 borrow seam (its third customer).

**Budget answer:** CPU +0.1–0.3 ms (or ~0) into `sim.block` — *after* m17.10, riding the same M18-owned gap as fx1b's pass-through, with the delta measured and gated by the 10% relative rule. GPU ≈0.05–0.1 ms. The honest ceiling note: this brick does not pretend to solve the 9.5 ms `sim.block` overage — it adds 0.6–2% to the measured 15.5 (2–5% of the ratified 6.0) and *inherits* the wait for M18's paydown; if the owner wants the paydown strictly first, this brick is the one that waits (§7).

### Brick 3 — **fx1c: "fire drives lights" (the deferred clause, landed)**

**Continuity:** the third deferred clause of the track's own sentence (ROADMAP:1631-32: "fire drives lights, smoke reads the M10 lighting data" — the second half of which fx1b.2 just discharged; "fire-as-light stays deferred behind its seam", ADR-0035 §5). The seam it was deferred behind is the one this brick finally occupies: events → FX, FX → *light*.

**Scope:** a reflected `FxLight` component + the consumer glue: muzzle-flash event → a 1-frame radiance spike at the muzzle; IslandDetached → a 2–3-frame fireball light with decay; *and the coupling that earns the clause*: a short-lived ignition state on struck flammables whose light intensity curve **drives the LingeringSmoke emission rate** of the *existing* fx1b machinery — fire, smoke, and light closing one loop, which is the owner's "watching" trifecta in a single demo. Implementation fact that makes this the cheapest brick of the three: the clustered pipeline's light buffer is **CPU-written every frame** and already ringed by m17.4 ("clustered (2)"); FX adds ≤8 rows of `LightDesc` to it. No new pass, no new GPU technique — the cluster cull, the forward-PBR shading, and the DDGI trace all read the same buffer, so **fire GI is free**: the flicker reaches the DDGI probes on the next trace, exactly the mechanism M10's "GI updates as the scene changes" proof already exercises — this brick inherits that witness shape (a light-Δ → irradiance-Δ assertion, the `0.04 → 0.74`-style covered-pixel proof, ROADMAP:604).

**Done when:**

| clause | the assertion that lets it fail |
|---|---|
| the flash connects | the muzzle+1-frame sample's radiance on a lit wall exceeds the pre-shot sample's — the "shot feels connected" clause (ADR-0035 §1) finally measured *by FX*, against a no-FxLight control that proves the light is the cause |
| the GI noticed | the DDGI-irradiance checksum changes by more than its measured noise floor when the fire lights vs doesn't (the m10 proof's light-Δ variant; if the GI *doesn't* move, the brick failed quietly — that is the vacuity lesson, now guarded) |
| the loop closes | smoke emission rate = f(fire intensity): two runs differing only in the ignition state produce measurably different LingeringSmoke coverage curves (against the fx1b.1 counters — which is why this brick *must* be third) |
| the ledger stays honest | `fx.lights_peak ≤ 8` counted; the clustered pipeline's known-32+lights behavior untouched (the count headroom was the milestone's *input*, never its output — the MegaLights question stays m17.6's) |

**Seams:** `engine/render` (≤8 rows into the clustered light buffer path), `engine/ecs` (one reflected component — registered via the worldkit profile, *not* the four hand-rolled lists whose drift m17.8a just documented, ADR-0041:726-731), the block's glue, the ledger.

**Budget answer:** the cheapest: **est. +0.05–0.15 ms GPU** (8 extra lights through cull+shade, measured, not asserted), **est. ≤0.1 ms CPU** (the flicker eval + 8 rows), same covenant: the A/B, the committed baseline, the 10% relative gate. Where it comes from: the same 8%-occupied GPU, and a light buffer the engine was already carrying.

---

## 4. What this plan does NOT do, and why

1. **The hybrid particle-grid now** (PIC/FLIP/APIC/MPM): priced in §2.1 — est. 3–8 ms GPU, 3–6 bricks, 1–2 milestones — and *deferred, not cut*, by the §2.2 argument (fx1b is its particle half; M18 holds the ratified slot). Its trigger lives in the ADR-0042: (i) the gated `frame` ≤ 16.6 (M18's paydown), (ii) M18's terrain question answered, (iii) the owner still wants the unified formalism after watching what fx1b+fl1a+fx1c actually look like. Any one of those can flip it honestly; none is knowable today, which is the point of the trigger.
2. **Volumetric fog / froxel smoke**: the brief's own flag, confirmed — no froxel pass exists. A participation-media timeline is a new *structural* pass family (new timeline, new barriers, ~est. 0.5–2 ms GPU) and duplicates what billboard+shadow-sampling already achieves at 65k scale. Correct *after* the billboards demonstrably stop satisfying — when the owner says the smoke looks flat, not when a planner predicts they will.
3. **Sorted/alpha blending and the FX texture atlas**: fx1a's own header reserves both as separate decisions (fx_pass.hpp:41-46) and the reasons still hold: additive is the one commutative blend; the fragment-computed soft disc needs no cooking. Sorting+alpha would *re-do* the coverage proof's radiance assumptions. Not this track.
4. **SPH water**: the roadmap's own "shallow-water + SPH literature" aside. SPH is the grid's *predecessor* — particle-neighborhood constraint iterations, no divergence freedom, worse 60 Hz stability — and it shares nothing with the fx1b particles the way FLIP does. Rejected with reasons so it stays rejected.
5. **Regression-forests / ML super-resolution** (Ladický 2015): right technique, wrong decade for this repo — it presumes the trained model + inference dep this tree doesn't have. Revisit *inside* the hybrid, where it accelerates something that exists.
6. **FFT-ocean** (Tessendorf): the block needs interactive, disturbance-able water in a 40 m basin; displacement-synthesis oceans are the wrong tool there. (Flagged as my addition to the brief's literature; see §2.3.)
7. **MegaLights-class FX lighting** (hundreds of billboard-emitted lights): FX emits ≤8 *rows*; the "find where clustered falls over" question is m17.6's measurement, not this track's technique. The boundary is stated so the temptation has a recorded cost.
8. **Multiplayer FX wire**: none needed, provably — the replicated destruction/weapon *events* are the shared cause, each peer's FX is the locally computed effect, and fx is outside the replicated state (vfx/README.md:53), so GPU-FP jitter is cosmetic by construction. Adding FX bytes to the snapshot would be spending the m11.5 budget to fix nothing.
9. **Terrain coupling**: fl1a's water-bed is a bounded 256² basin; the M18/M19 terrain-LOD question (does virtualized geometry subsume it?) is *adjacent, not coupled* — answering it early would be guessing, the exact failure Ruling 5's posture warns about (ADR-0041:281-282). The 0042 notes the adjacency; this plan does not resolve it.
10. **Flame propagation / heat-field solver**: the fx1c "fire" is event-ignition + light-curve + smoke-rate — the *visible* fire. Cellular/field propagation of burning across parts wants the temperature scalar that the hybrid's MPM carries naturally (Stomakhin 2014's heat transport); until the grid exists it would be a bespoke second heat sim. Deferred *with the hybrid*, by design.
11. **MoltenVK/compute-perf work**: compute on MoltenVK is CI-proven-adjacent (lavapipe gates the device tests; macOS CI compiles and link-checks) but its *performance* is unmeasured. Power > portability (VISION #2); nobody measures it until someone ships on the Mac.

---

## 5. What needs an ADR versus what is a roadmap edit

**One ADR: 0042 — "Track FL reopens; the substrate ruling; and what 09-06 changed."** (The brief's instinct that the hybrid "needs an ADR" is correct — but it is *one* ADR covering one decision, and the hybrid itself is *not* what's decided; its *trigger* is.) Contents, all ruling-shaped:

- **The reopening.** Track FL's "no" (ADR-0035 §5, reaffirmed at the label level by ADR-0036:104) was always conditional — "for whenever it opens" (ROADMAP:1639). The opening event: the owner's 2026-09-16 ask (handoff §3). Recorded, cited, done.
- **The substrate ruling.** A (particles+heightfield) now; B (hybrid particle-grid: APIC transfer, MPM heat/phase-change scalars — Jiang 2015, Stomakhin 2014) as the recorded destination, **gated** on the three-clause trigger of §4.1, with Two-Scale (2011) and the Forests (2015) as B's *own later* options. This is the only genuinely *decision* part — it prices both, states what each forecloses, and thereby survives its own supersession.
- **The "CPU" re-read.** The ratified "CPU heightfield water" carried the M12-era GPU-free-proof convention; the 09-06 ledger (one-island CPU at 2.58×, GPU at 8%) is new information. Ruling: production = compute, proof = the CPU reference, agreement = a stated margin; the all-CPU fallback documented. This is the kind of ruling that would otherwise happen silently in a header comment — the A8/A10 disease this repo keeps inoculating against.
- **The buoyancy seam.** Confirmation that ADR-0026:142's register + the 0041:496-497 carried collider-shape placeholder are discharged *as written* — sphere/capsule/box vs heightfield, interface-level, physics depends only on `core`+`ecs`+its own contracts.
- **The cut order** (house rule): fx1c → fl1a. **Never cut:** fx1b.1 (it is the owner's ask), the witnesses, the covenant.

**Roadmap edits — exactly one place.** The *Cross-cutting tracks* paragraph, ROADMAP:1629-1642, gains: (a) the fx1b contingency discharged (the criterion transfer, §1's last paragraph, cited); (b) Track FL: reopened → ADR-0042, at the ratified scope, queued behind m17.10; (c) the three brick names. Nothing else:

- **Not VISION.md** — the ADR-0036:110-117 precedent is verbatim on point: this changes neither intent nor scope, the UE5-class/`feels-right` words already cover fluids, and re-labelling churn is what the precedent warns against.
- **Not the M17/M18 milestone entries** — the tracks' own convention says they interleave, *not* that they edit the ladder; a milestone that absorbs an unrelated track stops being a milestone (ADR-0041:326-331).
- **Not ADR-0026** — its seam is *consumed*, not amended; ADRs are append-only and nothing in it became false (the 2026-08-20 amendment's mover-lesson is the cautionary precedent for *untrue* records; none here).
- **Not CLAUDE.md** — the brick-delivery covenant (docs/perf on perf-touching bricks) already covers every brick above; adding a fluids-specific line would be the ADR-0035 §2c habit-vs-ledger mistake in miniature.

---

## 6. Risks and uncertainties, flagged rather than smoothed

- **The 19.849 / 15.468 figures are quoted from ADR-0041's 09-07 amendment, whose own A/B the handoff declares stale-by-construction** (the binary changed 24 minutes after the table; the harness reports were wiped with /tmp). The absolute numbers are probably sound (handoff finding 2) but formally await the re-take. Every comparative claim in this plan therefore binds to *the brick's own committed baseline*, not to these.
- **The m17.8b harness concerns 1–2 are unresolved** (per-arm clock-filtered population drift; hold-loop measuring a narrower claim than "reaches the frame"). Both of fx1b's headline proofs want exactly the harness; settling those, handoff §4.2, is a declared prerequisite — this plan queues rather than re-derives.
- **GPU-FP determinism is an assumption, not a settled fact.** The fx1b sim is atomic-free, fixed-dispatch-order, same-binary-same-device → deterministic *in practice*; the hash witness (double-run, bit-identical live-set) is what turns the assumption into a guarded invariant, and any later atomic "optimization" will flap it *loudly*. Cross-device determinism is already ruled a non-goal (ADR-0026:144); nothing here re-opens it.
- **Every sim-shape number in §2–3 is an estimate** (labeled est.): 64k-256k particle counts, 256² grids, 128³ hybrids, 3–8 ms. The 09-06/07 amendments' lesson — the mechanism was wrong, not just the number, and *in the direction that flattered the brick* (ADR-0041:853-861) — applies to this document with full force. The bricks' A/Bs, not this plan, are the measurements.
- **The 200-cap saturation is believed, not measured** — the stub's drops are *silent* today (§1). fx1b.1's counters are the first hour's work precisely so the criterion transfer in the 0042 rests on a ledger fact, not on this paragraph.
- **Validation-layer output into pass/fail** (the m17.8b lesson, ADR-0041:812-814): fx1b adds three storage buffers and the graph's first sim→draw edge; the layout-assert discipline (fx_pass.hpp:85's `static_assert` tripwire) extends to every new interface block, and the validation log joins the CI-gate evidence, or the BC7-ghost repeats.
- **Capacity claims (65k, 256², ≤8 lights) are placeholders the bricks measure** — per the founding perf-ADR lesson, a gate nothing can fail is not a gate: each brick's "done when" asserts *with margins* and against a named control, never "looks right".

---

## 7. What the owner actually needs to decide

One decision, one input:

- **Ratify A-then-B** — i.e., commission ADR-0042 (§5) and let the three bricks queue behind m17.10 in the order fx1b → fl1a → fx1c. The honest alternative is B-now: a 3–8 ms GPU subsystem *now*, which buys the one-solver label early at the price of M18's ratified slot and any water for 1–2 months. If the "highly advanced" identity outranks watching the three things soon, B-now is defensible; this plan's §2.2 argues it is not, and says why.
- **The sequencing ballot this plan cannot cast:** ADR-0041:349-352 already scheduled M18 to open with *two* tracks and an ordering question (virtualized geometry vs terrain, terrain→M19 if both survive). This plan adds a third claimant. Whether fluids precedes, joins, or follows M18's ratified work is the owner's priority call — every variant above is written to survive any answer.

*Adjudicated and written by GLM (glm-5.3-flash) — planning only; the only byte this session added to the tree is this file.*
