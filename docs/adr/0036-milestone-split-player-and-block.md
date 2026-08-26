# ADR-0036: Splitting M12 into "The Player" (M12) and "The Block" (M13)

- Status: Accepted
- Date: 2026-08-27

## Context

[ADR-0035](0035-vision-demo-m12.md) considered exactly this split and *deferred* it rather than
rejecting it. Its own words, under Alternatives considered:

> **Split the milestone in two — "The Player" and "The Block".** Genuinely arguable, and *not*
> rejected on the merits: M12 as specified bundles two proof regimes that behave differently, one
> GPU-free and CI-gateable (move/aim/shoot, predicted and reconciled under loss) and one that only a
> single machine can judge (the block holds frame rate). A second independent review of this same
> question proposed exactly that cut. It is **deferred rather than taken** because the milestone
> table and VISION both name one final demo, and re-cutting the map is a deliberate act that wants
> its own decision rather than a side effect of this ADR.
>
> **The seam is left where the split would go.** The ladder already divides cleanly at
> **m12.5 / m12.6**.

This ADR is that deliberate act. It is written *after* m12.5 landed, which is the honest moment for
it: the seam ADR-0035 predicted turned out to be exactly where the work actually divided, and the
half above it is complete and proven. Nothing here is a forecast.

**What the evidence says at the seam.** m12.0 through m12.5 are done and every one of their claims
is a number produced by a CI-gated, GPU-free proof:

| claim | measured |
|---|---|
| own-input response, prediction on | **1 tick** |
| the same, prediction off (the control) | **6 ticks** — the round trip at 48 ms one-way |
| predicted vs. authoritative at quiescence | **bit-identical** |
| under 25% loss, 300 ticks | 124 pairings, 2 corrections, 122 within-tolerance skips |
| divergence with / without reconciliation | 0.2 m / 1.1 m |
| remote motion, frames showing no motion | **0 of 120** (v1: 79 of 120) |
| smoothing's effect on the simulation | **bit-identical trajectories** over 250 lossy ticks |

Every one of those runs on the scripted-loss harness on any CI machine. Not one of them needs a GPU,
a frame rate, or a human.

**What sits below the seam is a different kind of thing entirely.** m12.6 onward is content, scale,
a GPU draw pass, a windowed client, audio, and a frame-time distribution that — by ADR-0035 §2b's
own ruling — only one machine in the world can measure, with the honest admission that "the hardware
gate is procedural, and that is a real weakness". Its headline budget is not even ratified yet;
ADR-0035's m12.0 amendment re-timed that ratification to the block's own content.

Bundling them meant one milestone whose "done when" was two incompatible sentences.

## Decision

**M12 becomes "The Player". M13 becomes "The Block".** The cut is at ADR-0035's own seam, between
m12.5 and m12.6.

### The two milestones

| # | Milestone | Done when (the proof) |
|---|---|---|
| **M12** | **"The Player"** | a server and two clients run a predicted, reconciled player under scripted loss: own-input response ≤ 1 tick against a prediction-off control, remote motion continuous, both clients converging bit-exactly — all GPU-free and CI-gated |
| **M13** | **"The Block" (vision demo)** | a destructible urban block (M8+M10+M11+M12) runs at a playable frame rate and *feels* right |

M13's "done when" is M12's old one, verbatim. **The final demo does not move, shrink, or change.**
That is the whole character of this decision: it re-cuts the map, not the destination.

### The brick ladders, and the renumbering

M12 gains one brick it did not have — its own closing proof — because the project's rule is that
*a milestone is done only when its proof runs*, and a milestone whose proof is "several test suites"
has never happened here. Every prior milestone closed with a `samples/` artifact a human can run;
m11.7 was explicitly "the milestone proof".

- **m12.6 — "The Player" milestone proof.** `samples/13-networked-player`: a headless server and
  two clients, scripted, with `--play` deferred to M13's windowed client. Records the milestone's
  numbers as a committed artifact rather than as test output nobody re-reads.

ADR-0035's ladder entries below the seam are renamed:

| ADR-0035 called it | now | what it is |
|---|---|---|
| m12.6 | **m13.1** | Track FX brick fx1 (fx1a load-bearing, fx1b contingent) |
| m12.7 | **m13.2** | block content + view-frustum culling + ADR-0032 C6 debris visual retirement |
| m12.8 | **m13.3** | the playable client: windowed present + first-person camera |
| m12.9 | **m13.4** | audio v1 (polish, cuttable) |
| m12.p | **m13.p** | the measured perf pass, scope chosen by the ledger |
| m12.10 | **m13.5** | the proof: `samples/99-the-block` + the docs true-up |

**One label is reused and it is worth stating plainly rather than hoping nobody notices:** ADR-0035
had no "m12.6 milestone proof" — its m12.6 was Track FX fx1, which is now m13.1. Any reference to
"m12.6" in ADR-0035 or anything older means fx1. Renaming a label inside an append-only ADR is not
available, so the mapping table above is the mechanism, and it is why the table is here rather than
in a commit message.

### Everything ADR-0035 decided stays decided

This ADR moves labels. It does not reopen anything:

- §1's falsifiable clauses stand. They split with the bricks: own-input response, remote continuity
  and peer agreement are M12's and are met; the fusion running, the shot feeling connected, and
  playability are M13's.
- §2's performance governance — the work ledger on lavapipe forever, the self-gating hardware run,
  `docs/perf/` as the only home for an absolute claim — is untouched, and lands entirely in M13.
- §2's headline budget stays as proposed and stays ratified **against the block's own content**, per
  ADR-0035's m12.0 amendment. The re-timing pointed at "m12.7", which is now **m13.2**.
- §5's scope rulings stand: Track FL out, Track FX in at its true size, audio in as cuttable.
- §6's 38 deferred rulings stand. The ones that named a brick move with it.
- The **cut order** stands and is now entirely inside M13: audio → fx1b → m13.p's tail. The one
  entry that pointed *up* the ladder — snapshot interpolation, "cut last" — is moot: it landed as
  m12.5 and is in the finished half.

### VISION.md is not edited, deliberately

CLAUDE.md's rule is that *changing intent or scope* means editing VISION.md with an ADR. **This
changes neither.** VISION §5 names a destructible urban block at a playable frame rate as the thing
being built; that is still the last thing on the map, still described the same way, still M13's
"done when" word for word. What changed is how many milestones the route is drawn in. Editing VISION
for a re-labelling would be the kind of churn that makes the north star look negotiable, which is
the opposite of what it is for.

## Consequences

- **M13 is the last milestone, and M12 is no longer it.** Every "M12 is the last milestone on the
  map" sentence in the roadmap and in ADR-0035 is now historical; the roadmap says so at the point
  where it says it.
- **M12 closes with a real proof rather than by exhaustion.** m12.6 is new work created by this
  decision, and that is a cost, not a saving. It is worth paying: without it "M12 is done" would
  rest on test output, and this project has just spent three bricks learning that a green suite is
  not the same as a demonstrated claim (ADR-0035 amendments C3 and D1).
- **The M13 half now has an honest, separate risk profile.** Its gate is procedural, its budget is
  unratified, and its proof needs one specific machine. Saying that about a whole milestone is more
  useful than burying it in the second half of one.
- **Cross-references cost a lookup.** ADR-0035 names m12.6–m12.10 in a dozen places and cannot be
  rewritten. The mapping table is the price of append-only, and it is the right price.
- **Nothing shipped changes.** No code moves, no test changes, no artifact is renamed. If this
  decision were reverted tomorrow the tree would be identical.

## Alternatives considered

- **Leave M12 whole, as ADR-0035 did.** The reason it deferred rather than rejected was that the
  evidence was not in yet. It is now: the seam it predicted is exactly where the work divided, and
  the half above it closed with seven measured numbers and no GPU. Keeping one milestone would mean
  one "done when" that is two sentences, and a completed body of work with no milestone boundary to
  record it at.
- **Split, but renumber the M13 bricks as m12.6′, m12.7′ …** — keeping the old numbers with a mark.
  Rejected: a prime on a label is invisible in conversation, in a commit message, and in a branch
  name, which is where these labels actually get used.
- **Split, and also renumber M12's finished bricks** (m12.0–m12.5 → m12.1–m12.6). Rejected outright:
  those labels are in six merged commit messages, four ADR amendments, two module READMEs and the
  roadmap. Renaming shipped history to make a ladder tidy is the worst trade in this document.
- **Give M12 no closing proof and declare it done on its test suites.** Genuinely tempting — the
  suites *are* CI gates, and the rule says "a `samples/` demo **and/or** a CI gate". Rejected on
  precedent and on what a milestone is for: every one of the previous twelve ended in something a
  human could run, and the numbers in the table above currently exist only as `MESSAGE` lines that
  scroll past in CI. A milestone proof that nobody can invoke is a milestone claim on trust.
- **Make the split at m12.4/m12.5 instead**, putting interpolation and smoothing with the block.
  Rejected: m12.5 is presentation for *the player*, its proofs are GPU-free like the rest of the
  player line, and — as it turned out — it was the brick that fixed a defect the player line had
  been carrying since M11. It belongs with what it fixed.
