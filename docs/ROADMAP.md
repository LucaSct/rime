# Rime Roadmap

The big-picture map from "empty repo" to the **vision demo**: a destructible urban
block with dynamic lighting, networked, at a playable frame rate (see [VISION.md](../VISION.md)).

**How we use this roadmap.** These are *milestones* — the map, not the turn-by-turn
directions. Each milestone is decomposed into small **bricks**, and every brick is
planned again before it's built. A milestone is **"done" only when its proof runs** (a
`samples/` demo and/or CI gate) — never when it merely compiles. We re-plan at each
milestone boundary; time estimates come at brick-decomposition, not here.

> **Update (2026-08-27) — m13.1a: the FX particles are finally DRAWN, and M8.4's deferred pixel
> proof lands.** Milestone 13 ("The Block") opens with Track FX brick fx1a's load-bearing half.
>
> `engine/vfx` has simulated deterministic CPU billboards since M8.4 and **nothing has ever drawn
> them.** The M8.6 sample fed the field from destruction events and self-checked `coverage()` — a
> CPU number standing in for pixels that did not exist, with its own header promising that "the
> m8.6 GPU pass's coverage-delta pixel test confirms it on screen". That test is here, a milestone
> late.
>
> `render::FxParticlePass`: additive HDR billboards, six vertices synthesised per particle from
> `gl_VertexIndex` with the particle array read straight from a storage buffer by
> `gl_InstanceIndex` — no vertex buffer, no index buffer, no per-particle mesh. Depth-tested,
> depth-write off, and **unsorted on purpose**: additive blending is commutative, so it is the one
> blend mode where the transparency-ordering problem does not arise.
>
> **Off is structural, not careful.** With the gate off the pass is never declared into the graph,
> so there is no bind, no barrier and no attachment load — the frame is byte-identical to a build
> without the file. A pass that ran and drew zero instances would be *almost* that, and ADR-0032
> §11's regression bridge cannot be built on almost. The proof compares raw bytes, and separately
> shows that `intensity = 0` is provably a *different state* from the gate being off (the pass runs,
> binds and blends, and draws nothing visible) — because a design where "dark" and "absent" are
> indistinguishable makes the baseline untestable.
>
> **The coverage delta, measured:** a burst takes on-screen radiance 0 → 20.27 while `coverage()`
> goes 0 → 0.078; as the puff ages both fall monotonically, together, to 3.64 and 0.011; when the
> field retires, the screen is exactly black again. Sampled at six points rather than two, because
> "up then back to zero" is also true of a pass that flickers.
>
> fx1a is sequenced in two halves — this is the draw pass, the gate and the proof; the three effect
> families (impact dust, lingering smoke, muzzle flash) and the destruction/weapon event glue are
> **m13.1b**. Both land in m13.1; the split is sequencing, not scope.
>
> **m13.1b lands with it: the three families and the consumer glue.** `impact_dust`,
> `lingering_smoke`, `muzzle_flash` — differing in ways a test can name rather than in taste. Smoke
> has **negative gravity** (it rises) and **growth** (it expands as it dissipates, so a long-lived
> puff does not just fade in place); a flash lives under a tenth of a second with no gravity, because
> nothing falls in 60 ms. Gravity and growth are per PARTICLE, which is what lets one field — and one
> draw — hold all three while smoke rises and dust settles in the same puff. The proofs assert the
> families against **each other**, because three identically-parameterised puffs that all work would
> prove one family three times and read as coverage.
>
> The glue stays in the consumer, as the M8.4 fan-out rule requires: a `PartDied` becomes dust, an
> `IslandDetached` becomes dust *and* smoke, a shot becomes a flash at the muzzle. **One authoring
> lesson worth recording:** scaling the smoke purely by the detach's `magnitude` looks obviously
> right and emits *nothing*, because an island can detach with a magnitude of exactly 0 — the damage
> impulse went to the part struck dead, and the event is explicitly not emitted for a killed part's
> own chunk. A structural collapse should always smoke and the impulse scales it *up*; multiply
> instead of flooring and the quietest collapses, where a wall simply gives way, are the ones with
> no smoke at all.
>
> **Next:** m13.2 — block content, view-frustum culling, and ADR-0032 C6's debris visual retirement.

> **Update (2026-08-27) — MILESTONE 12 ("The Player") COMPLETE.** m12.6 closes it with
> `samples/13-networked-player`, the proof [ADR-0036](adr/0036-milestone-split-player-and-block.md)
> said the split created: a headless server and two clients on a 20%-loss link, driving the same
> scripted tape and differing in exactly one boolean — **client 0 predicts, client 1 does not.**
>
> The control is a PEER IN THE SAME MATCH rather than a second run, because the cheapest way to be
> wrong about "prediction is the reason" is to compare two numbers that were never comparable. Same
> server, same tick, same scripted losses, same tape.
>
> ```
> own-input response   prediction ON  : 1 tick     prediction OFF : 7 ticks (round trip is 6)
> agreement            both clients match the server BIT FOR BIT at quiescence
> remote continuity    0 of 1679 drawn frames exceed 3 ticks of walking (worst 0.110 m)
> non-vacuous          packets dropped, commands lost outright, 4 corrections, 386 within tolerance,
>                      40 shots fired / 12 hit, the wall lost 40 of 40 parts to gunfire
> ```
>
> It self-gates: the binary exits non-zero if any clause fails, so the clauses live in the program
> rather than in a CMake assertion nobody reads. **GPU-free** — it never opens a window; the playable
> client is m13.3.
>
> Two things the run made visible that were worth writing into the sample rather than tuning away.
> Starved player-ticks track the loss rate almost exactly (272 of ~1300 at 20% loss) because a lost
> packet costs one tick's command and the next packet's redundancy window then delivers two — that
> is the design working, and it is printed rather than asserted. And the two handshakes RACE on a
> lossy link, so pairing a client to its avatar by spawn order compares one client's prediction
> against the other player's authoritative state; the pairing goes through the NetId instead.
>
> **Next:** M13 "The Block" — m13.1, Track FX brick fx1.

> **Update (2026-08-27) — THE MILESTONE IS SPLIT: M12 "The Player", M13 "The Block"
> ([ADR-0036](adr/0036-milestone-split-player-and-block.md)).** ADR-0035 considered this cut and
> *deferred rather than rejected* it, leaving the seam at m12.5 / m12.6 and saying the decision
> wanted its own deliberate act. This is that act, taken after m12.5 landed — so the seam it
> predicted is not a forecast but a description of where the work actually divided.
>
> **The evidence at the seam.** m12.0–m12.5 closed with seven measured numbers, every one from a
> GPU-free, CI-gated proof: own-input response 1 tick with prediction against 6 without; predicted
> equals authoritative bit-for-bit at quiescence; 2 corrections in 124 pairings under 25% loss;
> 0.2 m divergence with reconciliation against 1.1 m without; 0 motionless frames of 120 where v1
> had 79; and smoothing proven to leave the simulation bit-identical. Below the seam is content, a
> GPU draw pass, a windowed client, audio, and a frame-time distribution only one machine on earth
> can measure — with an unratified budget and a gate ADR-0035 itself calls procedurally weak. One
> milestone could not have one "done when" for both.
>
> **The final demo does not move.** M13's "done when" is M12's old one word for word, and VISION is
> deliberately not edited: this re-cuts the map, not the destination.
>
> **M12 gains one brick it did not have — m12.6, its own closing proof** — because a milestone here
> is done when its proof *runs*, and every previous one ended in something a human could invoke.
> The numbers above currently exist only as MESSAGE lines scrolling past in CI, which is a milestone
> claim on trust.
>
> **Next:** m12.6 — `samples/13-networked-player`, and with it M12 closes.

> **Update (2026-08-27) — m12.5 COMPLETE: interpolation v2 and predicted-player smoothing, and
> the stutter m11.6 had been shipping.** Listed as *load-bearing-lite, cut last* — and it turned
> out to fix a defect the milestone had carried since M11.
>
> m11.6's interpolation blended previous→current over **exactly one tick**, which is right only if
> a value arrives every tick. Its proof moved its entity every tick, so that was the only case ever
> exercised. Loss, relevancy, the byte budget and simply-not-changing all break it: a value carrying
> N ticks of motion was played in one tick and held for N−1, so the mirror lurched and froze — at a
> rate set by how badly the link was behaving. Measured over a 3-tick interval: **v1 showed 79
> motionless frames of 120; v2 shows 0**, with a third of v1's worst single-frame jump.
>
> The span needs no clock, which is why the information was already there: the Delta header carries
> the server tick, and a *difference* of two server ticks has no origin to agree about. A gap above
> 8 ticks snaps rather than crawling (counted), and a value arriving mid-blend retargets from where
> the mirror is being drawn rather than from the newest value.
>
> **This is the third time this milestone that a proof asserted the right value and missed what
> mattered** (m12.3's transform write was the second). The pattern is not carelessness but a
> property of what is easy to assert: **state is easy to check and motion is not**, so a suite grown
> out of convergence proofs is systematically blind to how anything moves. Every position v1
> produced was correct.
>
> Predicted-player smoothing lands beside it: a correction's displacement is absorbed into a
> decaying visual offset, bounded so a large one is still shown at once. It is presentation-only and
> that is asserted rather than intended — two runs differing only in the smoothing constant produce
> bit-identical simulation trajectories over 250 lossy ticks while their drawn poses differ on 156.
>
> **Next:** m12.6 — Track FX brick fx1 (fx1a load-bearing: the GPU draw pass for the existing
> deterministic CPU sim, off byte-identical).

> **Update (2026-08-26) — m12.4 COMPLETE: prediction and reconciliation, the milestone's hardest
> brick.** The client runs the same `step_character` the server will run, immediately, and checks
> its work when the authority answers. ADR-0035 §1's "own-input response" clause is met and its
> control is measured on the same tape: **1 tick with prediction on, 6 without** — the round trip.
> Under 25% loss over 300 ticks, 124 pairings produced 2 corrections and 122 within-tolerance
> skips; divergence after 300 lossy ticks was 0.2 m with reconciliation and 1.1 m without; a run
> with 13 permanently-lost commands took 10 corrections and then agreed bit for bit.
>
> **§4's replay set was wrong, and only building it showed why.** It said "replay every `unacked()`
> command with sequence > q" — but `unacked()` retires on the `InputAck` frontier, which rides a
> *different* unreliable superseding stream from the snapshot carrying `q`, so the ack is routinely
> fresher (measured on 60 of 300 ticks). Replaying from it skips commands the server consumed and
> the client predicted, sliding the prediction backwards on every correction. The predictor keeps
> its own `{sequence, command, state}` ring instead, retired on the reconciled `q`.
>
> **And m12.3 had shipped a silent transform bug that all of its own green proofs missed.** The
> controller wrote only `WorldTransform`; `propagate_transforms` recomputes that from
> `LocalTransform` one step later, so the write was discarded every tick. An avatar whose
> `CharacterState` had walked to z = −3.29 had a transform and a physics body still at z = 0 — the
> capsule pushed nothing, and every *other* client mirrored it standing at its spawn point forever.
> It was invisible because `CharacterState` is what a controller test asserts on, and it was right
> the whole time. The lesson is the milestone's own turned on itself: **a proof that asserts the
> value a system computes has not thereby asserted that the value reached anyone.**
>
> **Next:** m12.5 — snapshot interpolation v2 and predicted-player smoothing (load-bearing-lite;
> the cut-last brick).

> **Update (2026-08-26) — m12.3 COMPLETE: the networked player, server-authoritative.**
> `engine/gameplay_net` joins `gameplay` to `replication` so neither knows the other: the consume
> loop, `PlayerRegistry`, the replicated `LastProcessedInput` pairing, and one message
> (`AssignPlayer`, claiming the shared tag registry's 0x80–0xBF block). `Weapon` v1 landed in
> `gameplay` alongside it — deterministic hitscan whose `RayHit::child` names the destructible part,
> with the damage call left to the consumer, so `gameplay` still never links `destruction`.
>
> **The number the brick exists to produce:** with prediction off, own-input latency is **6 ticks at
> 48 ms one-way** — exactly the round trip — and **0.30 m** of visible position lag while walking at
> 6 m/s. Measured with no clock synchronisation, by counting the client's own ticks between stamping
> a sequence and seeing it come back on its own avatar. ADR-0035 §1 asks m12.4 for ≤ 1 tick against
> this control; recording it *before* prediction exists is the point.
>
> Two things building it changed about the design on paper. **§4's loop needed a rate budget it did
> not mention** — one step per command hands a client a speed multiplier for editing its own send
> loop — and the surplus must be *dropped*, never deferred: `consumed_through` steps over commands
> the server will never act on, so dropping is honest and deferring would advance the frontier over
> commands still queued. And **the weapon's damage default was wrong by ~45×**: parts cook at 1.0
> health, the header said 45, and the end-to-end proof levelled all sixteen parts of its test wall
> on the opening shot before spending 399 more shots measuring rubble. An engine default that is not
> scaled against the engine's own cooked assets is a design claim nobody checked.
>
> ADR-0035 §6's **`server.despawn` backstop** shipped with it: `ServerReplicator::publish` now walks
> the live NetIds once per tick and retracts, warns about and counts any whose entity was destroyed
> behind its back — repairing a permanent phantom into a one-tick-late despawn, and counting it so
> the call site still gets fixed.
>
> **Next:** m12.4 — prediction + reconciliation, the milestone's hardest brick.

> **Update (2026-08-21) — m12.2 COMPLETE: `engine/gameplay`, the character controller as a pure
> function.** Collide-and-slide on a kinematic capsule over the `PhysicsWorld` seam:
> `step_character(state, input, config, world, dt) → state`, no writes to the world it observes.
> The ADR-0035 proofs hold — analytic slope/step/slide bounds and **replay determinism** (same
> tape twice ⇒ bit-identical; two independently built worlds ⇒ identical trajectories) — plus
> recovery convergence and a **1,296-cell structural probe grid** (18 slopes × 12 headings × 6
> step heights) asserting only what must hold everywhere: no NaN, no tick ends overlapping, no
> falling through the world, no `grounded` without a walkable surface under the axis, bounded
> speed, bounded give-up counters.
>
> The grid is the story worth carrying forward. The hand-written cases all passed while **264
> grid cells failed**, and the failures decomposed into five distinct controller defects — a
> tangential slide grazing millimetres into a convex lip each tick, a foot-down ray whose reach
> ended *exactly* at the answer it needed (float rounding decided a refusal), a step ladder
> committing a landing measured by a ray that saw the floor behind the riser, a ground snap
> burying the trailing flank in a ramp edge the axis had already crossed, and an edge-contact
> normal reading walkable at the crest of a 50° wall. One principle closed all five: **the tick
> certifies what it hands back** — depenetration recovery runs at start *and* end of the tick
> under one shared budget, every committed pose (step landing, snap) is confirmed against a
> penetration query before it is written, and `grounded` is returned only when a ray has vouched
> for it at the final pose. Each fix was found by probe-first replay of a printed failing cell,
> never by theorising from the summary line.
>
> **Deferred upward, measured:** GJK's overlap predicate loses shallow overlaps against very
> large convex shapes (10 m half-extent: exact to 1 mm; 20 m: misses 1.4 cm; ≥30 m: misses
> 10 cm — table in `tests/gameplay/character_fixture.hpp`). Test geometry is capped inside the
> trustworthy regime; the defect belongs to the collision core (#131/#132 family), not the
> controller. Named v1 costs live in `engine/gameplay/README.md`.
> **Next:** m12.3 — `gameplay_net`, the server-authoritative networked player.

> **Update (2026-08-21) — GJK stall verdicts are now certificates; the shallow-penetration misread
> is closed, and m12.2 is unblocked.** The deferred item below suspected `kTouchEps2`; the measured
> culprit was the **stall exits** — duplicate-support, no-progress, iteration-cap — which reported
> "separated" with whatever distance the stalled simplex held: an upper bound from an arbitrary
> simplex, which says nothing about *which side of contact* the shapes are on. Measured: "separated,
> 1.11e-3" at a true penetration of 1.85e-3 m, and a cast stopping ~1 cm past the surface.
>
> The verdict is now **earned**: when the origin is inside the Minkowski difference no support plane
> can exclude it, so a plane bound above a scale-relative noise floor (`kSupportEps · |w|`) is a
> *proof* of separation — and a stall without one reports overlapping. The subtlety worth the
> teaching comment in `src/gjk.hpp` is what it takes to *find* that certificate. A first design
> synthesized it from the terminal simplex's own perpendicular, and it is provably unfixable: the
> measured stalls terminate on a **segment — a chord of the curved Minkowski difference** — whose
> perpendicular is 0.13 rad off the true axis, costing 0.65 m of bound across a 5 m face (certificate
> −0.93 for a true gap of 8.7e-4; *no plane through that chord certifies, at any precision*), while
> defaulting the uncertified rest to overlap misreads genuine gaps up to ~300× the noise floor. The
> landed design is an **unconditional double-precision distance polish at every stall**: restart the
> walk from the stalled simplex with the simplex arithmetic in double (the transverse steering signal
> the float loop lost sits ~6 orders above double's blend error), round each search direction to
> float *before* the support call and take the bound along that exact float vector — so
> `(lower_bound, plane_dir)` is a true statement about the plane the caller receives. The polish runs
> even when the in-loop bound already certifies, because a true bound does not launder the stalled
> simplex's other outputs — measured, a certified-but-unpolished exit handed the retracted normal
> probe a normal transverse to a 20 m wall (n.x = 4e-4 where −1 was the answer).
>
> Fallout, both directions gated in `gjk_test.cpp` (penetration grid 1,536 poses, gap grid 384):
> false "separated" 56 → **0**; zero-bound certificates 155 → **384/384 certified within 20 % of the
> true gap** (worst 0.978×, over-claim ≤ 6.9e-8); stall-exit `distance` now worst **1.38 %** off
> where it was unrelated garbage (14.14 m for 2.8e-5). The 80° graze cast reaches 2.0 exactly (was
> 5.87 mm short). Cost: 11 support evaluations mean / 16 max for the *whole query* on the
> stall-heavy family, and the normal convergence path pays nothing. One consequence needed care: an
> exactly-flush cast start is honestly "not provably separated", so `shape_cast` now discriminates
> touch from penetration **by observation** — retract by the touch tolerance, re-measure; separated
> there is a hit at t = 0 with real witnesses, not `initial_overlap`. And the promise below is kept:
> the grid's over-report slack tightens 2e-2 → **5e-4** (measured: zero of 280 rows stop past
> `travel`, worst −1.05e-5) and the witness slack 5e-2 → **5e-3** (worst 1.795e-3).
> **Next:** m12.2 — the character controller (in flight). *[landed 2026-08-21; m12.3 landed 2026-08-26.]*

> **Update (2026-08-21) — shape-cast stepping fixed; the graze no longer stops short.** The item the
> entry below left open ("that is the next thing to fix") is closed. The old advance stepped by
> `GjkResult::lower_bound` **radially** — deliberately refusing the textbook `d / dot(dir, n)`, because
> m12.1 had measured a diagonal `n` turn that quotient into a 30× leap through a wall. A radial step
> closes an oblique sweep's gap by only *(1 − cos θ)* per iteration, so a graze burned the whole
> 64-iteration budget and reported a hit while still short of the surface; and the same bound
> **collapses to zero** on GJK's early exits against a large target, which drops the loop to its
> `kMinStep` floor and turns "short" into "barely moved".
>
> Three changes, in descending order of what they prove. **The projected plane bound**: `lower_bound`
> now travels with `plane_dir`, the direction of the plane that *produced* it, and the step is
> `lower_bound / -dot(dir, plane_dir)` — van den Bergen's ray-clip. The pairing is the whole point.
> That is what m12.1's rejected form was missing: it divided a distance by a direction the distance
> knew nothing about, where here bound and closing rate are two readings of **one plane**, so a noisy
> direction merely tilts that plane and *weakens* the bound — the quotient stays a proven
> under-estimate whatever the direction's quality. A plane the sweep never closes is a **proven miss**,
> returned immediately instead of walked to `tmax` in 64 steps. **The leashed-distance rescue**: when
> the proven step stalls below a sixteenth of the gap, advance by the measured distance — but only
> ever by `min(measured, trusted_prev + last_advance)`. True distance is 1-Lipschitz along the sweep,
> so that expression is an upper bound by induction, and the leash discards a measurement that is not
> loose but *wrong*: GJK's stall exits reported **14.14 m for a true gap of 2.8e-5 m**, and a step
> believing it clears the target and reads the far side as a clean miss. Clamping is asymmetric on
> purpose — a falsely large distance moves the caster, a falsely small one only slows the descent.
>
> **Measured on a 3,696-configuration sphere-vs-box probe against the exact analytic TOI** (x86-64,
> GCC -O2). Before: **1,340 under-reports, worst 3.35 m of travel**, every θ = 89° configuration
> failing, and — with the rescue present but *unleashed* — **2 outright tunnels** through a 0.2 m wall.
> After: **zero** under-reports beyond the loop's own resolution (worst residual 2.3e-4 radial, against
> `kTouchTolerance` 5e-5 plus GJK's distance slack), zero tunnels, zero phantom hits on 132 clean-miss
> configurations. The gate is `tests/physics/shape_cast_test.cpp` — three named regressions plus a
> 280-row grid, all through `PhysicsWorld::shape_cast`, because unlike #131 this defect's symptom
> **is** the number the seam returns; on the pre-fix code 72 of those 280 rows fail, the worst by
> 0.167 m. `plane_dir`'s support-plane invariant is re-verified from raw supports in `gjk_test.cpp`.
>
> **What this uncovered, and it lands before m12.2.** GJK still reports **separated** at shallow
> penetrations: measured, *separated with distance 4.9e-4 while the shapes were 2.9 mm into each
> other* (sphere r = 1 against a 5 × 5 × 0.1 slab, oblique). A cast that believes that number keeps
> stepping, and can stop up to **~1 cm radial PAST the surface** — 114 of the 3,696 probe
> configurations, worst 9.9e-3. The suspicion is specific: `kTouchEps2 = 1e-10` is an **absolute**
> epsilon — its own comment says so — guarding an error that scales with the shapes, the **third**
> instance of exactly the disease the entry below fixed twice in the same header. The over-report assertion in the new grid is held deliberately loose
> (`t ≤ L + 2e-2`, 40× the worst measured there) with a comment saying to tighten it when this lands.
> **Next:** the GJK shallow-penetration discrimination fix, then m12.2.

> **Update (2026-08-21) — GJK feature convergence fixed, ahead of m12.2.** m12.1 ended by *bounding*
> a narrowphase defect rather than repairing it; this repairs it, because the controller is built on
> the query it corrupted. **Two of GJK's epsilons were ABSOLUTE while the float error they guard is
> proportional to the size of the shapes** — the recurring shape of every bug in this pair of bricks:
>
> * the **convergence bound** compared against `kRelEps * dist2`, a purely relative budget, while the
>   error in the dot product it evaluates scales with the SUPPORT magnitude. Below roughly
>   *(float eps / kRelEps) × |w|* it could never fire, so GJK could not stop — it kept taking supports
>   along a direction whose transverse sign was pure noise, flipping between two opposite corners of
>   the same face, accreting **near-duplicate vertices**;
> * the **triangle degeneracy check** compared `|va+vb+vc|` against a fixed `1e-9`, while those three
>   are differences of products whose ULP grows as *|edge|² × |point|²*. For vertices a metre out one
>   ULP is ~1.9e-6 — a thousand times the epsilon — so a **collinear** triangle read as a real face and
>   the barycentric division turned noise into weights.
>
> Together GJK returned the **centroid of a sliver** whose vertices were collinear along the box's
> y=z diagonal: a 0.3 m sphere 1.9e-5 m from a 1 m box reported **distance 0.47 for a true gap of
> 1.9e-5**, normal perpendicular to the truth. That is where m12.1's diagonal contact normal came
> from, and why it looked like an edge direction — it was one. Both epsilons are now scale-relative,
> sized by measurement (the degenerate `va/vb/vc` came out as exactly 1–2 ULP of a ~16.1 product).
>
> **The gate reaches below the seam, and that is argued rather than sloppy.** Verified over 21,000
> configurations: `shape_cast` gives byte-identical results with the bug present and absent, because
> m12.1's lower-bound stepping and retracted normal probe mask it on x86-64 — so a seam-level
> regression test provably *cannot* fail. `tests/physics/gjk_test.cpp` is the one physics test that
> includes a `src/` header, and it does fail on the old code (0.4687 vs 0.00166). Everything else in
> the suite still drives `PhysicsWorld` only.
>
> **Two things found on the way, both still open.** `GjkResult::lower_bound` can collapse to **zero**
> when GJK exits through its no-progress guard: `closest` carries ~2e-6 of transverse noise, the
> target's ±20 m transverse support components multiply it, and the product swamps the real axial
> term. Since m12.1's shape cast *advances* by that bound, it then crawls at its minimum step to the
> iteration cap and under-reports — 3.2e-5 m for a true 8.4e-3 m gap, i.e. a caster that barely moves
> when it should step 8 mm. That is a **shape-cast stepping** problem rather than a GJK one now that
> `distance` is trustworthy again, and it is the next thing to fix. Separately, below a gap of about
> ten ULP of the vertex coordinates (5e-6 m for a 30 m box) the normal is accurate only to ~9°: the
> float32 resolution floor, not an algorithm defect. **Next:** the shape-cast stepping fix, then
> m12.2.

> **Update (2026-08-20) — m12.1 COMPLETE: the physics top-up the controller needs.** Two halves,
> neither of them new physics — both expose machinery the engine already had and could not reach.
> **`shape_cast`** sweeps a convex shape along a line by **conservative advancement** over the GJK
> distance the speculative-CCD path already uses: if the shapes are `d` apart and the sweep closes
> that gap at rate `dot(dir, n)`, advancing by `d / dot(dir, n)` provably cannot pass through. It
> exists because a ray is infinitely thin and threads gaps a body could never fit down — the reason
> a controller built on raycasts walks through door frames. `ShapeHit::initial_overlap` is the field
> that makes it more than a fat raycast: "touched after moving 0 m" and "started inside a wall" are
> the same number and opposite instructions, and confusing them freezes a controller inside the
> geometry it is stuck in. **Kinematic push-in** fills the seam `sync.hpp` has named since M7.6: a
> game that moves a kinematic body's `WorldTransform` drives the physics body, *with the velocity
> that move implies*, so a capsule walking into a crate PUSHES it at walking speed instead of
> teleporting into it and having the solver undo a penetration by firing it off.
>
> Two things worth carrying forward. The "unmoved, so skip" path needed an **exit** — without one
> final zeroing push, a body the game stops moving keeps its last velocity and the crate slides away
> by itself forever. And the contact normal is measured at a **retracted** position, one extra GJK
> where the shapes are provably apart: taking it at the touch is ill-conditioned (on a flat face the
> witness slides freely without changing the distance), which showed up as a normal 90° off on
> x86-64, and then again on **arm64 only** after the first fix. Threshold-tuning was the wrong
> answer; measuring where the answer is well-conditioned was the right one.
>
> **The GJK defect this brick uncovered is FIXED — see the entry above.** It was deferred here on
> the day and taken next, because m12.2's collide-and-slide leans on exactly the normal it corrupted. **Next:** m12.2 — `engine/gameplay`, the character controller as a pure function
> over these queries.

> **Update (2026-08-20) — m12.0-perf COMPLETE: performance is now measured, in both halves.** The
> brick landed in two passes. The first (#128) built the **work ledger** — counts, never clocks, so
> lavapipe gates them in CI forever — and wired it into `10-destructible-wall` and `11-lit-rooms`,
> turning M10's caching claims into a gate: a static frame re-stamps nothing and re-renders no
> shadow slot, while the break recomposes exactly three SDF regions, invalidates the one shadow slot
> it touched, and fast-tracks 60 probes. The budgets come in **static/break pairs** on purpose — a
> clipmap that had silently stopped working would also re-stamp nothing, so each zero is paired with
> a floor on its break-frame twin.
>
> The second pass built the **hardware report** (`core/diagnostics/perf_report.hpp`,
> `--perf` on both samples, `scripts/perf.sh`): a fingerprinted JSON of frame/sim **distributions —
> p50/p95/p99/max, and no mean anywhere**, because ten 40 ms frames in a 600-frame run move a mean
> by half a millisecond and vanish. Reports are committed **append-only to
> [`docs/perf/`](perf/)**, and a run is compared only against one whose fingerprint matches — GPU,
> **driver**, OS, build config, **sanitizer**, preset, resolution. Two of those exist because of
> past scars: an ASan binary would otherwise read as a catastrophic regression (#125's lesson), and
> a Vulkan API version does not change across an NVIDIA driver update, so a driver's performance
> change would have been billed to the engine. Three engine seams fell out of it: `on_post_submit`
> (the only window in which per-pass GPU timestamps are readable), the engine's **first profile
> zones** — `RIME_PROFILE_ZONE` had existed since M1.6 with *zero* callers, corrected in the ADR —
> and `AdapterInfo::driver_name/info`.
>
> **The first RTX 3060 baseline is committed, and it does not ratify what ADR-0035 hoped it would.**
> At 1920×1080 Release: the full M10 lighting stack runs at **p99 7.96 ms / max 8.05 ms**, and a
> 60-part collapse at **p99 3.79 ms** with a **0.53 ms** worst sim tick — 2.1× and 15× inside the
> proposed budget. By the letter of §2 that means *tighten*; the [amendment](adr/0035-vision-demo-m12.md)
> rules otherwise, because the measurement is of the wrong subject: these samples are two orders of
> magnitude smaller than the block §1 requires, so tightening against them would manufacture the
> appearance of ratification. **The headline budget is re-timed to m12.7**, measured against the
> block's own content; what m12.0 ratifies is the **relative** gate, which had nothing to compare
> against until a baseline existed and could therefore not fail. It fails now at 8.76 ms, not 16.6.
> The gate was proven to fail five ways on the 3060 — absolute breach, regression, missing timeline,
> too-few-samples, and the vacuity guard catching a run whose wall never broke. **Next:** m12.1 —
> physics top-up (`shape_cast` + kinematic push-in).

> **Update (2026-08-20) — Milestone 12 ("The Block", the vision demo) kicks off.** **m12.0 lands
> [ADR-0035](adr/0035-vision-demo-m12.md) (Accepted)**, and the milestone enters with a problem no
> previous one had: its "done when" is written as an *adjective*. M12 was the last milestone on the
> map and the only one that must answer **"feels right"** — so the ADR's first job is turning that
> into clauses that can fail: own-input response ≤ 1 tick **against a prediction-off control**,
> same-frame fire feedback, bounded remote-motion continuity, bit-exact peer agreement at ~10×
> m11.7's scale, and scale *floors* (≥ 8 structures, ≥ 1,500 parts, ≥ 400 peak debris, ≥ 32 local
> lights) **asserted by the proof**, because too-small is the quiet failure — below them the byte
> budget never binds, relevancy never differs between clients, and half of M11 is dead code in the
> run. The one clause that cannot be a test is named as such: a recorded human play session is
> evidence for a judgement, not a passing assertion.
>
> **The load-bearing decision is how to make a performance claim durable.** This is the first
> milestone that *can* make an absolute one — the primary workstation now has an RTX 3060, and M10
> deferred every frame budget to exactly this point — while CI stays lavapipe forever. The ruling
> **splits "how much work" from "how fast the work runs"**: the *work ledger* is a set of
> machine-independent, exact, deterministic counters (most already built — `resolve_timings`,
> `LocalShadowStats`, the SDF stamp counts, `WorldStats`, the m11.3–m11.6 replication suite;
> **missing and added at m12.7: draws submitted vs. culled**, since no frustum cull exists anywhere
> today) that **lavapipe gates in CI forever**, so a change that doubles draw calls or wakes a
> sleeping pile fails deterministically with no hardware in the loop. The absolute claim is a
> **self-gating `--perf` run** reporting a **distribution — p99 and max, never the mean**, because
> destruction is bursty and the fracture tick is exactly the frame that must not hitch — with the
> run's own ledger attached, so a fast run on a scene that did no work is self-evidently invalid.
> Reports are fingerprinted JSON committed **append-only to `docs/perf/`**: the ADR culture applied
> to measurements, so "measure before optimizing" finally gets a ledger instead of a habit. Numbers
> are *not* fixed here — m12.0's baseline session ratifies them, and if the block sails under
> trivially the budget tightens, because **a gate nothing can fail is not a gate**. *(Amended at
> m12.0-perf: the baseline exists and the headline numbers are **re-timed to m12.7** rather than
> tightened — see ADR-0035 A1. Tightening a budget for the block against samples two orders of
> magnitude smaller would have looked like ratification and meant nothing.)*
>
> **The player who was never built.** A11/A20 deferred prediction because no controller existed to
> shape the seam; a ground-truth pass for the ADR also found [ADR-0026](adr/0026-physics-core.md)
> still promising *"a capsule mover ships at m11.3"* — false, since that brick became the replication
> core (now corrected by amendment). So: `step_character` is a **pure function over physics queries**
> (capsule collide-and-slide on a new `shape_cast`), because purity is what makes it replayable and
> replay is what prediction is built from; it lands in a new **`engine/gameplay`** that does *not*
> link `destruction` (the M8.4 fan-out discipline), with **`engine/gameplay_net`** above it on the
> `destruction_net` argument. Reconciliation compares **resulting state, never a diff of command
> lists** — m11.6c's rule, and not a stylistic one: `consumed_through` steps over permanent gaps, so
> a command leaving the un-acked list means *"the server will never act on it"*, and a list-diffing
> predictor is wrong exactly under loss, the condition it exists for. The state↔input pairing crosses
> as a replicated **`LastProcessedInput`** component riding the ordinary snapshot path — A15's
> "data, not a message" move reused, so there is no new framing and no new completeness rule to get
> wrong. **Scope rulings:** Track FL (water) is **out** — this is the "decided at M12.0" the roadmap
> promised; Track FX is **in at its true size** (fx1a's draw pass load-bearing, fx1b contingent on
> the ledger, fire-as-light deferred); audio is **in as cuttable polish**, first on the cut list.
> **Never cut:** prediction, fx1a, the perf gate. All 38 deferred items across the ADRs, module
> READMEs and named-gap trail are ruled in §6, because an unruled item silently becomes scope — and
> the 38th is the lesson: a **second, independent** review pass found **ADR-0032's C6** (bounded
> debris *visual* retirement, declared "M10's to build" and never built) missing from a section whose
> first draft claimed completeness. m8.5 bounds debris in the *physics* world, so no M10 proof ran
> long enough to notice the *visual* population — render leaves, SDF stamps, shadow casters — growing
> without limit. It is ruled in two halves: **m12.0-perf counts it, m12.7 fixes it**, which makes C6
> the work ledger's first real customer.
> **Ladder:** m12.0 (this ADR + the hardware baseline) · m12.0-perf (harness + ledger) · m12.1
> (shape casts + kinematic push-in) · m12.2 (the controller) · m12.3 (networked player, server-auth)
> · m12.4 (**prediction + reconciliation — the hardest brick**) · m12.5 (interpolation v2) · m12.6
> (fx1) · m12.7 (block content + frustum culling) · m12.8 (windowed present + FPS camera) · m12.9
> (audio) · m12.p (the measured perf pass, scope chosen by the ledger) · m12.10 the proof,
> `samples/99-the-block`. **Next:** m12.2 — the character controller. *(m12.0-perf and m12.1 are
> done; see the completion entries above. The headline budget's ratification moved to m12.7, where
> there is content of the right size to measure.)*

> **Update (2026-08-20) — m11.7 (the milestone proof) + Milestone 11 COMPLETE.** M11's "done when"
> now runs: [`samples/12-networked-destruction`](../samples/12-networked-destruction) — a dedicated
> headless server owns the canonical destruction simulation while **two clients at opposite ends of
> the wall row receive demonstrably different bytes (640 vs 558) and still agree bit for bit** on
> which parts died, what health they hold, and what debris exists. That claim is only available
> because the hybrid model earns it: composition is **derived** from a replayed op stream, never
> sent, so relevancy may legitimately give the two clients different mail without giving them
> different worlds.
>
> **The brick opened on a contradiction in its own spec** — "two clients **over loopback**" and, in
> the same sentence, "hash-verified **in CI**, deterministic, scripted loss, never environment luck."
> Real UDP brings a per-runner scheduler; a proof that never touches a socket is a 51st unit test.
> `net::Link` had already answered this one layer down, so the peers are written **once** against
> `Link` and the transport is a flag. **Both ship, because what they can claim differs**: the
> scripted run gates CI on an exact packet economy, the UDP run can make no exact claim but is the
> only thing that can see a socket bug. Both produce the same hash, `d539e2f3e8799592`.
>
> **The design error worth recording.** The first version installed a plain `distance_relevancy` and
> asserted the peers agree. They did not — and **the engine was right**: `shared_state_hash` folds
> each peer's *own* `NetIdMap`, so a client relevancy never told about wall 5 hashes five walls
> against the server's six. The mismatch was correct behaviour and the proof was measuring relevancy
> rather than destruction. The roadmap already carried the rule ("destruction events are never
> culled, debris transforms are distance-budgeted per client"), so destructibles are forced
> always-relevant and debris stays distance-scored. What comparison to make is a design decision,
> not a test detail. Comparison is peer-to-peer, **never against a checked-in constant** — the op
> list is fed by contact impulses whose float results may legitimately differ between compilers, and
> a golden hash would quietly convert this proof into the cross-platform float-determinism claim this
> project has already recorded as false — and it is sampled at **quiescence barriers**, where
> quiescence is each client's *own* hash going still, never its agreement with the server, which
> would beg the question the barrier exists to ask.
>
> **Four ways it could have passed while broken, each now an assertion; the first draft hit three.**
> Shots were single taps, so nothing died and **228 dead parts were 0** — every downstream claim
> vacuous. `sync_debris` was never called, so no chunk became a replicated entity and the entire
> m11.5 half measured an empty world (culled 0, over-budget 0; now **69603** and **24479**). And the
> negative control decremented its counter and then applied the batch anyway, so "the sabotaged
> client disagrees" was passing against a client that had lost nothing. Also asserted: loss actually
> happened (299 packets), nothing was silently discarded as malformed, and the m11.6c input path
> carried commands end to end. One trap the segfault taught: `ScriptedLink` holds a back-pointer to
> the `ScriptedNetwork` that vended it, so returning the transport **by value** moves the network out
> from under every link already handed out — it compiles, reads as ordinary RAII, and crashes in
> endpoint comparison with a backtrace pointing nowhere near the move.
>
> **The milestone, whole.** **m11.0** ADR-0033 + the ladder · **m11.1** transport v2 (`UdpSocket`,
> the `Link` seam, frontier-anchored acks) · **m11.2** sessions + the `Application` ordered sim stage
> · **m11.3** replication core (`NetId`, reflection snapshots, ack-baseline deltas with **no history
> buffer**) · **m11.4a/b** networked destruction — events, then debris · **m11.5** relevancy +
> budgets · **m11.6a/b/c** interpolation, the draw path, and input as intent · **m11.7** the proof ·
> **m11.8** this docs true-up. ADR-0033 accumulated **twenty-three amendments (A1–A23)**, nearly all of
> them found by *building* rather than by reading — including two false claims in the ADR's own text
> (A9/A10). The one that generalizes beyond networking is the
> [replication invariant](design/replication.md): **any per-peer "what they have" fact may only
> strengthen on confirmed *holding*** — never on "we sent it", "it arrived", or a proxy with a blind
> spot. That bug appeared **five separate times** across m11.3–m11.5 in five different disguises, and
> every instance was caught by a **counter**, never by reading the code. A proof that cannot see what
> it skipped reads as passing.
>
> **Honest gaps, named not hidden.** Deferred fast-follows: **late-join baseline snapshots** (and so
> composition mismatch is *detected but not repaired*); **transform quantization**; **lag
> compensation**; **player-controller prediction** — the seam ships as state rather than an interface
> (A20) precisely because no controller exists to shape it, and nothing consumes the replicated input
> yet; debris **velocity** is not replicated; snapshot interpolation still spans exactly one tick
> rather than the interval a value covers; the relevancy pass still **walks every replicated entity
> per client** (narrowing it needs a spatial index); and the dev-server scale run has not been done.
> **Next:** re-plan at the milestone boundary — M12, "The Block", is the vision demo, and it is the
> first milestone that can make **absolute** performance claims now that a real GPU (an RTX 3060) is
> the primary workstation. CI stays lavapipe, so how a hardware-only measurement becomes a durable,
> regression-catching gate is an open design question, not a detail.

> **Update (2026-07-28) — Milestone 11 (Networking + networked destruction) kicks off.**
> **M11.0 landed [ADR-0033](adr/0033-networking-v1.md) (Accepted)** — the networking-v1 decisions:
> **dedicated-server authority** (a listen server is a degenerate embedding, not a design; lockstep
> stays rejected per ADR-0026's cross-platform non-goal); the **hybrid replication model** —
> destruction *topology* replicates as **reliable-ordered events** that each client replays through
> M8's deterministic damage → detach function (the ADR-0029 event-replay contract, at last), while
> dynamic *state* (debris, bodies, players) rides **unreliable-sequenced snapshots** off the server
> as the drift-correcting authority; **our own UDP transport** — a `platform::UdpSocket` plus a thin,
> teachable reliability layer (sequence/ack/resend; reliable-ordered + unreliable-sequenced channels)
> over a scripted-loss deterministic `Link` seam, so every networking proof stays GPU-free and
> reproducible in CI while the S0/S1 TCP/UDS tools wire is untouched; **server-assigned `NetId`s**
> with **reflection-generated, schema-hash-checked snapshot serialization**; and **relevancy v1** —
> destruction events are never culled, debris transforms are distance-budgeted per client. M11 is
> decomposed into bricks **m11.0–m11.7** (see the M11 detail below): transport → sessions →
> replication core → networked destruction → relevancy/budgets → interpolation/input → the proof,
> `samples/12-networked-destruction` (a dedicated headless server + two clients seeing the same wall
> break at scale, hash-verified in CI). **Next:** m11.1 — transport v2.
>
> **Update (2026-07-28, evening) — m11.1 (transport v2) + the ADR-0033 amendment.** The transport
> foundation landed: `platform::UdpSocket` (POSIX-shared + Win32, non-blocking, IPv4 `Endpoint`)
> and **`engine/net` born** — the `Link` datagram seam (`UdpLink` + the deterministic
> `ScriptedNetwork`: scripted loss/latency/reorder/duplicate on a virtual clock, xorshift-seeded so
> traces are bit-reproducible) and the `ReliableChannel` reliability layer. A post-implementation
> adversarial review caught two critical ack bugs the first design shared with the classic
> "newest + backward-bitfield" scheme (the seq-0 false-ack deadlock; the unreportable late
> recovery), so the shipped design **anchors the ack at the delivery frontier with a
> forward-looking bitfield** — proven by regression tests — plus a sender-side in-flight window,
> a driver-routed `process_packet` (one socket, N peers), `ByteWriter`/`ByteReader` wire parsing,
> and 9 GPU-free proofs incl. 30%-loss exact-order delivery and a real-socket UdpLink loopback.
> The same review produced the **ADR-0033 amendment (A1–A6)**: the replicated destruction unit is
> the committed damage-OP list (contact-derived damage included), `compute_type_hash` needs
> name-folding (a live collision exists in-tree), destruction grows a state-application seam,
> m11.6 BUILDS interpolation (ADR-0023's buffer is an unbuilt seam), `Application` grows the
> ordered sim stage at m11.2, and m11.7's shooter is a deterministic server-side script. The
> ladder is updated to match. **Next:** m11.2 — sessions + the ordered sim stage.
>
> **Update (2026-07-29) — m11.2 (sessions) + the `Application` ordered sim stage.** The session
> layer landed: **`NetDriver`** (owns the endpoint→session routing table, polls the shared `Link`
> once per tick, runs the handshake, reaps dead peers — role-agnostic, so `listen()` makes it a
> server and `connect()` a client) and **`Session`** (one peer relationship: its channel, its
> Connecting→Connected→Closing state, its liveness timers, its inbox). The **handshake is
> connectionless** — a `ReliableChannel` is a conversation with an *established* peer, and
> allocating one on first sight of an endpoint is precisely the DoS the handshake must prevent — and
> is **validated before it can allocate**: protocol version, app id, and schema hash travel as
> separate fields, compared separately, so a rejection names exactly what to fix. The schema number
> comes from the new **`ecs::component_schema_hash`** (a *sorted* fold of registered components'
> `type_hash`es — registration order is not a contract), which `engine/net` only ever sees as an
> opaque `u64`, so the module keeps its core+platform-only dependency. Design review produced
> **ADR-0033 amendment A7**: every session datagram now carries a **4-byte incarnation salt**,
> because a peer reconnecting from the same address gets a channel whose sequence spaces restart at
> 0, so its *old* incarnation's in-flight packets would be buffered into the new stream as
> legitimate early traffic. The same salt is how a reincarnated client is told from a duplicate
> request. **`Application` grew the ordered sim stage** (A5/A8): `PreSim → [Schedule] →
> [propagate_transforms] → PostSim → Publish`, with `on_fixed_tick` preserved exactly as sugar for
> its one `PostSim` entry. 17 GPU-free proofs incl. schema rejection *with no server-side
> allocation*, reincarnation, a bounded session table, garbage that poisons nothing, **peer death by
> timeout**, and a real-socket loopback case. **Next:** m11.3 — replication core.
>
> **Update (2026-07-30) — m11.3 (replication core) + ADR-0033 A9/A10.** A ground-truth pass first:
> §4's claim that *"the ack bitfield doubles as the delta-baseline tracker"* is false against the
> shipped code (that machinery serves only the reliable stream; the unreliable-sequenced channel
> acknowledges nothing in either direction), and §7's claim that `engine/net` depends on `ecs` was
> never true — it links `rime::platform` alone, which m11.2 paid real cost to preserve. Both are now
> **amendments A9/A10**, and the second is why the brick landed as a **new `engine/replication`
> module above `net` and `ecs`** rather than inside `engine/net` (the shape `editorhost` already has
> with `ecs`+`stream`). The core: **`NetId`** (a generational `core::Handle`, recycled — debris churn
> makes a never-reused counter unbounded in practice), reflection-derived **snapshot writers/readers**
> whose component ids are each type's **rank in the `type_hash`-sorted set** (both peers derive it,
> nothing is transmitted — `ComponentId` cannot go on the wire, since m11.2 made registration order a
> non-contract), and **ack-baseline delta replication** that needs **no history buffer at all**:
> `ecs::Chunk`'s per-column version stamps (ADR-0018 §4) already answer "changed since V", so
> per-client state is *one integer*. Three traps found and closed in the design: the baseline ack has
> nowhere to live but a replication message on the client's own unreliable channel (A9); a client
> that acked a tick on the strength of one of its **parts** would make the server skip entities
> written at that tick, diverging **permanently and silently**, so `AckTracker` advances only past
> complete ticks; and because spawn/despawn (reliable) and deltas (unreliable) have **no ordering
> relative to each other**, a recycled `NetId` needs `NetIdMap::resolve`'s generation check or a new
> entity's state smears onto the old one's mirror. Also added `core::reflect::packed_size` — a framed
> reader cannot use `sizeof`, which counts padding the wire format does not have. Proofs are
> GPU-free on the scripted-loss harness: convergence to a **bit-identical NetId-ordered state hash**
> under 20% loss with reordering (with a negative control, so the hash is proven to discriminate),
> multi-packet ticks exercised, the cross-channel race observed firing, and `Parent` proven excluded
> from the wire schema because it carries an `Entity`. Named gaps: no prioritization (m11.5), no
> interpolation (m11.6), entity-reference fields do not replicate, and change detection is
> chunk-grain so it over-includes. **Next:** m11.4 — networked destruction.
>
> **Update (2026-07-30) — m11.4a (networked destruction: the events half) + ADR-0033 A11/A12/A13.**
> The brick grew a prerequisite on contact with the code: naming a destructible on the wire needs the
> **ECS bind path** M8.2 deferred (`DestructibleInstanceRef` was declared and written by nothing), so
> that landed first. It is what makes the addressing honest — an op names the destructible ENTITY's
> **NetId**, never an `InstanceId`, because that is a local table position two peers agree on only if
> they happened to spawn in the same order, which late-join breaks on its first tick. `DamageOp` is
> now public and a wire contract, ops are replicated **expanded per-part with the falloff already
> resolved** (so that float math runs on exactly one machine), and mirrors are `Authority::Remote` —
> one early return in the contact drain, deliberately not a forked update path. The code lives in a
> **new `engine/destruction_net` module above both `destruction` and `replication`**, the same
> guardrail-2 argument A10 used to create `engine/replication`. **Three findings, all from making it
> run rather than from reading it:** (A11) "clients apply ops at the same tick" cannot mean a local
> tick index — reliable delivery is ordered but never timely, so the tag is an ordering/identity key
> and clients apply on arrival; (A12) two of the server's ticks arriving in one of the client's must
> NOT be merged into one `update()` — alive bits and healths still converge, but **debris composition
> diverges** (measured: two islands became one), and m11.4b addresses debris by roster index, so that
> is a wrong address rather than a cosmetic difference; (A13) a **live m11.3 bug** the first
> end-to-end proof walked into — a delta packet whose records were all *discarded* as unresolvable was
> still acknowledged, so the server advanced the baseline past writes the client never applied, and an
> entity that then stops changing stays wrong **forever**. That is precisely the case m11.4 replicates
> (a wall standing quietly until shot); m11.3's own proof missed it because every entity there moved
> every tick. Also: the payload tag space is **shared** per session and nothing said so — both modules'
> enums would have started at 1, and `drain_received` moves messages out, so one subsystem would have
> silently never received its mail. A registry now lives in `replication/snapshot.hpp`. Proofs are
> GPU-free on the scripted-loss harness: convergence of a **NetId-ordered per-part alive/health +
> debris-composition hash** under 20% loss with reordering (with a negative control), multi-packet
> ticks exercised on both ends, and the A3 seam proven to reach the same tables — including the debris
> roster — as a peer that watched the whole collapse. Named gaps: debris transforms do not replicate
> yet (m11.4b), and `DestructionWorld::state_hash()` is a same-process replay witness only — it folds
> physics body ids, which two independently-built worlds never agree on. **Next:** m11.4b — debris.
>
> **Update (2026-07-30) — m11.4b (debris) + ADR-0033 A14/A15.** The split that makes debris cheap:
> determinism already gives both peers the **same chunks in the same order with the same initial
> conditions** (m11.4a proves the rosters agree index for index), so **composition is derived and
> never sent**; what it does not give is the trajectory afterwards — the peers are not in lockstep,
> their physics worlds hold different body populations, and same-binary determinism is not
> cross-platform — so **transforms are replicated**. That is exactly the split "debris transforms are
> distance-budgeted per client" always assumed: you can budget a correction, never an event. The
> association crosses as **data rather than as a message** — a reflected `DebrisOrigin{source NetId,
> ordinal}` rides m11.3's snapshot path, so there is no new tag, no new framing and no new
> completeness rule. Corrections apply on a **tolerance**, not every tick: the replicated transform is
> authority for where a chunk ends up, not a per-tick puppet string, and snapping a
> continuously-simulated body every frame would replace tumbling rubble with a stutter (smoothing is
> m11.6's job, not an ad-hoc lerp here). The ordinal is only a safe address because A12 holds, so a
> `CompositionCheck` message verifies it rather than trusting it — ordinal addressing fails *silently*,
> resolving to a **different** chunk, after which the client corrects the wrong rubble. **A14 replaces
> A13's rule with a better one:** rather than withholding an acknowledgement when a delta record's
> `Spawn` has not landed (which made acking hostage to spawn traffic and stalled the baseline for the
> whole burst — the cost m11.4a recorded honestly), the client now **holds the bytes and replays them**
> when the `Spawn` binds the id. Nothing is lost, so the tick is honestly complete; the buffer is
> bounded because the ids keying it come from the peer, and overflow falls back to exactly A13's
> behaviour. Also: the **cross-peer witness moved out of the test and into the engine** as
> `destruction_net::shared_state_hash` — m11.7 hash-verifies it in CI, a dedicated server compares it
> to spot a diverged client, and a sample prints it, and three callers re-deriving it privately would
> be three subtly different answers to one question. Proofs stay GPU-free on the scripted-loss
> harness: debris mirrors bind to the chunks the client derived, compositions match with zero
> mismatches, and under a **deliberately wrong client gravity** the corrections fire and the gap stays
> **bounded** as the run triples in length — a property no magic tolerance constant can express and no
> runaway can accidentally satisfy. Named gaps: debris **velocity** is not replicated (the local
> solver's is kept, which is a good estimate precisely because both peers started the chunk from the
> same impulse), and composition mismatch is **detected but not repaired** — repair needs a
> client→server path or a periodic state broadcast, which is late-join machinery. **Next:** m11.5 —
> relevancy + budgets.
>
> **Update (2026-08-12) — m11.6c (the upstream half) + ADR-0033 A19/A20.** The one stream in M11 that
> runs client→server, and the direction changes what the bytes mean: a snapshot is a fact, an input
> is a *request*, so this path is built so that disbelieving one is the default. **What crosses the
> wire is intent, not `platform::Event`** (A19) — raw device events are unbounded per tick, are
> device-shaped rather than game-shaped (a keybind is client policy the server has no business
> knowing), and would put the server's arithmetic on the client's side, which is §1's authority model
> inverted. So an `InputCommand` carries movement axes on the unit disc, **absolute** view angles, and
> two bitfields; `InputSampler` does the conversion the whole brick turns on — **the local path is
> edge-shaped and the wire wants level-shaped**, so somebody has to integrate down/up edges into held
> state and relative motion into an angle, and a sampler that read only the current frame's events
> would send `held = 0` on every frame but the first. Channel choice is §3's `Delta` argument pointed
> the other way: **unreliable-sequenced**, because a retransmitted input arrives a round trip stale
> *and* holds every fresher one behind it — loss is answered with **redundancy rather than
> retransmission**, each packet carrying the last few commands against a receive frontier that
> deduplicates. Both new message kinds claim **their own unreliable stream** rather than defaulting
> into stream 0 — this is the message pair whose design turned up A18's shared-sequence-space bug in
> the first place, so sharing here would have been that bug in its original form, and the stream
> argument defaulting to 0 makes the omission silent. `held` and `pressed` are separate fields
> precisely because their loss stories differ:
> a level is self-healing, an edge exists in exactly one command and is what the window protects. A11
> carries upstream unchanged — the sequence is an ordering key, never a schedule.
>
> The server keeps **two frontiers, not one**, and this is the first mechanism in the module written
> to [the replication invariant](design/replication.md) rather than retrofitted after a bug:
> `received_through` advances on arrival (deduplication needs it to), `consumed_through` advances only
> when the **game drains**, and only the second is acknowledged. One deliberate exception is written
> down rather than left to look like an oversight: `consumed_through` is **not a completeness claim**
> — it steps over permanent gaps, because nothing re-offers a lost input — and the consequence is that
> a command leaving the client's un-acked list means *"the server will never act on it"*, not *"the
> server acted on it"*, so reconciliation must compare resulting **state**, never a diff of command
> lists. **The prediction seam ships as state, not as an interface** (A20): with no character
> controller anywhere in `engine/`, a `virtual void replay(...) = 0` would be guessing at its own
> signature, and a wrong guess in a header is inherited as a constraint. What is not a guess — the
> un-acked command list and the `InputAck` echo that retires it — is built, bounded, counted and
> proven. Proofs are GPU-free on the scripted-loss harness (16 cases), and the load-bearing one is a
> **negative control**: the same scenario, same seed, same scripted drops, run at redundancy 1 and 3 —
> **146 commands lost versus 23**, so the window is demonstrably *the reason* input survives rather
> than a threshold some other design might meet by luck. Also proven: the ack does not move while a
> server buffers without draining, a stale ack cannot resurrect retired commands, a flooding client
> cannot grow server memory, a hostile count field is not a loop bound, and input and state
> replication share one session without eating each other's mail. Named gaps: nothing consumes the
> commands yet (there is no controller to consume them — M12), inputs are not associated with a
> controlled entity, and there is no clock offset, so the tag cannot yet mean a server tick.
> **Next:** m11.7 — the milestone proof, `samples/12-networked-destruction`.
>
> **Update (2026-07-30) — m11.6b (drawing the blend).** m11.6a computed an interpolated pose that
> nothing could read; this brick connects it to the renderer and finds two defects doing it. The
> connection is **`ecs::RenderTransform`** — an unreflected "pose to draw this frame" component —
> because `rime::render` links rhi/ecs/assets and `rime::replication` links ecs/net, so the two can
> only meet on the module below both. `replication::update_render_transforms(world, alpha)` deposits
> it once per frame from the render callback and `extract_scene` prefers it over `WorldTransform`,
> falling back where absent. That split is also the determinism argument: `WorldTransform` stays the
> simulated truth, and since `Application` hands `alpha` out through `FrameContext` alone, a sim
> stage could not call the pass correctly even by mistake. **First defect: a replicated entity was
> undrawable** — a mirror is spawned bare, `WorldTransform` is unreflected so it never crosses the
> wire, and `propagate_transforms` only touches entities that already have both transforms, so no
> mirror ever had a world pose and every renderer query skipped it in silence. It now gets one on
> its first transform write, seeded from that write because `bind_destructibles` prefers
> `WorldTransform` and a default would stand a destructible at the origin. **Second defect, the
> sharper one: `valid` was never turned off.** `alpha` sweeps 0→1 every tick regardless of whether
> an entity received anything, so a pair left valid after the motion stopped replays its last step
> forever — the mirror snaps back and slides forward once per tick for as long as it stands still,
> and debris coming to rest is the most common event in a destruction engine.
> `settle_transform_history()` expires history a tick did not renew, keyed to a **genuinely
> different value** rather than to "a record arrived" — the server re-sends an unacked value for a
> round trip, and treating those as motion would hold the blend open for the whole window. m11.6a
> could not see either: its proof drove the entity for 20 straight ticks, so "moves, then rests" —
> one of the two behaviours the history exists to distinguish — was never run. Named gap: the blend
> always spans exactly one tick, so an entity whose record relevancy defers replays at speed and
> then holds (judder, not rewind); interpolating over the interval a value actually covers is the
> **snapshot interpolation** layer, and the delta header already carries the server tick for it.
> **Next:** m11.6c — the client→server input half.
>
> **Update (2026-07-30) — m11.5 (relevancy + budgets).** Per-client interest, nearest-first priority
> ordering and a per-tick byte budget, plus the ready-made `distance_relevancy` policy in
> `replication/relevancy.hpp`. Two decisions in that policy are worth more than the arithmetic:
> position falls back from `WorldTransform` to `LocalTransform`, because debris — the population the
> whole brick exists to cull — are spawned as roots carrying only the latter, so a stricter policy
> would fail to locate exactly its own subject; and an entity with **no position at all is always
> relevant**, because a distance filter that culled what it could not measure would silently stop
> replicating every non-spatial entity in the game. Failing open costs bandwidth, failing closed
> costs correctness, and only one of those is visible. The **hysteresis band** is load-bearing rather
> than cosmetic: without it a boundary-straddling chunk re-enters every other tick, and every entry
> both forces a full-state re-send and surrenders the per-chunk delta skip for that whole tick.
>
> The roadmap's "destruction events are never culled, debris transforms are distance-budgeted per
> client" is now a **test rather than a comment**: with a policy scoring everything irrelevant and a
> one-byte budget, the cross-peer destruction witness still matches bit for bit, because alive bits,
> health and composition are all *derived* by the client from the op stream. **Three more instances
> of the [replication invariant](design/replication.md) surfaced while building it** — a despawned
> slot that read as eternally arriving and pinned the full-walk optimization off forever; per-item
> relevancy bookkeeping strengthened when a record was *built* rather than *sent*; and the byte
> budget declaring incomplete ticks complete, which retired withheld state from the candidate set
> permanently. All three were found by counters, not by reading. Named gaps: the relevancy call still
> **walks every replicated entity per client** (narrowing it needs a spatial index — its own brick),
> and debris **velocity** is still not replicated. **Next:** m11.6 — interpolation + input.
>
> **Update (2026-07-22) — Milestone 10 (Advanced lighting) COMPLETE.** The whole [ADR-0032](adr/0032-lighting-v2.md)
> stack landed on `main`, brick by brick, every technique gated behind `LightingSettings` so **off is
> the byte-identical M5.6 baseline**: **m10.1a** RHI array/cube textures + depth-compare sampler ·
> **m10.1** directional **CSM** · **m10.2** cached **local spot shadows** (destructibility-aware) ·
> **m10.3** **clustered-forward** many-lights (+`RGBuffer`) · **m10.4a/b** cooked-SDF **clipmap** ·
> **m10.5a/b** **DDGI** probes (trace-and-store, then consume with Chebyshev visibility + a
> destruction-reactive hysteresis) · **m10.6** retire the ambient hack when GI is on · **m10.7a/b/c**
> **SSR** (thin G-buffer → linear-march resolve → DDGI-probe fallback + roughness cone) · **m10.8**
> the milestone proof + this docs true-up. **The thesis is proven executable** — *break a wall and the
> shadow moves **and** the bounced light updates* — in `tests/render/gi_thesis_test.cpp` (the isolated
> GI-in-HDR rise, the leak guard, the reactivity), and the **whole stack runs together in the new
> [`samples/11-lit-rooms`](../samples/11-lit-rooms)** (M10's "done when": all six gates on, break the
> divider, the shadowed floor floods with light — self-checked in CI on lavapipe). **Honest gaps,
> named not hidden:** grey-world GI albedo (no colour bleed) + a true pre-filtered specular probe +
> point-light cube shadows + full virtual shadow maps are follow-ups; **m10.i** (virtualized geometry,
> Nanite-class) never started and floats; the **hi-Z SSR march** waits for m12.0 (pure perf,
> unmeasurable on lavapipe's CPU-rendered depth). Absolute frame budgets wait for real hardware at
> **m12.0** — CI stays lavapipe, so the proofs are **structural**, never golden images. **Next:** M11
> (networking/replay) or the FX·FL·AI forward tracks — re-plan at the boundary.
>
> **Update (2026-07-20) — Milestone 9 COMPLETE; Milestone 10 (Advanced lighting) kicks off.** M9's
> close-out landed on `main`: **m9.8 docs true-up (#84)** and the **gizmo/inspector Edit-mode fix
> (#83)** are merged, so Editor v1 is done — *build a scene, tweak components, hit Play* works. **M10
> begins** with **[ADR-0032](adr/0032-lighting-v2.md) (Accepted)** — lighting v2: SDF-traced **DDGI**
> global illumination, **cascaded + local shadow maps**, **clustered-forward** many-lights, **SSR**
> reflections, and the **destruction-coupling contracts (C1–C6)** that make the milestone's thesis —
> *break a wall, the shadow moves and the bounced light updates* — provable. Three lead rulings are
> baked into the ADR: (1) **build the real array/cube texture RHI** for cascade/cube-face storage
> rather than atlas-hack around it; (2) **`Application` grows an ordered sim stage** so the per-tick
> lighting hook has a home in a generic app, not just a sample's `main.cpp`; (3) **debris gets a
> bounded visual-retirement stage** that emits a world-bounds event, so lighting caches can't leak
> over a long session. **Confirmed brick ladder:** **m10.0** ADR-0032 + SDF-trace spike + this ladder ·
> **m10.0-perf** editor idle-frame skip (an idle editor must cost ≈0% CPU before M10 piles on GPU work) ·
> **m10.1a** RHI top-up (array/cube textures + depth-compare sampler) · **m10.1** directional CSM ·
> **m10.2** local-light shadows + destructibility-aware shadow cache · **m10.3** clustered forward
> (+`RGBuffer`) · **m10.4** SDF clipmap · **m10.5a/b** DDGI probes · **m10.6** GI integration + the
> walls-fall proof · **m10.7** SSR reflections · **m10.8** milestone proof + docs; **m10.i**
> (virtualized geometry) floats as interleave filler. CI stays lavapipe — **no hardware RT, no mesh
> shaders**; absolute frame budgets wait for real hardware at m12.0.
>
> **Update (2026-07-20) — Milestone 9 (Editor v1) nearly complete; closing out at m9.8.** Bricks
> **m9.0–m9.7 are all merged to `main`** (PRs #68–#82): the editor-architecture ADR, the engine-side
> editor host (`engine/editorhost`), the `.rscene` scene format, the Rust shell + streamed viewport, the
> reflection-driven **inspectors with undo/redo** (m9.4), the **asset browser + placement** (m9.5),
> **viewport picking + transform gizmos** (m9.6, Fable), and **play-in-editor** (Edit ↔ Playing/Paused,
> snapshot → sim → bit-exact restore — m9.7, Fable). The **s1.4 local socket** carries all of it. So the
> M9 "done when" — *build a small scene, tweak components, hit Play* — is functionally reachable today.
> **m9.8 (this brick) closes the milestone:** the docs true-up (this file, ARCHITECTURE, the glossary,
> the editor README's "first five minutes"), a headless **scripted-story** CI proof, and the hand-driven
> **Mac human proof** with an honest editor-UX backlog. **A fast-follow fix (#83)** made gizmo/inspector
> edits move the object live in Edit mode — m9.7's tick policy had deferred the `LocalTransform` →
> `WorldTransform` compose to Play — and dropped a duplicate transform row from the inspector. **Not yet
> in the editor (documented, not faked):** a destruction cameo in play-in-editor — `DestructionWorld`
> isn't editor-bound yet (ADR-0029 §6), so the Play proof is physics-only for now. **Next:** m9.8 close,
> then Milestone 10.
>
> **Update (2026-07-17) — Track S1 complete; Milestone 9 (Editor v1) kicks off.** The full S1 streaming
> runway (s1.0–s1.4) is **merged to `main`** (PRs #63–#67, all green on Windows/Linux/macOS): the
> streaming-v1 ADR, async readback, the AV1 codec (SVT-AV1 + dav1d), the input-v2 **latency ledger**, and
> the **local fast path** (`LocalSocket` over AF_UNIX + a transport-generic `ProtocolConnection`) — the
> editor viewport's wire. **M9's hard gate is satisfied, so Editor v1 begins.** **M9.0 landed
> [ADR-0031](adr/0031-editor-v1.md)** — the editor is a **client of a live engine process**
> ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)): the editor launches the engine as a child
> (`--editor-host`) for crash isolation; the **editor channel** is new message families in the reserved
> 0x02xx band (schema-hash handshake, entity-tree snapshot + change-detection deltas, component get/set
> by reflection bytes, spawn/despawn, asset manifest, scene ops, pick, gizmo, play control); the **Rust
> UI toolkit is egui + egui_dock** (immediate-mode fits live data, MIT/Apache, docking exists — perf
> validated in m9.3 on a display, since this build box is headless); **play = snapshot → sim → restore
> bit-exact** (the fixed tick makes step honest); undo = a command over the mutating messages; the
> **viewport is the s1.4 local LZ4 session**. Invariants: the editor never links engine internals,
> inspectors are *generated* from reflection, every host behaviour is provable headless (the UI is
> Mac-eyeballed). **M9 is decomposed into bricks m9.0–m9.8:** **m9.0** ADR-0031 (this) · **m9.1**
> engine-side editor host (`engine/editorhost`) · **m9.2** `.rscene` scene format · **m9.3** the Rust
> shell + live viewport · **m9.4** outliner + reflection-driven inspectors · **m9.5** asset browser ·
> **m9.6** picking + gizmos (Fable) · **m9.7** play-in-editor (Fable) · **m9.8** the proof (build a scene,
> tweak, hit Play). **Next:** m9.1 — the editor host.
>
> **Update (2026-07-17) — Milestone 8 complete; Track S1 kicks off as the M9 runway.** M8 (Destruction
> v1) landed on `main` (through `samples/10-destructible-wall`, PR #62). M9 (Editor v1) is next, but its
> **hard entry gate is Track S1 streaming** — the editor is a *client of the engine* over the streaming
> protocol ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)), and s1.4 is the viewport's local
> wire. S1 had been skipped on the way here (after M6 the path ran S0.7 → M7 → M8), so the decision this
> session (Luca): **build the full S1 track (s1.0–s1.4) before M9**, rather than shortcut the editor onto
> the S0 TCP loopback. **S1.0 landed [ADR-0030](adr/0030-streaming-v1.md)** — the streaming-v1 decisions:
> the inter-frame **wire codec is AV1** (SVT-AV1 encode + dav1d decode), chosen on the same ship-safe
> licensing test that ruled out GPL x264 in ADR-0017 — AV1 is royalty-free, H.264 rides the MPEG-LA patent
> pool; `Codec::Av1 = 3` is appended, **JPEG stays the intra fallback, LZ4 the lossless/local editor
> path**; **async readback** kills the measured S0 capture stall and needs one new RHI primitive (a
> non-blocking submit + completion token — the Vulkan backend already fences frames-in-flight; S0 exposed
> only `submit_blocking`/`wait_idle`); a **seven-stage latency ledger** (no NTP — echoed-timestamp one-way
> estimates) makes glass-to-glass honest; and the protocol grows (codec negotiation, a parameter-set
> message, a keyframe-request seam; `kProtocolVersion` bumps). **S1 is decomposed into bricks s1.0–s1.4:**
> **s1.0** ADR-0030 (this) · **s1.1** async readback (the RHI completion primitive + an N-deep readback
> ring, latest-wins drop) · **s1.2** the AV1 software codec (SVT-AV1 + dav1d behind
> `VideoEncoder`/`VideoDecoder` seams; hardware encoders slot in later; the confirming `codec_bench`
> numbers land here) · **s1.3** input v2 + the latency ledger · **s1.4** the **local fast path**
> (UDS/named-pipe transport + LZ4-lossless default) — **the M9 viewport's wire, the one hard M9 blocker**.
> Proofs stay GPU-free/structural on lavapipe; the video codec serves the WAN path, off the editor's
> LZ4-lossless critical path. **Next:** s1.1 — async readback.
>
> **Update (2026-07-04) — Milestone 4 merged to `main`; M5 begins.** The M4 stack landed via PRs
> #11, #15, #13, #14 (#12 was closed by a base-branch race when #11 merged; #15 supersedes it),
> CI-green on all three OSes + both sanitizer jobs. One first-contact find on the way in, caught
> by the Clang TSan job — the first compiler to build `rime_ecs` under Clang: Clang < 19 defaults
> sized-deallocation off, hiding the sized+aligned `operator delete`, so superblocks now free
> through the unsized aligned form. **M5 — the render graph + PBR — is decomposed into bricks
> M5.0–M5.9** (see the M5 detail below). Scope decisions recorded: **no shadows** in M5 (M10 owns
> them), **no IBL**/cube textures, dogfood acceptance is an offscreen **test** expressing the
> viewer's frame as a graph (ADR-0016 rule 4 — no viewer port), and first light gets watched
> **live over Track S0 streaming** (`07-first-light --serve`) since the dev server is headless.
> **M5.0 landed:** [ADR-0019](adr/0019-render-graph.md) settles the graph architecture —
> **frame-declared passes** (rebuilt every frame, UE-RDG-style), **virtual resources** with a
> desc-keyed transient cache, **declared access driving order, culling, *and* barriers** (emitted
> as explicit transitions through a new RHI seam, cashing in the deferral `command_buffer.hpp`
> has documented since M3), raster/compute/copy pass kinds, **serial single-queue v0 with the
> parallel + async-compute seams kept open**, and per-pass GPU timestamps from day one.
>
> **Update (2026-07-03) — Track S0 landed; M4 (ECS) kicks off.** The **S0 dev-stream** track is in:
> blocking TCP sockets (S0.1), the `engine/stream` frame tap (S0.2), the JPEG/LZ4 codec
> (S0.3, [ADR-0017](adr/0017-streaming-codec.md)), the versioned protocol (S0.4), an engine-side
> loopback proof (S0.6), and the headless `samples/04-remote-view` server+client (S0.5) — the full
> render→capture→encode→transport→present→input loop, verified GPU-free on lavapipe (S0.5's *windowed*
> client is the one piece deferred to a machine with a display). **M4 — ECS / the world — now begins**,
> decomposed into bricks **M4.0–M4.6** (see the M4 detail below). **M4.0 landed:**
> [ADR-0018](adr/0018-ecs-storage-model.md) settles the storage model — **archetype/SoA chunked
> tables**, generational-`Handle` entities, chunks drawn from `core`'s (now load-bearing) allocators,
> and change detection designed in from day one. **M4.1 – M4.3 have since landed** — the `engine/ecs`
> module: the generational entity directory + reflection-aware component registry (M4.1), the
> allocator-backed chunk storage primitives `ChunkPool` / `ChunkLayout` / `Chunk` (M4.2a), the
> `World` archetype integration — `spawn`, add/remove component = **archetype move**, `get`/`has`,
> directory `location` wired (M4.2b), **`Query<Ts...>`** — column-wise iteration over the entities that
> have a given component set (M4.3), and **`Query::par_for_each`** — that iteration run across all cores
> with **one chunk per job** (chunks are separate pooled buffers ⇒ no false sharing), the engine's
> first real multicore load on the M1.6 deque; the Phase 0 **TSan** CI job now nets `rime_ecs_tests`
> too (M4.4a), and the **`System` + `Schedule`** scheduler that batches systems into parallel **phases**
> from their declared read/write **access sets** — independent systems run side by side, conflicting
> ones fall into ordered phases (M4.4b), and a **`CommandBuffer`** that records structural edits
> (spawn/despawn/add/remove) from inside a system — thread-safe under `par_for_each` — for the schedule
> to apply at each phase boundary (M4.4c). **M4.4 is complete**: all ASan+UBSan-clean, and the
> data-parallel, concurrent-systems, and concurrent-recording paths are all TSan-clean. **M4.5 has
> since landed** — the **transform hierarchy**: `LocalTransform` / `WorldTransform` / `Parent` +
> `propagate_transforms` composing `world = parent.world * local` depth-by-depth, each level updated in
> parallel (flat scenes take a fully-parallel fast path); derivation in
> [docs/math/transform-hierarchy.md](math/transform-hierarchy.md). Change detection's dirty-subtree
> optimization is **deferred** (measure before optimize; recompute-all is correct + parallel).
> **M4.6 has landed, and with it MILESTONE 4 IS COMPLETE**: the proof sample
> `samples/05-ecs-playground` runs both of M4's "done when" clauses green — **200k entities stepped in
> parallel** through the ECS (a `Query::par_for_each` integrate system on a `Schedule`), timed against a
> serial baseline at **≈10× on 16 cores (Release)** and verified bit-for-bit identical, and a **transform
> hierarchy** (a tank: hull → turret → barrel → muzzle) composing `world = parent·local` correctly and
> following its root when moved. `engine/ecs` is in the default build; the sample self-checks (non-zero
> exit on failure). **Next:** M5 — the render graph + PBR (first light).
>
> **Update (2026-07-03) — Phase 0: land + harden.** `feat/icem-viewer` (all of M3 plus the ICEM
> viewer through ladder **F**) merged to `main` via **PR #2** and is now **CI-green on Windows,
> Linux, and macOS** (Linux on lavapipe, `RIME_REQUIRE_VULKAN=1`) — that 57-commit branch had never
> run CI before. Landing it needed the X11 leaky-macro fix (the engine had not compiled on Linux
> since M3.1). CI now also builds `feat/**` pushes, and two Linux **sanitizer** jobs guard the
> lock-free code (ASan+UBSan over all suites; Clang **ThreadSanitizer** over the deque + job system).
> Direction set this session: **the editor is a client of the engine**
> ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)) and a graphics-**streaming** track
> (Track S — S0 dev-stream now, shippable remote play later; see Cross-cutting tracks).
>
> **Status (2026-06-21):** **Milestones 0 (build bootstrap) and 1 (core foundation) —
> COMPLETE.** The repo is public at https://github.com/LucaSct/rime with **CI green on
> Windows, Linux, and macOS**. `scripts/build` builds and tests the C++ engine and the Rust
> tooling with warnings-as-errors; format / lint / license-header gates are enforced in CI.
>
> **M1 (core foundation) is done — all four "done when" proofs pass:** the unit-test battery
> is green; `samples/jobs_core_saturation` saturates the cores through the work-stealing job
> system; reflection describes and round-trips a struct through bytes; and a module loads at
> runtime. Bricks M1.1–M1.8 landed as focused commits (see `git log`): diagnostics
> (log/assert/timing) · allocators (arena/stack/pool, tracked) · math I (vectors & matrices)
> · math II (quaternions & transforms) · containers (generational slot map) · the lock-free
> work-stealing deque (M1.6a) and the job system + core-saturation sample (M1.6b) · minimal
> reflection · the runtime module loader. Math bricks ship derivation notes (`docs/math/`),
> systems bricks ship design notes (`docs/design/`), and decisions live in ADRs 0004–0005.
>
> **Milestone 2 (Platform & window) — COMPLETE.** `engine/platform` provides window, input,
> filesystem, timers, and threads behind a seam with no OS `#ifdef`s leaking upward. All bricks
> landed (see `git log`): **M2.1** module & seam · **M2.2a–d** a native window + event pump on
> **Cocoa, Win32, X11, and Wayland** (Linux selects Wayland or X11 at runtime) · **M2.3** polled
> keyboard/mouse input · **M2.4** filesystem + frame timer · **M2.5** the `00-hello-window` proof.
> CI builds and link-checks all four backends on Windows/Linux/macOS; the runnable proof opens a
> window and handles input on Cocoa/Win32/X11 (a Wayland surface is created and event-wired but maps
> on screen once the M3 renderer attaches a buffer).
>
> **Milestone 3 (RHI + Vulkan backend) — COMPLETE.** The graphics seam `engine/rhi` and its Vulkan
> backend are up: a `Device` (volk + VMA, Vulkan 1.3 dynamic rendering + synchronization2), offline
> GLSL→SPIR-V shaders, and a triangle rendered **off-screen** with a pixel-readback proof
> (**M3.1–M3.3**); **swapchain presentation** — the same triangle in a real window via frames-in-flight,
> with surfaces built from `platform::NativeWindow` across all four window systems (**M3.4**, ADR-0009);
> and **index buffers + texture upload + samplers + a combined-image-sampler descriptor model** that
> draw M3's "done when" — a **textured quad**, pixel-verified off-screen (`tests/rhi/textured_quad_test`,
> four R/G/B/Y quadrants) and presented in a window (**M3.5**, ADR-0010). Verified locally on
> macOS/MoltenVK (Vulkan 1.3.334); the off-screen proofs keep M3 runnable **GPU-free in CI** on lavapipe,
> mirroring M2's headless split. Decisions in ADRs 0007 (Vulkan bootstrapping), 0008 (offline shaders),
> 0009 (swapchain/presentation), and 0010 (textures & descriptors). **Next:** M4 — ECS / the world.

## Ordering principles (why this sequence)

1. **Bottom-up the layer cake** ([ARCHITECTURE.md](ARCHITECTURE.md)): destruction needs
   physics; physics needs core/jobs/math; rendering needs the RHI. Earn each layer
   before standing on it.
2. **Seams before features.** The render graph exists *before* Lumen-class GI; the
   part/physics model *before* spectacular destruction. The hard-to-retrofit seams
   (RHI, ECS, destruction event model) go in early — that's the whole bet.
3. **Every milestone ends in a runnable proof** — a `samples/` demo and/or a CI gate.
4. **Power > portability at the edges.** All three OSes are CI-gated from M0; if a
   portability cost ever threatens engine quality, we narrow platforms (VISION #2).
5. **Math is derived, not hand-waved.** Math-heavy milestones (M1, M5, M7, M10) ship a
   short derivation note alongside the code.

## Cross-cutting tracks (continuous, not milestones)

- **CI/CD:** build + test on Windows/Linux/macOS from M0; format/lint/license-header
  gates; warnings-as-errors.
- **Testing & profiling:** unit tests per module; a profiling/timing hook in `core`
  early, so "measure before optimize" is real.
- **Docs:** keep ARCHITECTURE, glossary, and ADRs current as we build.
- **Audio & animation:** feature tracks that slot in — audio *stub* at M8 (destruction
  event fan-out), real audio ~M8–M9; skeletal animation ~M6–M7.
- **Simulation effects (Tracks FX & FL):** dedicated modules for the hardest simulation domains,
  building on the M7 physics core's seams and the render graph. **Track FX** (`engine/vfx`) — a GPU
  particle substrate with fire and dust/smoke as effect families (spawned from the M8 destruction event
  fan-out; fire drives lights, smoke reads the M10 lighting data); it replaces M8.4's dust stub and
  hard-gates M12's block — **scoped at m12.0** to its true size: fx1a's GPU draw pass for the existing
  deterministic CPU sim is load-bearing, fx1b's compute scale-up is contingent on the ledger, and
  fire-as-light is deferred behind its seam. **Track FL** (`engine/fluids`) — CPU heightfield water
  with two-way buoyancy coupling into physics; *decided at M12.0, and the decision was **no**:* no
  water in the block ([ADR-0035](adr/0035-vision-demo-m12.md) §5). A whole module plus a two-way
  physics coupling does not earn a slot in a demo whose thesis is destruction, lighting and
  networking at scale; the ADR-0026 substrate seams stay intact for whenever it opens. Both are
  cross-cutting (interleave under mainline-first), not
  milestones; most of both is provable GPU-free/structural on lavapipe. *Inspired by: Frostbite/Niagara
  effects; shallow-water + SPH literature.*
- **Graphics streaming (Track S):** the engine renders → captures → encodes → transports → a thin
  client presents and sends input back. **S0** (LAN/loopback dev-stream — TCP, JPEG/LZ4, a thin
  Rime-built client) lands right after Phase 0, before M4; **S1+** (hardware codecs, QUIC/WebRTC)
  post-M5. It is a **shippable engine feature** (`engine/stream` over `engine/net`), not dev-only
  tooling, and the *same* versioned protocol carries the M9 editor viewport
  ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)). Ship-safe codecs only under
  Apache-2.0 — never GPL x264 in the engine.

---

## Milestones

| # | Milestone | Done when (the proof) |
| --- | --- | --- |
| **M0** | Build bootstrap & skeleton | CI green on Win/Linux/macOS; `hello` runs; a trivial test passes |
| **M1** | Core foundation | test battery green; a sample saturates all cores via the job system; reflection describes & serializes a struct; a module loads at runtime |
| **M2** | Platform & window | a window opens and handles keyboard/mouse on all three OSes |
| **M3** | RHI + Vulkan backend | a textured quad renders through the RHI (Win/Linux + macOS/MoltenVK) |
| **M4** | ECS / the world | 100k+ entities update in parallel; transforms compose correctly |
| **M5** | Render graph + PBR | a lit PBR scene draws via the render graph; adding a pass is easy |
| **M6** | Asset pipeline + runtime assets | import → cook → load → render a real glTF model with textures |
| **M7** | Physics (rigid bodies) | objects fall/collide/stack; raycasts hit; runs parallel to the frame |
| **M8** | **Destruction v1** | a wall fractures on impact, debris falls/settles, one event drives a VFX+sound stub |
| **M9** | Editor v1 (Rust) | build a small scene in the editor, tweak components, hit Play |
| **M10** | Advanced lighting | dynamic GI updates as the scene changes — *including when walls fall* |
| **M11** | Networking + networked destruction | two clients see synchronized destruction at meaningful scale |
| **M12** | **"The Player"** ✅ | a server and two clients run a predicted, reconciled player under scripted loss: own-input response ≤ 1 tick against a prediction-off control, remote motion continuous, both clients converging bit-exactly — GPU-free and CI-gated (`samples/13-networked-player`) |
| **M13** | **"The Block" (vision demo)** | a destructible urban block (M8+M10+M11+M12) runs at a playable frame rate and *feels* right |

### Detail

**M0 — Build bootstrap & skeleton.** One command builds the C++ engine and the Rust
tools on all three OSes. CMake presets + a trivial `engine/core` lib and a `hello` exe;
C++ test harness; Cargo workspace under `tools/`; `scripts/setup`+`scripts/build`; a CI
matrix with format/lint/license gates. *Inspired by: modern C++/Rust project hygiene.*

**M1 — Core foundation.** The bedrock: allocators (arena/pool/stack, tracked); SIMD math
(+ derivation notes); cache-friendly containers (slot map, handle table); a
**work-stealing job system**; logging/asserts + profiling hooks; minimal **reflection**;
the **module loader**. *Inspired by: O3DE modules; Bevy/DOD.*

*Bricks (planned 2026-06-17, bottom-up):* **M1.1** diagnostics (log/assert/timing) ·
**M1.2** allocators (arena/stack/pool, tracked) · **M1.3** math I — vectors & matrices
(+derivation) · **M1.4** math II — quaternions & transforms (+derivation) · **M1.5**
containers — slot map / handle table · **M1.6** work-stealing job system (+ a sample that
saturates all cores) · **M1.7** minimal reflection (describe + serialize a struct) ·
**M1.8** module loader. Proofs map to M1's "done when": test battery (all) · cores
saturated (M1.6) · struct serialized (M1.7) · module loaded at runtime (M1.8). After M1.1,
the memory / math / reflection lines can proceed in parallel.

**M2 — Platform & window.** `engine/platform` — window, input, filesystem, timers,
threads for Win32/Linux/macOS. No OS `#ifdef`s leak upward. Sample `00-hello-window`.

*Bricks (planned 2026-06-17, bottom-up):* **M2.1** platform module & seam — the
`engine/platform` target, public interface headers, and OS-backend selection in CMake
(backends compiled per-OS under `src/<platform>/`, never `#ifdef`-ed into public headers),
plus `init`/`shutdown`, a monotonic clock, and thread-naming; *proof:* builds and links on
all three OSes (CI). · **M2.2** window — open/close/resize and the event pump behind a
`Window` interface, implemented **natively** (no GLFW/SDL — ADR-0006). Decomposed per-OS:
**M2.2a** `Window`/event seam + native-handle struct + null backend + **Cocoa**; **M2.2b**
**Win32**; **M2.2c** **X11/Xlib**; **M2.2d** **Wayland** + runtime backend selection. · **M2.3**
input — keyboard & mouse as events + polled state through the platform interface. · **M2.4**
filesystem & time — file/path utilities (read/write, exists, base dirs) and a
high-resolution frame timer (complementing `core` profiling). · **M2.5** sample
`00-hello-window` — **the proof:** a window opens, handles keyboard/mouse, and closes
cleanly, CI-built on all three OSes. M2's "done when" maps to M2.5. After M2.1, the
window/input and filesystem/time lines can proceed in parallel.

> *Decided for M2.2 (ADR-0006):* the windowing/input backend is **native** — Win32 / Cocoa /
> Xlib + Wayland — for full control of DPI, raw input, cursor capture, event timing, and
> fullscreen, and to ship zero windowing dependencies (VISION #1). The `Window`/event seam keeps
> a backend swappable (a GLFW backend could still be added later for bring-up). "Native" is
> scoped to windowing/input; the clock and filesystem use `std::chrono`/`std::filesystem`, with
> native shims only where std has no answer (thread naming, exe path, user dirs).

**M3 — RHI + Vulkan backend (first pixels).** `engine/rhi` interfaces (device, swapchain,
command buffers, pipelines, descriptors, sync) + the **Vulkan backend** (only place that
includes Vulkan headers); GLSL/HLSL→SPIR-V; VMA. Samples `01-hello-triangle` → textured
quad. *(ADR-0002.)*

*Bricks (planned 2026-06-18, bottom-up):* **M3.1** the `engine/rhi` seam (agnostic interface +
opaque handles) and Vulkan device bring-up — instance + validation, physical-device selection
(requires Vulkan 1.3 + dynamic rendering + synchronization2), logical device + queue, VMA; *proof:*
builds & link-checks the backend on all three OSes (CI) and a headless device-creation test on
**lavapipe**. · **M3.2** offline GLSL→SPIR-V (`rime_add_shaders`, mirroring `wayland-scanner`) + the
RHI `Shader`. · **M3.3** graphics pipeline (dynamic rendering, no render-pass objects), the command
encoder, vertex buffers/textures, and an **off-screen triangle verified by pixel readback** — the
GPU-free "first pixels" proof (`tests/rhi`), runnable as `samples/01-hello-triangle`. · **M3.4**
`VkSurfaceKHR` from `platform::NativeWindow` (all four window systems) + swapchain + frames-in-flight
+ present — the triangle in a real window (Win/Linux + macOS/MoltenVK), and the M2 Wayland surface
finally maps. · **M3.5** index buffers, texture upload + sampler + descriptor model → the **textured
quad** (M3's "done when"). M3.1–M3.3 land the first triangle off-screen; M3.4–M3.5 put it on screen
and texture it.

> *Decided for M3 (ADRs 0007–0008):* the Vulkan backend uses the **volk** meta-loader (no loader
> linked at build time), **VMA** for memory, and a **Vulkan 1.3** baseline — **dynamic rendering +
> synchronization2**, so there are no `VkRenderPass`/`VkFramebuffer` objects and the RHI maps cleanly
> onto the M5 render graph. Shaders are compiled **offline** (GLSL→SPIR-V at build time) and embedded;
> the engine ships no runtime shader compiler. Build dependencies come from **Conan**; the runtime
> loader + ICD (a GPU driver, **MoltenVK** on macOS, **lavapipe** on GPU-less CI) are the
> environment's, so the off-screen render proof runs green on all three OSes without a GPU.

**M4 — ECS / the world.** `engine/ecs` — entities, components, archetype storage,
parallel systems on the job system, queries, a transform hierarchy. Sample
`05-ecs-playground`.

*Bricks (planned 2026-07-03, bottom-up):* **M4.0** the storage-model decision —
**archetype/SoA chunked tables** over sparse sets, entity IDs on the generational
`core::Handle`, chunks drawn from `core`'s allocators (the allocator module finally becomes
load-bearing), and **change detection** via per-component version stamps designed in from day
one ([ADR-0018](adr/0018-ecs-storage-model.md)); *proof:* the ADR — no code, the decision the
rest of M4 cites. · **M4.1** the `engine/ecs` seam + **entity directory** (generational spawn /
despawn / liveness / recycling) + **component registration through reflection** (registered once
⇒ serializable now, editor-inspectable at M9; extends `RIME_REFLECT_*`). · **M4.2**
**archetype / chunk storage**, in two steps: **M4.2a** the storage primitives — an allocator-backed
`ChunkPool` (16 KiB blocks from `core`'s pool allocator, finally load-bearing), the per-signature SoA
`ChunkLayout`, and the `Chunk` row store with swap-remove · **M4.2b** the World integration — an
archetype keyed by `ComponentSignature`, spawn-with-components, and add/remove component = archetype
move (the entity relocates; its directory location is wired). · **M4.3** **queries + chunk-wise
iteration** — find the archetypes matching a signature and scan their columns. · **M4.4** the
**parallel system scheduler** on the `JobSystem`, in three steps: **M4.4a** `Query::par_for_each` — the
query body run across all cores with **one chunk per task** (chunks are separate pooled buffers ⇒ no
false sharing), the first real multicore load on the Chase-Lev deque, with Phase 0's TSan job
extended over `rime_ecs_tests` as the net · **M4.4b** the **`System` + `Schedule`** scheduler — declared
read/write **access sets** batched into parallel **phases** (ASAP leveling of the conflict order:
independent systems run together, conflicting ones keep declared order), phases run concurrently on the
job system · **M4.4c** **deferred structural changes** — a command buffer applied at phase boundaries,
lifting the no-structural-change-inside-a-system rule. · **M4.5** the
**transform hierarchy** — `LocalTransform`/`WorldTransform`/`Parent` + `propagate_transforms`,
`core::Transform` composition (`world = parent.world * local`) processed depth-by-depth with each level
updated in parallel (flat scenes take a fully-parallel fast path); the change-detection dirty-subtree
optimization is deferred (measure first). · **M4.6 (done)** the proof sample
`samples/05-ecs-playground` — **200k entities updating in parallel** (≈10× on 16 cores, Release, verified
bit-for-bit vs serial) and **transforms composing correctly** (M4's "done when"), self-checking;
`engine/ecs` builds by default. M4.0–M4.3 build the world's data model bottom-up; M4.4 runs systems over
it in parallel; M4.5–M4.6 land the two proofs. A `docs/design/ecs.md` note accompanies the storage
bricks and a `docs/math/` derivation the transform hierarchy. **Milestone 4 complete.**

**M5 — Render graph + PBR (first light).** `engine/render` — **render graph** (passes,
transient resources, auto-barriers), mesh/material/camera, **PBR** (+ derivation), depth
pre-pass, one dynamic light. Samples `06-render-graph`, `07-first-light` (renumbered — 03/04
were taken by the viewer and remote-view). The home for
M10. *Inspired by: UE5 render-graph discipline.* — *Note: several RHI features were pulled
ahead of M5 to unblock the ICEM viewer — the **depth attachment** + depth test (ADR-0011),
**push constants** (ADR-0012), and **3-D/volume textures** (ADR-0013, for field colormaps).
The render graph adopts and extends them (multiple targets, stencil, MSAA, streamed volumes).*

> **Status (2026-07-06): MILESTONE 5 COMPLETE — M5.0–M5.9 built and green on lavapipe.** The RHI
> top-ups (M5.1–M5.3), the **render graph** (M5.4), the **scene layer** (M5.5), the **forward-PBR
> pipeline** (M5.6, [math/pbr.md](math/pbr.md)), the **fixed-tick application loop** (M5.7,
> ADR-0023), the two proof samples (M5.8) — `07-first-light` draws M5's "done when": a lit PBR scene
> through the graph, headless-self-checked and streamable over Track S0 — and the **dogfood
> acceptance test** (M5.9) are all in. M5.9 re-expresses the ICEM viewer's cross-section frame
> (clip-planed lit mesh → stencil cut-mark → solid cap → alpha-tested UI overlay) as four
> render-graph passes **sharing one colour + one D32FloatS8 depth+stencil target**, proving the
> graph's resource model covers depth+stencil attachments and Load/keep-across-passes semantics
> (`tests/render/viewer_frame_graph_test.cpp`, offscreen, GPU-free in CI, ADR-0016 rule 4).
> **Next:** M6 — asset pipeline + runtime assets (import → cook → load → render a real glTF model).

*Bricks (planned 2026-07-04, bottom-up):* **M5.0** the architecture decision — a **frame-declared
render graph** with virtual resources, declared access driving order *and* barriers, graph-owned
transitions through a new RHI barrier API, serial single-queue v0 with the parallel seams kept
([ADR-0019](adr/0019-render-graph.md)); *proof:* the ADR — no code, the decision the rest of M5
cites. · **M5.1** RHI top-up I — **descriptor model v2**: declared binding layouts, **uniform
buffers**, per-frame descriptor pools (ADR-0020); then **blending**, **multiple render targets**,
and **RGBA16Float** (the HDR scene format); *proofs:* UBO-driven draw, blended quads, MRT —
pixel-verified, GPU-free on lavapipe. · **M5.2** RHI top-up II — **compute pipelines + dispatch +
storage buffers/images** (ADR-0021); *proofs:* compute pattern → exact readback; compute-written
image sampled by a draw. · **M5.3** RHI top-up III — **mipmaps** (blit-generated chains) +
**anisotropic sampling**; **GPU timestamps** + debug names/labels (RenderDoc legibility);
*proofs:* minification pixel test; monotonic timestamps. · **M5.4** the **render graph v0** —
`RGTexture`/`RGBuffer`, `add_pass` (raster/compute/copy), `import`, compile (resource versioning →
edges → topological order → cull), the transient cache, graph-emitted barriers, per-pass timing;
*proofs:* multi-pass pixel tests (incl. compute-in-graph, pass culling, transient reuse); compile
overhead measured and recorded. · **M5.5** the **scene layer** — `OrbitCamera` graduates from the
viewer (ADR-0016 rule 3), procedural mesh primitives + mesh/material registries, ECS render
components registered through reflection (`Camera`, `MeshRef`, `MaterialRef`, lights). · **M5.6**
the **PBR forward pipeline** — depth pre-pass → Cook-Torrance forward shading into HDR → tonemap,
as reusable graph passes, + the `docs/math/pbr.md` derivation (ADR-0022); *proofs:* structural
radiometric asserts on a metallic×roughness sphere grid. · **M5.7** **`engine/app` becomes real**
— the fixed-tick frame loop (simulation ticks decoupled from render frames — the M11 seam;
ADR-0023) with a headless mode; *proofs:* tick determinism; a headless CI run. · **M5.8** the
**proof samples** — `06-render-graph` ("adding a pass is easy," demonstrated in ~10 lines) and
`07-first-light` (M5's "done when": the lit PBR scene through the full stack; `--headless`
self-check in CI; `--serve` streams it over Track S0 to the thin client for the first live look).
· **M5.9 (done)** **dogfood acceptance** — the ICEM viewer's frame (mesh + stencil cap + UI overlay)
expressed as a render graph in an offscreen test (ADR-0016 rule 4). M5.1–M5.3 make the RHI
renderer-ready; M5.4 is the seam itself; M5.5–M5.7 build the scene and the loop; M5.8–M5.9 land
the proofs. **Milestone 5 complete.**

**M6 — Asset pipeline + runtime assets.** `tools/asset-pipeline` (Rust) imports glTF +
textures → cooked formats; `engine/assets` loads/streams at runtime; `tools/rime-cli`
cooks; the stable C-ABI/file **FFI boundary** stands up. *(ADR-0001.)* Skeletal-animation
import begins.

> **Status (2026-07-11): MILESTONE 6 COMPLETE — the whole offline→runtime asset pipeline is built
> and green on lavapipe.** [ADR-0024](adr/0024-asset-model.md) settled the model — **Rust cooks, C++
> loads, files are the boundary** (ADR-0001 made concrete) — and M6.1–M6.10 made it real: the `RMA1`
> container + `engine/assets` reader/registry/manifest (M6.1); the `tools/asset-pipeline` crate + the
> `rime` CLI cooking **glTF meshes** (M6.2), **textures** with gamma-correct offline mip chains
> (M6.3), **PBR materials** with MikkTSpace tangents + normal/MR/AO/emissive maps (M6.4), and
> **skeletons + animation clips** with a CPU sampler (M6.7); **async loading** on the job system with
> placeholder assets + the **GPU asset bridge** (M6.5, [ADR-0025](adr/0025-gpu-asset-bridge.md)); the
> **STL dogfood** in the ICEM viewer (M6.6); the **SDK** (`find_package(rime CONFIG)`, an out-of-tree
> consumer built in CI, M6.8); and the **C ABI** `librime_capi` + the `rime-ffi` crate (M6.9).
> `samples/08-gltf-zoo` (M6.10) runs the milestone's "done when" end-to-end: **import → cook → load →
> render** three real glTF models (base-color-textured cube, normal-mapped metallic-roughness sphere,
> and a CPU-posed skinned rig) through the render graph — self-checked headless in CI and streamable
> live over Track S0. **M7 — physics (own rigid-body core) — the milestone's "done when" is met**:
`samples/09-physics-playground` self-checks (objects fall/collide/stack, a raycast+impulse topples a
tower, everything sleeps, two runs hash identically) headless in CI. **Next:** M8 — destruction v1
(building on the physics core's seams), or the M7 fast-follows (contact events, shapes II, CCD).

*Bricks (planned 2026-07-06, bottom-up):* **M6.0 (done)** the asset-model decision
(ADR-0024); *proof:* the ADR. · **M6.1** `engine/assets` is born — the RMA1 reader
(trust-nothing decode, negative-battery tested), `AssetId`, the handle-based registry,
synchronous mesh loads. · **M6.2** the Rust `tools/asset-pipeline` crate + **glTF mesh
import→cook** and `rime-cli cook`/`inspect`; the cross-language golden fixture (Rust cooks
what C++ reads in CI). · **M6.3** **textures** — PNG/JPEG decode, offline linear-space mip
chains, sRGB-vs-linear by semantic; RHI top-up: uploading pre-generated mip data. ·
**M6.4** **materials + the PBR texture upgrade** — cooked metallic-roughness materials;
normal mapping (MikkTSpace tangents at import), MR/occlusion/emissive textures in the
forward-PBR shaders (+`docs/math/tangent-space.md`). · **M6.5** **async loading** on the
job system — IO/parse jobs, frame-point GPU-upload drain, placeholder assets, TSan-netted.
· **M6.6** dogfood — **STL import→cook** and the ICEM viewer loading cooked meshes
(ADR-0016 rules 3+4; the content-hash cook cache earning its keep on real multi-MB parts).
· **M6.7** **skeletal-animation import begins** — glTF skins/skeletons/clips cooked + a
CPU clip sampler with paper-checked poses (GPU skinning follows at M7). · **M6.8** the
**SDK story** — CMake install/export, `find_package(rime CONFIG)`, an out-of-tree consumer
app built in CI (arms the ICEM-migration trigger, ADR-0016 rule 5). · **M6.9** the
**C-ABI FFI boundary stands up** — a tiny `rime_capi` shared library + a Rust FFI crate
whose tests drive the engine's own loader; protocol message-type space reserved for the M9
editor channel. · **M6.10 (done)** the proof — `samples/08-gltf-zoo`: cook → load → render three
**hand-authored** glTF models (base-color-textured, normal-mapped + metallic-roughness, and a
CPU-posed skinned rig — first-party, so no third-party asset licenses), `--headless` self-check in
CI, `--serve` streamed live; docs true-up. M6.1–M6.2 land the boundary, M6.3–M6.5 make assets
real, M6.6–M6.7 widen the funnel, M6.8–M6.9 open the SDK/FFI doors, M6.10 closes the milestone.
**Milestone 6 complete.**

**M7 — Physics (rigid bodies, multicore).** `engine/physics` behind an interface —
bodies, collision, queries — stepped on the job system. **Decision (M7.0,
[ADR-0026](adr/0026-physics-core.md)): Rime builds its OWN rigid-body core — no Jolt** (VISION #1
power-first, #3 code-as-textbook; the core is shaped for destruction rather than adapted to it). The
core is the *universal simulation substrate* — destruction (M8), lighting invalidation (M10), and the
effects/fluids tracks all build on its seams. *Inspired by: Jolt (studied, not integrated); Bullet;
the sequential-impulse literature.*

*Bricks (planned 2026-07-12, own-core, bottom-up):* **M7.0 (done)** the physics-core decision
(ADR-0026: own core; the algorithm suite; substrate seams; same-binary determinism; the deferred
register); *proof:* the ADR. · **M7.1** `engine/physics` born — the seam headers, the SoA `BodyPool`
(generational ids), ECS `RigidBody`/`Collider` components + reflection, semi-implicit-Euler +
quaternion integration (no collision yet). · **M7.2** **broadphase** — dual dynamic AABB trees, fat
AABBs, parallel queries, the canonical pair list. · **M7.3** **narrowphase I** — GJK + EPA +
reference-face-clipping manifolds (feature ids, warm-start cache), sphere/capsule fast paths. ·
**M7.4** **the solver** — sequential-impulse PGS, warm starting, friction pyramid, restitution, the
NGS position pass (+`docs/math/sequential-impulse.md`). · **M7.5** **islands + sleeping + the parallel
step** on `core::JobSystem` — bit-identical world hash across thread counts, TSan-netted. · **M7.6**
**fixed-tick + ECS sync + change-detection stamps** (lands ADR-0018 §4 as public `engine/ecs` surface;
awake-only write-back). · **M7.7** **queries** — ray/overlap/shape-cast via the BVH, batched
parallel-safe variants, filters. · **M7.8** **contact/trigger/sleep events** — point + normal +
impulse, canonical per-tick order, double-buffered (the M8-damage input). · **M7.9** **shapes II** —
runtime convex hull (quickhull), polyhedral mass properties, static triangle mesh + midphase, compound
(static + dynamic). · **M7.10** **CCD** (speculative contacts) + debris-scale tuning + `WorldStats` +
the stress harness. · **M7.11** the proof — `samples/09-physics-playground` (`--headless` self-check +
`--serve`) + docs true-up. Track brick **an1** (skeletal-animation runtime: palettes on jobs + GPU
skinning) interleaves after M7.4 under the mainline-first rule.

*Status (2026-07-14):* **M7.0–M7.6 shipped**, then **M7.7 (scene queries — raycast + overlap — plus
external impulses)** and the **proof sample `samples/09-physics-playground`** land the milestone's
"done when" (objects fall/collide/stack, raycasts hit, the sim runs on the job system inside the
fixed tick; the sample self-checks headless in CI). **Reordered from the original plan to reach the
acceptance proof sooner:** the remaining planned bricks were **deferred as fast-follows** into M8's
runway, since M8 destruction is their first real consumer. Two of them have since landed:
**M7.9 — contact & sleep events** (`engine/physics/events.hpp`): began/persisted/ended contact
events carrying point + normal + impulse (the M8-damage input) plus `Slept`/`Woke` sleep events,
buffered and double-buffered in canonical per-tick order, with the event stream proven bit-identical
across worker counts (the **trigger/sensor** third of that brick is held back — no sensor-body concept
in M7's scope, no consumer yet — with its design pinned in `docs/design/physics.md`); and **M7.10 —
CCD (speculative contacts)**: a per-body opt-in flag, a velocity-swept broadphase bound, GJK-distance
speculative contacts (a negative-penetration gap), and a solver gap-bias, so a fast body is arrested
at a thin wall instead of tunnelling — no time-of-impact rewind, determinism preserved, and the stop
surfaces as an M7.9 contact event (the projectile-damage path). A third has now landed:
**M7.11 — shapes II, the convex hull** ([ADR-0027](adr/0027-convex-hull-shapes.md), the
shape-storage decision this brick wanted first): a world-owned hull store (`register_hull` →
`HullId`; `ShapeDesc` stays a flat POD), authored-and-validated geometry (no runtime quickhull —
that is the M8.1 cook), exact polyhedral mass properties diagonalized to principal axes
(`docs/math/polyhedral-mass-properties.md`), the hull support function through the unchanged
GJK/EPA path, reference-face clipping generalized from boxes to arbitrary hull faces (stable
feature ids, warm-start-cache compatible), hull raycast/overlap/CCD, and the determinism hash
proven across worker counts with hulls in the scene. Then **M7.12 — compound shapes**
([ADR-0028](adr/0028-compound-shapes.md), built on M7.11's store): one rigid body made of many
convex children at local poses, parallel-axis mass composition, and a narrowphase-expansion
multi-region contact model (per-child manifolds and per-region events — the M8 "which part was
hit" signal); the standing dumbbell is its witness. And **M7.13 — the measure-first capstone**:
`WorldStats` (a deterministic per-tick snapshot of body/collision/island counts via `stats()`,
counts not clocks so the tick stays reproducible) plus `samples/09-physics-playground --stress`, a
debris-scale load that reports peak solver load and throughput and self-checks that a 1000+-body
pile settles and hashes identically twice — the instrument that turns the remaining optimizations
from guesses into measurements. Still outstanding as fast-follows: the **static triangle mesh +
midphase** (the last shape), and the **debris-scale performance pass** the harness now measures
(its first target: the every-tick narrowphase). They remain tracked here; nothing about them is
cancelled.

**M7 non-goals (deferred, recorded in [ADR-0026](adr/0026-physics-core.md)):** joints/motors/
character controller (m12.0) · soft bodies/cloth/fluids (own modules — water is Track FL) · TGS solver
mode · implicit gyroscopic integration · dynamic mesh-vs-mesh (convex decomposition at M8.1) · scaled
colliders (v1 ignores scale) · cross-platform lockstep determinism.

**M8 — Destruction v1 (the headline begins).** `engine/destruction` — part-based
destructibles + connectivity, precomputed fracture, debris as real physics bodies,
**health-transition hooks**, and a **one-event → physics/VFX/audio fan-out**. Sample
`10-destructible-wall`. *Inspired by: Frostbite (Battlefield 6) — see
[engine-survey.md](research/engine-survey.md).*

*Bricks (planned 2026-07-15, bottom-up — refreshed against the shipped M7 substrate;
[ADR-0029](adr/0029-destruction-model.md) is the model):* **M8.0** the destruction-model ADR
(this) · **M8.1** the **fracture cook** (Rust) — seeded Voronoi → convex part hulls + bond/anchor
graph + render meshes, a new `Destructible` asset (quickhull lands here) · **M8.2** `engine/destruction`
runtime — load a pattern, register it once, stand an instance as one static compound (costs ≈ static
baseline) · **M8.3** **damage → connectivity → detach** (the hard core: contact-impulse + explicit
damage, union-find support solve, the fracture body-swap; determinism across worker counts) · **M8.4**
**health-transition fan-out** — a generic `EventChannel<T>` + a VFX dust stub + the `engine/audio` null
seam · **M8.5** **lifetime** — debris budgets over `WorldStats` + the physics `unregister_hull/compound`
· **M8.6** the proof — `samples/10-destructible-wall` (a wall fractures on impact, debris settles, one
event fires) headless-self-checking in CI. Two small **physics** seam additions are M8-owned:
`RayHit::child` (M8.3) and hull/compound `unregister` (M8.5). Proofs stay structural/headless on
lavapipe; the damage→fracture path is deterministic (the M11 replay contract). *Status: **M8
COMPLETE** — M8.0–M8.6 all landed: the model, the cook, the runtime, the fracture body-swap, the
event fan-out (`core::EventChannel` + the `engine/audio` seam + the `engine/vfx` dust stub), the
debris lifecycle/budgets over the physics `unregister_hull`/`unregister_compound`, and the
`samples/10-destructible-wall` proof — a wall fractures on impact, an island detaches, debris settles,
and one event stream fans out to dust/audio/gameplay, PBR-lit with per-part render leaves and
deterministic across physics worker counts.*

**M9 — Editor v1 (Rust).** `tools/editor` — a **client of a live engine process**
([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)): the Rust shell owns docking,
reflection-driven inspectors, and the asset browser; the **engine renders the viewport** and
delivers it over the (by then S1-hardened) streaming protocol; edits flow back as
reflection-described component data. The viewport toolkit graduates from the ICEM viewer and the
protocol is already proven by Track S, so M9 becomes assembly, not invention. Play-in-editor.
*Inspired by: Frostbite's FrostEd (editor-as-client) + Unity/UE iteration.*

**M10 — Advanced lighting (the Unreal-class push).** Each its own sub-effort + ADR
([ADR-0032](adr/0032-lighting-v2.md) sets the architecture + brick ladder):
SDF-traced DDGI GI + SSR reflections (Lumen-style), cascaded + local shadow maps
(virtual shadow maps deferred), clustered-forward many-lights (MegaLights-style),
virtualized geometry (Nanite-style, floats as m10.i). The through-line: lighting
**couples to destruction through data seams only** so *when walls fall, the light
updates*. *Inspired by: UE5.*

**M11 — Networking & networked destruction.** `engine/net` — client-server, replication,
and **prioritization + culling** of part-destruction/debris; determinism where required.
*Inspired by: Frostbite's networked destruction at 64 players.*

*Bricks (planned 2026-07-28; [ADR-0033](adr/0033-networking-v1.md) is the model — server authority,
hybrid event-replay/snapshot replication, own UDP transport — **plus its 2026-07-28 amendment
A1–A6**, a post-m11.1 ground-truth pass over the later bricks' assumptions):* **m11.0** ADR-0033 +
this ladder (the decision brick — no code) · **m11.1** **transport v2** — `platform::UdpSocket`
(POSIX-shared + Win32, non-blocking) and the `engine/net` reliability layer (frontier-anchored
ack + forward bitfield, send window, resend; reliable-ordered + unreliable-sequenced channels) over
a scripted-loss deterministic `Link` seam, with a driver-routed `process_packet` so one socket
serves N peers · **m11.2** **sessions** — `NetDriver`/`Session` (the driver that owns the Link and
routes by endpoint), schema-hash handshake, heartbeat/timeout, and the **`Application` ordered sim
stage** the net tick needs (amendment A5); two-process loopback + LAN smoke · **m11.3** **replication
core** — server-assigned `NetId`s, reflection-generated snapshot writers/readers, spawn/despawn +
ack-baseline delta replication, in its **own module above `net` and `ecs`** (amendment A10) with the
baseline ack as **replication-layer traffic**, the reliability layer having none for the snapshot
channel (amendment A9) — **preceded by the `compute_type_hash` name-folding micro-brick** (amendment
A2: identical-shape components collided; **landed 2026-07-29**, deliberately *without* the
format-version bump the plan first called for — A2's implementation note says why) · **m11.4**
**networked destruction**, split in two at build time because the bind path it needed turned it into
the largest brick in the milestone: **m11.4a** *the events half* — the server commits the canonical
**damage-op list** (including contact-derived ops; amendment A1) onto the reliable-ordered channel,
addressed by NetId through the **destructible bind path** M8.2 deferred; clients apply each batch as
it arrives and never run their own contact→damage conversion for replicated instances (amendment
A11: "the same tick" is a position in the op sequence, not a local tick index — a client is not in
lockstep), plus the **state-application seam** for late-join/drift correction (amendment A3) ·
**m11.4b** *the bodies half* — debris ride m11.3 snapshots through a debris↔entity bridge, with
composition-hash drift detection ·
**m11.5** **relevancy + budgets** — per-client interest, distance culling for debris transforms,
nearest-first priority, per-tick byte budget · **m11.6** **interpolation + input**, split in three at
build time: **m11.6a** BUILD the previous-tick transform history (amendment A4: ADR-0023's buffer is
an unbuilt seam) · **m11.6b** the alpha consume path — `ecs::RenderTransform`, the component below
both `render` and `replication`, plus expiry so a resting mirror stops replaying its last step ·
**m11.6c** the upstream half — tick-tagged client inputs as **intent rather than device events**
(amendment A19) on an unreliable channel with a redundancy window, a consumption-not-arrival
acknowledgement, and the prediction seam delivered as **state rather than an interface** (amendment
A20). Snapshot interpolation over the interval a value actually covers is deferred with the seam
named · **m11.7** the proof —
`samples/12-networked-destruction`: a dedicated headless server with a **deterministic server-side
scripted shooter** (amendment A6 — no player controller exists; that is M12 scope) + two clients
over loopback see the same wall break at meaningful scale, hash-verified in CI. Proofs stay GPU-free
and deterministic (scripted loss, never environment luck) · **m11.8** the docs true-up. Deferred
fast-follows: late-join baseline snapshots, dev-server scale run, transform quantization,
player-controller prediction, lag compensation. *Status: **M11 COMPLETE** — m11.0–m11.8 all landed:
ADR-0033 and its twenty-three amendments, the UDP transport and reliability layer, sessions, the
replication core, networked destruction in both halves, relevancy and budgets, interpolation and
upstream input, and the `samples/12-networked-destruction` proof — a dedicated server and two clients
that receive **different bytes** (640 vs 558) and still agree bit for bit on the broken wall, run
both on the deterministic scripted-loss harness (what CI gates on) and over real loopback UDP. The
milestone's most portable lesson is not about networking: the
[replication invariant](design/replication.md) — a per-peer "what they have" fact may strengthen only
on confirmed **holding** — was violated five separate times in five disguises, and every instance was
caught by a **counter**, never by reading the code.*

> **SPLIT (2026-08-27, [ADR-0036](adr/0036-milestone-split-player-and-block.md)).** What follows was
> planned as one milestone and is now two, cut at the seam ADR-0035 explicitly left for it. The text
> below is kept as written, because it is the plan the work was actually done against; read the
> bricks through this mapping:
>
> **M12 — "The Player"** is m12.0 – m12.5, all landed, plus **m12.6**, its closing proof
> (`samples/13-networked-player`) which ADR-0035's ladder did not have.
>
> **M13 — "The Block"** is everything from ADR-0035's m12.6 down, renamed: fx1 → **m13.1** ·
> block content + culling + C6 → **m13.2** · the playable client → **m13.3** · audio → **m13.4** ·
> the measured perf pass → **m13.p** · the proof `99-the-block` → **m13.5**. Its "done when" is
> M12's old one, word for word: the final demo did not move.
>
> A reference to "m12.6" in ADR-0035 or anything older means Track FX fx1, i.e. **m13.1**.

**M12/M13 — The vision demo: "The Block."** Sample `99-the-block` — destruction + dynamic
lighting + scale, together, at a playable frame rate. The thesis, demonstrated.

*Bricks (planned 2026-08-20; [ADR-0035](adr/0035-vision-demo-m12.md) is the architecture — the
falsifiable thesis, the performance-governance split, the controller/prediction design, and the
rulings on all 38 deferred items):* **m12.0** ADR-0035 + this ladder + the **hardware true-up** (the
first RTX 3060 baseline session; the first `docs/perf/` entries; the budget numbers ratified against
measurement rather than guessed — the decision brick, no engine code) · **m12.0-perf** the perf
harness + **work ledger v1**, wired into `11-lit-rooms` and `10-destructible-wall` so the instrument
is proven *before* the block exists, its proof being that a deliberately doubled draw count fails the
ledger (a gate that cannot fail is not a gate) · **m12.1** physics top-up — **`shape_cast`** (sphere/
capsule sweeps through the BVH, exposing the GJK-distance machinery CCD already uses) + **kinematic
push-in** in `PhysicsSync` · **m12.2** `engine/gameplay` — the character controller as a **pure
function over queries**, with replay determinism proven *here* so the next bricks debug
reconciliation and never the mover · **m12.3** the networked player, **server-authoritative, no
prediction yet** (`engine/gameplay_net`, the consume loop, the replicated `LastProcessedInput`,
weapon→destruction glue), whose proof records **own-input latency = RTT ticks — the number m12.4 must
beat** · **m12.4** **prediction + reconciliation** — the hardest brick in the milestone, flagged now
the way m8.3 was · **m12.5** snapshot interpolation v2 over the interval a value actually covers (the
delta header already carries the server tick) · **m12.6** Track FX brick fx1 — **fx1a** the GPU draw
pass for the existing deterministic CPU sim with three effect families, off byte-identical; **fx1b**
compute scale-up, contingent on the ledger · **m12.7** the block content + **view-frustum culling**
(none exists today) with its submitted/culled counters, riding the cook-cache schema-key fix ·
**m12.8** the playable client — **windowed present** (ADR-0023's seam; the RHI side has existed since
M3.4) + a first-person camera on the predicted player · **m12.9** audio v1 (mixer + a Linux sink) ·
**m12.p** the **measured** perf pass, its scope chosen by the ledger rather than guessed · **m12.10**
the proof — `samples/99-the-block` (scripted CI mode, `--play`, `--perf`) + the docs true-up. Cut
order if it runs long: audio → fx1b → m12.p's tail → m12.5. **Never cut:** m12.4, fx1a, the perf
gate. Track FL (water) is **out** — the "decided at M12.0" the cross-cutting note promised.

---

## Rough shape (not commitments)

Foundations **M0–M5** are the long, unglamorous climb everything depends on — they pay
back forever. **M6–M9** make it usable and show the first destruction. **M10–M12** are
the "wow." We re-plan at each boundary.

> The frost does not form all at once. Crystal by crystal. ❄

---

## Appendix: the ICEM Viewer (Frostlens) — a flagship application

Rime's first non-trivial **application**: a from-scratch 3-D viewer for the computed engineering
parts, simulation fields and flow produced by **ICEM** (a separate, deterministic
computational-engineering project). It is built *on* Rime — `samples/03-icem-viewer` links only
`engine/{core,platform,rhi}` — so it **dogfoods the engine** and pulls exactly the features Rime's own
roadmap wants next. Several RHI bricks were landed early to serve it and are adopted+extended by the M5
render graph: the **depth attachment** (ADR-0011), **push constants** (ADR-0012), **3-D/volume textures**
(ADR-0013) and **stencil** state (ADR-0014). The two repos share only *files* (STL/OBJ meshes + a native
`.icef` field binary), never code. Full plan lives outside the repo; the brick ladder, with proofs:

- **A — foundations.** A1 depth attachment (RHI) · A2 orbit camera · A3 the `.icef` field bridge (ICEM
  side). **DONE.**
- **B — surfaces + cross-section.** B1 load an STL → lit, depth-correct, orbitable part · B2 movable clip
  plane + **stencil solid cap** to look *inside* a part. **DONE.**
- **C — visualize the existing simulations.** C1 colour the part (and the cut face) by an `.icef` scalar
  field + legend · C2 GPU raymarched **isosurface + DVR** · C3 vector-field **warp** (animated
  displacement / modal mode). **DONE.**
- **D — real 3-D CFD → flow view.** ICEM grew a genuine 3-D CFD ladder (D1 inviscid potential flow, D2
  viscous Navier–Stokes); the viewer renders the computed velocity as **RK4 streamlines** coloured by
  speed (`docs/math/streamlines.md`), and — **D2·V** — derives the scalar **speed** $\lVert\mathbf u\rVert$
  so the colormap / isotach / slice / **DVR** show the viscous **boundary layer** as a volume
  (`tests/rhi/viscous_offscreen_test`). **DONE.** **D3 — compressibility:** ICEM gained a third CFD model
  (brick26, `core/sim/compressible.hpp`) — quasi-1-D isentropic **de Laval nozzle** flow recovered from the
  area–Mach relation, where ρ/T/p ride the flow, gated against `thermo::gasdyn`; the viewer colours the
  nozzle by the computed **Mach** field (0.15 subsonic inlet → green sonic throat → 2.47 supersonic exit),
  cross-sectioned. **D4 — turbojet/nozzle flow view:** a from-scratch **gas-path chart** — `--chart` / **H**
  overlays a 2-D line plot of the field along the flow axis (Mach vs station, with the dashed M = 1 sonic
  line), built on a new `ui.hpp` `line()` primitive (`docs/math/gas-path-chart.md`,
  `tests/rhi/chart_offscreen_test`). **DONE — milestone D complete.**
- **E — assemblies, from-scratch UI, provenance.** E1 multi-part **assemblies** — the ITER-class tokamak
  loads as 10 colour-tinted, number-key-toggleable parts with an axial **exploded view**
  (`tests/rhi/assembly_offscreen_test`, `docs/math/assembly.md`) — **DONE**. E2 a **from-scratch
  immediate-mode UI** on the RHI — a built-in bitmap font + panel/label/button/checkbox/slider, no Dear
  ImGui, drawn as one alpha-tested overlay pass; the assembly control panel toggles parts and scrubs the
  explode slider ([ADR-0015](adr/0015-imgui-free-ui.md), `docs/math/ui-text-layout.md`,
  `tests/rhi/ui_offscreen_test`) — **DONE**. E3 a **provenance panel** — `--provenance` reads ICEM's
  `.icejson` "why" Ledger (the DAG of which law / material / rule / safety-factor produced every value)
  and lays it out on the E2 UI as an origin-tinted, scrollable list; clicking a value expands its
  **derivation** (the values it was computed from). Shares a file format with ICEM, never code
  (`docs/math/provenance-panel.md`, `tests/rhi/provenance_offscreen_test`) — **DONE**. E4 **export polish**
  — `--turntable N` renders a full 360° orbit to a numbered PPM sequence (frame $i$ at azimuth
  $\varphi_0+2\pi i/N$) with per-frame render-throughput stats, the windowed **P** key screenshots the live
  view, and all three share one off-screen `render_view` path so the export is pixel-identical to the
  interactive frame (`turntable.hpp`, `docs/math/orbit-camera.md` §export, `tests/rhi/turntable_test`) —
  **DONE**. **Milestone E complete.**
- **F — the engine cut-away (Bview).** ICEM's showcase is a computed **geared turbofan**: `icem engine`
  emits 15 `engine_*.stl` parts plus `engine_core.icef` / `engine_bypass.icef`, each carrying a `velocity`
  and a `mach` field. This view **fuses D and E** into how engineers actually draw an engine — the
  multi-part assembly, now opened by a **meridional cut-away** clip plane through the flow axis, drawn in
  **one shared-camera, shared-depth pass** with **streamlines** traced through both ducts and coloured by
  the computed **Mach** (not raw speed), on one Mach scale shared by core and bypass. Sharing the depth
  buffer makes the remaining metal occlude the flow behind it while the opened half reveals it — a true
  cut-away. `--engine <dir>` (oriented y-up, the engine lying horizontal); the panel gains a CUT-AWAY
  toggle + Mach readout, and **C** toggles the section. No new pipeline/shader/RHI: the assembly's mesh
  clip is switched on and the Mach rides the streamline `w` channel the speed used to. A latent streamline
  bug surfaced here — a line reaching an open OUTFLOW face ran straight to `max_steps` because the sampler
  clamps at the domain edge; it is now stopped at the domain bound (`engine.hpp`,
  `docs/math/engine-view.md`, `tests/rhi/engine_offscreen_test`). **DONE.**

Each viewer brick follows Rime's conventions — a `docs/math/` derivation for the math-heavy ones, an ADR
for engine decisions, an off-screen pixel-readback proof in `tests/rhi/` that stays GPU-free in CI, and
an auto-committed+pushed focused change.
