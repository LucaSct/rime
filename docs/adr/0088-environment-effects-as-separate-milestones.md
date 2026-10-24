# ADR-0088: Separate environment-effects milestones and start with seams

Status: accepted for milestone boundaries and initial interfaces, 2026-10-17.

## Context

The owner requested general smoke, water/buoyancy, fire lighting/smoke and
destruction smoke as separate milestones, with reusable code and small initial
seams. A lifetime engine product cannot make each entire physical domain one
delivery brick. Research must include working source and individual developers,
as well as papers. The current city still exceeds its collapse budget.

## Decision

Create four separately verifiable milestones: **M-FX1 General smoke**, **M-FX2
Destruction smoke**, **M-FL1 Water and buoyancy**, and **M-FX3 Fire lighting and
smoke**. These names are track identifiers, not renumberings of the existing
M0–M21 product milestones. Each has several small bricks and its own first useful
release; later fidelity improvements need not hold that release hostage.

The [milestone plan](../design/environment-effects-milestones.md) defines the
interfaces, delivery slices, research/code references and measurable exits.
Reuse the existing particle families, emission parameters, destruction event
fan-out, RHI/render graph, clustered lights and public physics impulse API.
Water is a separate numerical substrate. Do not make every domain inherit a
shared fluid solver before they have a measured common requirement.

This supersedes **ADR-0042 Ruling 2 only for research, default-off interfaces,
CPU reference calculations and their tests**, following the owner's newer
instruction. Those can begin before M18 closes. It does not relax M18's budget,
permit a new default-on GPU workload, declare the smoke renderer complete, or
waive hardware measurements. The particle/heightfield substrate split and
conditional later full-fluid investigation from ADR-0042 remain.

Gameplay forces come from canonical CPU state or server-authored transitions.
A GPU result arriving one tick late does not prove cross-device determinism.
Cosmetic smoke and lighting cannot silently become authoritative visibility,
damage or ignition. Such gameplay requires its own replicated state and tests.

## Initial work and limits

General smoke starts by making the existing `ParticleField` emission, capacity
rejection and retirement observable. Its existing `EmitParams` and
`LingeringSmoke` family are the shared emission seam; another unused emitter
hierarchy would duplicate them. This does not change additive particles into
physically lit smoke or enable a new demo pass.

Water starts with a removable core-only `rime::fluids` interface and an analytic
spherical-probe buoyancy calculation against a supplied surface sample. The
consumer applies the returned body impulse through physics. An opposite impulse
is recorded for a future water solver; recording it is not two-way simulated
water. There is no heightfield solver or moving water surface yet.

Destruction smoke starts from the existing immutable post-tick destruction
events; fire lighting starts from the existing bounded light extraction and
particle inputs. Their new adapters will live in consumers, with no reverse
dependency from destruction into VFX or VFX into rendering. The plan specifies
their missing adapters rather than inventing empty solver modules.

## Consequences and verification

Each brick has one falsifiable result, an explicit off path and counters for
deferred/dropped work. Render leaves, collision bodies and cosmetic particles
are different populations and receive separate budgets. Stable rubble remains
interactive cover; sleeping or coarser representation must preserve reactivation.

Pure calculations and emission accounting receive CPU tests, including a real
public-physics consumer and negative controls. New translation units receive
Clang checks. Enabled rendering/coupling later requires hardware reports and
network/replay proofs appropriate to its authority. A milestone completes only
when its documented exit is demonstrated, not when its interface compiles.
