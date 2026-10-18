# Environment effects: small bricks inside separate milestones

Decision: [ADR-0088](../adr/0088-environment-effects-as-separate-milestones.md),
2026-10-17. This starts interfaces and numerical witnesses, not four finished
simulators. Existing M18 performance and multiplayer-shipment work remain open.

| milestone | first useful release | shared code and existing seams |
|---|---|---|
| M-FX1 General smoke | an independently scripted, lit smoke plume with bounded lifetime and cost | `vfx::ParticleField`, `EmitParams`, `LingeringSmoke`, FX input buffers and render graph |
| M-FX2 Destruction smoke | dust/smoke at actual breaks and collapses, with meaningful zero-impulse events | immutable destruction events, M-FX1 emission/rendering, stable source/tick identity |
| M-FL1 Water/buoyancy | a bounded basin with visible waves and a buoy that disturbs the water | core math, public physics impulse seam, surface samples, later render graph surface |
| M-FX3 Fire lighting/smoke | an ignition source visibly drives smoke and bounded reflected light, then extinguishes | M-FX1 smoke, clustered point lights, canonical ignition snapshots and event fan-out |
| M-FL2 Temperature-driven freezing/melting | cooling freezes water and heating melts it, with measured energy/phase accounting | M-FL1 cell/surface identity, a separate thermal CPU oracle, later water transport and visible ice |

Dependencies are small: general smoke supplies the visual substrate for destruction
and fire; water's CPU reference and coupling interface are independent. Initial
seams can progress now. Enabling new frame workloads still needs budget evidence.
The first complete visual slices should be general smoke, its destruction
consumer, a bounded water/buoyancy basin, then fire lighting/smoke. Compute scale-up
is justified by observed cap drops, not by the existence of a GPU.

## Reuse and authority contracts

* **Emission:** keep `EmitParams`, bounds, intensity and the caller's canonical
  tick/source ordering. Ambient plumes, collapse events and burning sources feed
  one particle substrate. Counts reconcile attempted requests, spawns, capacity
  drops, natural retirement and explicit clearing. A future GPU implementation
  must expose equivalent accounting; it may have a different internal layout.
* **Destruction:** read immutable events after the authoritative tick. The
  consumer translates event kind/bounds into emission; destruction never calls
  VFX. Deduplication keys include event kind, stable source and tick. A zero
  `IslandDetached` magnitude is meaningful and receives a positive visual
  baseline. Debris eviction is not another explosion.
* **Water:** `WaterSurfaceSample` carries height, flow, density and source tick.
  `BuoyancyProbe` carries a stable caller-owned body key, position, velocity,
  radius and drag. `BuoyancyExchange` returns displaced volume and impulses with
  the same key/tick. The consumer validates tick policy and applies impulses
  in canonical order before the corresponding physics step. Multiple overlapping
  probes double-count volume; authoring must prevent that in the first version.
* **Fire:** a future canonical ignition snapshot supplies stable source/tick,
  bounds and burn intensity. Its consumer emits smoke through the same substrate
  and appends bounded light rows through ordinary scene extraction. Fire spread,
  fuel consumption, damage and gameplay visibility are separate state, never
  reverse-engineered from rendered light or stochastic particle density.

No new code reaches inside a Vulkan backend, destruction's implementation or the
physics solver. VFX and fluids depend only on core; consumers know both sides of
an adapter. Existing destruction/lighting interfaces are retained instead of
duplicating their types in an unused generic simulation framework.

## M-FX1 — General smoke

1. **Emission accounting seam (initial slice):** add bounded-request and lifecycle
   counters to the existing CPU pool. Prove family mixing, overflow conservation,
   invalid-input rejection and unchanged scatter for valid call sequences. Reset
   accounting without resetting live particles or the random stream.
2. **Independent plume consumer:** a scripted source emits without damage or
   destruction. Map the existing particle data into the existing draw input.
   Off declares no new pass; on has a visible sensitivity control. Reconcile
   emission and draw caps, and report source, simulation and upload costs.
3. **Smoke shading:** implement lit attenuation/coverage and a documented
   transparency policy. The current additive pass is a flash renderer, not a
   satisfactory smoke renderer. Test smoke against dark/bright backgrounds and
   a moving local light; fading must not leave TAA ghosts.
4. **Measured capacity:** capture near/far and overlapping plumes, memory,
   p50/p99 CPU/GPU time and draw/drop counts. Move simulation to compute only if
   the CPU cap or cost binds. Keep CPU reference inputs for comparison.

Exit: a visible reusable plume, a clean disabled path, bounded lifetime/memory,
observable saturation and a committed hardware report. Dense volumes, pressure
projection and combustion are later optional fidelity milestones, not hidden
requirements for this first useful smoke release.

## M-FX2 — Destruction smoke

1. **Event adapter:** translate `PartDied` into impact dust and `IslandDetached`
   into lingering smoke with a nonzero baseline. Consume each canonical event
   once. Test zero-impulse collapse and duplicate delivery with distinguishable
   event identities. Do not touch fracture acceptance or solve order.
2. **Spatial budgets and lifetime:** cap emission per source, tick and visible
   region; every rejection has a counter. Use bounds/size to guide the plume,
   rather than emit one expensive effect per visual fragment.
3. **Playable proof:** a single building then three buildings produce readable
   smoke and eventually clear. Compare effects off/on with identical damage,
   body counts, support state and replicated inputs. Measure smoke separately
   from the already expensive rigid-body collapse.
4. **Network relevance:** visuals consume confirmed source events without
   pretending a rendered puff proves network convergence. Join/relevance churn
   must not replay a historical explosion storm.

Exit: visible smoke tied to real destruction, no new gameplay/physics divergence,
no per-fragment emission explosion, bounded persistence and a hardware cost report.

## M-FL1 — Water and buoyancy

1. **Surface/impulse seam and analytic oracle (initial slice):** independently
   implement spherical-cap submerged volume and `rho*g*V`. Prove dry/half/full
   cases, relative-flow drag direction and key/tick preservation. Apply returned
   impulses through `PhysicsWorld::apply_impulse`: a half-submerged sphere of
   matching density stays afloat while the dry control falls. The water reaction
   is only a record until a water solver consumes it.
2. **Bounded CPU basin:** implement conservative height/depth and horizontal
   momentum transport on a regular grid. Specify CFL/substep limits, boundary
   and wet/dry policy before coding. Test still water on uneven bed, dam break,
   positivity and mass accounting over 10,000 steps. Include deliberately leaky
   and force-disabled controls. Select explicit tolerances from the oracle and
   numeric precision before recording results; do not tune them to a failing run.
3. **Two-way coupling:** gather stable body/probe snapshots, integrate canonical
   impulses, then feed solid displacement/momentum into the grid. Test drop,
   bobbing, settling and interaction with other bodies. Distinguish displaced
   surface feedback from full momentum conservation in the report.
4. **Visible water:** a separate surface consumer samples the validated field.
   Reuse water materials/reflections, not the pond's static geometry as solver
   proof. Demonstrate surface/body alignment, shoreline behavior and bounded cost.
5. **Authority and optional GPU path:** CPU canonical state or server-authored
   impulses drive gameplay. A cosmetic GPU surface may use that state. Generic
   staging/readback and failure handling need their own lifetime proof;
   `wait_and_borrow` alone is not a water readback protocol. GPU-derived forces
   require cross-device agreement evidence. Test loss, late join, delayed samples
   and replay; hashing destruction composition alone does not prove trajectories.

Exit: a visible bounded basin and genuine two-way disturbance with mass/coupling
witnesses, canonical authority, replay and hardware cost reports. Pouring into a
room, overturning waves, FLIP/APIC and full 3D SPH/PBF remain later research.

## M-FX3 — Fire lighting and smoke

1. **Ignition-to-presentation adapter:** consume a bounded ignition snapshot or
   scripted source and map its burn curve into smoke emission and a local light.
   The input is a seam for gameplay, not a claim of chemical combustion.
2. **Light and smoke integration:** reuse clustered-light extraction, shared
   smoke data and existing HDR/reflection paths. Bound source count, light range,
   probe invalidation and shadow slots. Rejected light rows have counters.
3. **Lifecycle and authority:** prove ignite, sustain, extinguish and source
   removal without a stuck light or orphaned plume. If ignition affects damage,
   author it on the server and replay it; cosmetic flicker must not change damage.
4. **Playable/performance proof:** expose a source near reflective and matte
   surfaces, verify light response and smoke shading, and measure overlap with
   destruction. No unmeasured dense 3D field enters the default frame.

Exit: a controllable ignition source visibly drives bounded smoke and lighting,
extinguishes cleanly, and passes network/cost gates. Heat transport, fuel/oxygen,
spread and volumetric combustion are distinct later increments, each with a
physical witness rather than merely a convincing picture.

## Research and concrete code to study

The [water research](../research/2026-10-17-water-buoyancy.md) and
[smoke/fire research](../research/2026-10-17-smoke-fire.md) record direct papers,
developer posts, actual source files and license findings. In particular:

* Chentanez/Müller's heightfield paper informs two-way coupling; Matthias Müller's
  compact heightfield/FLIP demos show working stepping code. The heightfield
  file's reuse permission was not established; study it without copying it.
* Boychev's MIT shallow-water solver provides inspectable conservative fluxes.
  Stephen Thompson's HLSL implementation and post explain a more robust wet/dry
  scheme; its GPL code is a design reference, not an Apache import.
* NVIDIA Flow 2 supplies actual sparse advection and pressure shaders. MantaFlow
  supplies working smoke/fire scenes. Both are later solver references, not
  dependencies for a first particle plume. Check licenses per file/version.
* Wronski's production volumetric-fog work informs the rendering seam. NanoVDB
  informs sparse volume representation; it is not an evolving smoke solver.

Initial Rime code is independently written. No third-party source is imported.
Document licenses/notices, platform fit and measured benefit before any import.
An old foundational paper can still be useful; current available code and
production tradeoffs are checked separately instead of calling every old method
the latest technique.

## Next-brick progress, 2026-10-17

The initial seams now have working, default-off successors:

* **M-FX1:** bounded continuous `PlumeEmitter`; a sorted alpha smoke mode in the
  existing FX pass; a copied SceneRenderer input with capped, observable drops.
  RGB approximates caller-supplied incident light. There are no soft intersections,
  self-shadowing, density grid or temporal smoke motion vectors yet.
* **M-FX2:** a sample-owned immutable-event consumer deduplicates source identities
  per tick, emits for zero-impulse detachments, and caps bursts and the shared pool.
  Live peer/relevance churn and a collapse smoke hardware ledger remain open.
* **M-FL1:** a bounded flat-bed conservative CPU basin uses shared Rusanov fluxes,
  reflective ghost states, explicit CFL substeps and atomic refusal. Tests cover
  10,000 steps of mass conservation, dry shores and surface sample ticks. Dry cells
  return no sample, preventing phantom lift. This is numerically diffusive reference
  water, with no bathymetry, visible dynamic surface or two-way displacement yet.
* **M-FX3:** a bounded source consumes monotonic ignition snapshots, drives shared
  smoke and exposes one light contribution. The demo source is scripted cosmetic
  fire, not combustion, fuel consumption, spreading or damage.

`the_block --play --effects-preview` enables the ambient plume, scripted warm light
and smoke, and destruction adapter together. Ordinary play leaves these off.
`--effects-smoke` freezes simulation while toggling presentation and measures a
visible delta against a drift control. The static pond still represents authored
reflective art; these CPU basin tests do not turn it into simulated water.

## Environment and editor sequence, 2026-10-18

The owner requested temperature-driven freezing/melting first for ice. It is a
new milestone, not a claim of existing collision, fracture or thermal simulation.
[ADR-0090](../adr/0090-environment-source-authoring.md) records the authoring seam.

1. **Source authoring (current implementation):** reflected smoke/fire components
   use the existing typed inspector, undo/redo and `.rscene` writer. The editor
   host previews a bounded shared pool and transient fire lights during Play/Step,
   freezes time during Pause and clears presentation on Stop. Defaults remain off.
   Edit/inverse, fresh-world save/load and lifecycle tests exercise the C++ seams;
   a clicked GUI proof and enabled-scene hardware cost are still required. See
   [`vfx_scene`](../../engine/vfx_scene/README.md) for usage and actual limits.
2. **Ice energy/phase oracle (next CPU brick):** select documented material
   parameters and implement a bounded water/ice sample whose stored energy gives
   temperature and liquid fraction. Cooling/heating must account for latent heat,
   rather than flip a Boolean at a threshold. Prove cooling → freeze → colder ice
   and heating → melt → warmer water; zero heat is unchanged, opposite equal heat
   restores energy, phase fraction stays in range, and malformed/nonfinite input
   refuses atomically. A deliberately latent-heat-disabled control must fail the
   phase-duration/energy witness. No transport, geometry or rigid-body forces yet.
3. **Water two-way coupling:** consume solid displacement and compatible momentum
   through public body/probe and basin interfaces. Prove mass accounting, drop,
   bobbing and settling. The current flat-bed basin has no vertical momentum: do
   not claim full momentum conservation by merely recording the reaction impulse.
4. **Water/thermal authoring and visible surface:** add reflected bounded-basin
   settings, probes, initial temperature and heat-source descriptions, with invalid
   combinations refused and reported. Preview from the validated numerical state;
   save descriptions rather than solver buffers. Show moving water, floating body,
   cooling/freezing and heating/melting with surface/body alignment. Coupling ice
   to moving water first needs explicit liquid/solid mass and transported-energy
   accounting; a painted white surface is not proof. Solid collision/thickness
   changes and breakable sheets are subsequent bricks with their own authority.
5. **Smoke and fire fidelity:** soft smoke intersections, lighting/temporal
   response, destruction relevance/late-join lifecycle and collapse cost; then
   canonical ignition/extinguish controls. Fuel, heat transport and spread each
   need their own state and witnesses before they affect damage or ice.
6. **Complete the creator pipeline:** clicked editor undo/save/reopen/Play proof,
   export the authored scene through the existing validated game bundle, and run
   an exported server plus two clients with late join and compatibility refusal.
   Preserve the separate Blender-authored M16 material/asset proof. Components
   appearing in an inspector do not complete that asset or shipment pipeline.

Each item is itself split at a reviewable numerical, authoring or visual result.
M18 remains open; CPU references and opt-in authoring can progress, while new
default-on rendering and gameplay-coupled water/ice retain budget/network gates.

## M-FL2 reference progress, 2026-10-18

The first thermal CPU brick is implemented as `fluids::ThermalSample`. It stores
energy for a fixed mass and derives temperature and liquid mass fraction through
solid warming/cooling, the latent-heat plateau and liquid warming/cooling. Both
initial temperature and phase are required. Heat and power/time updates validate
before committing, expose rounding accounting, and refuse unsupported arithmetic
or out-of-range states without clipping energy. It creates no frame workload.

The [research and independent Fable/Astra consultation findings](../research/2026-10-18-water-ice-phase.md)
and [ADR-0091](../adr/0091-fixed-mass-thermal-phase-reference.md) describe coefficients,
scope, adopted and rejected advice. Tests include a no-latent-heat counterfactual,
changed latent heat versus changed capacities, sub-ulp input, signed zero, strict
range admission and atomic failure. The built library is also checked by exact
Python fractions; Python is not a runtime dependency.

The next **thermal** brick is conservative heat exchange between fixed-mass
samples: warm/cold exchange with an equal-and-opposite energy ledger, equilibrium
and an exchange-disabled control, followed by a bounded spatial thermal field. The
next **water** brick remains solid displacement/body feedback. Joining the two
requires liquid/solid mass and transported energy accounting before a moving
basin can freeze. Temperature/heat-source editor authoring and visible phase
remain separate work; there is no rendered or collidable ice in this reference.
