# ADR-0047: Two machines, two tiers; the workstation never serves a hosted session

- Status: Accepted
- Date: 2026-09-27

## Context

[ADR-0045](0045-hosted-front-end-v1.md) made the hosted front end committed scope and
[ADR-0046](0046-exported-games-and-the-blender-boundary.md) settled the exported artifact and the
Blender boundary. Both were written against an assumption that turned out to be wrong, and this ADR
records the correction and what follows from it.

**The assumption.** The hosted design and M20's framing were written as though there were one host,
and an earlier reading of `scripts/perf.sh`'s comment about "a box with two of them" was taken to
mean a workstation with two discrete GPUs. Both were wrong.

**What is actually true, measured 2026-09-27.** There are two *machines*:

| | starbase | the workstation |
|---|---|---|
| role | Proxmox VE host, always on, hosts the estate's projects | development, intermittent |
| GPU | **GTX 1060 6GB**, driver 580.178.04 | **RTX 3060 12GB**, driver 610.57.04 |
| other adapters | — | the 9950X3D's integrated RADV device (type score 500) |
| public edge | behind **blackStar** (CT 113) on `vmbr2` | none, and none intended |

The workstation's "two of them" is one discrete GPU plus an integrated one, which the existing
`deviceType` scoring has always resolved correctly — so the two-discrete-GPU case this project
briefly designed for **exists on neither machine**. Luca's actual arrangement is "starbase for the
usual case, the workstation when the work is heavy", and that is a choice between *machines*.

Luca's words for it: *"yes it's two machines, that's the trick and difficulty."*

## Decision 1: the workstation never serves a hosted session (Luca, 2026-09-27)

`rime.peekstar.eu` resolves to blackStar and reaches **only** the starbase container. "Use the 3060
when it is heavy" means running the game or the editor **locally** on the workstation, not routing a
hosted session to it.

**What this forecloses:** 3060-tier performance is not reachable from a browser. Heavy work means
sitting at the workstation.

**What it buys, and this is the substantive part:** the development machine is never an inbound
target. It holds the owner's SSH keys, git credentials and every repository, and **none** of
`hosted-rime.md`'s containment plan exists there — so routing tenants to it would have reversed that
plan's decision 1 rather than satisfying it. It also deletes a whole subsystem before it is written:
there is no cross-host placement, no host discovery, no liveness protocol, and no policy for what
happens to a live session when the workstation goes down.

**The corollary for the gateway,** recorded because it is easy to reintroduce by accident: the
gateway is **single-host**. A session is placed on the machine the gateway runs on, and the only
"placement" decision left is admission — whether this host can take another session now.

## Decision 2: two ratified budgets, not one relaxed one (Luca, 2026-09-27)

[ADR-0035](0035-vision-demo-m12.md)'s ratified budget — `frame` p99 ≤ 16.6 ms, max ≤ 33 ms — was
measured on the RTX 3060, and the block **already misses it there**: `frame` p95 is 27.456 ms at 1080p
in `docs/perf/2026-09-22-99-the-block-nvidia-geforce-rtx-3060.json`. A GTX 1060 is roughly 2-3x
slower, so on the machine that actually hosts, that budget is not merely unmet but unreachable.

The response is **not** to relax it. ADR-0035's number was ratified against measurement and
[ADR-0041](0041-the-visual-bar-m17.md) already refused to move it when the first measurement missed;
relaxing a ratified bar because a *different, slower machine* cannot meet it would be the same
mistake wearing a hardware costume. Instead there are two bars, and each says which machine it is
about:

- **The workstation bar** is ADR-0035's existing budget, unchanged, on the RTX 3060. It remains the
  engine's ceiling and M18's gate 7 is still measured against it. Nothing here touches it.
- **The starbase bar** is new, is about the GTX 1060, and is **the one a hosted game must meet**,
  because starbase is where hosting happens.

**The starbase bar's number is not set here, deliberately.** It will be ratified from the first
clock-pinned `scripts/perf.sh` run inside the Rime container, exactly as ADR-0035 earned its
authority — measure, then ratify. Writing a number now would be inventing one, and an invented bar is
worse than an absent one because it looks like evidence. Until that run exists the starbase tier has
no bar and no hosted performance claim may be made.

Two things the first run must report alongside the timings, because they bound what the tier can ever
be: **6 GB of VRAM** (half the workstation's, against a design note in `hosted-rime.md` that already
called 12 GB with no partitioning the thing that kills the box), and whether the 1060 reports
`gpu_driven_draw`.

## Decision 3: Rime gets its own container on starbase, behind blackStar

`universe/starbase/MOVING-A-PROJECT.md` §0 says a project earns its own CT when it needs hardware or
runs a service that wants to keep running. Rime is both, so it does not go in `drydock`.

Three things follow from the estate as it already is, rather than from new design:

1. **The GPU recipe is copied, not invented.** `starbase/scripts/tabula-ct.sh:37-38` passes
   `/dev/nvidia0`, `/dev/nvidiactl`, `/dev/nvidia-uvm` and `/dev/nvidia-uvm-tools` into CT 110 and
   installs the host's **exact** NVIDIA userspace (580.178.04) with `--no-kernel-modules`. Rime's CT
   does the same. Unlike tabula it needs the **Vulkan** ICD rather than OpenCL, so the container also
   needs the Vulkan loader; the `.run` installer provides the ICD.
2. **The 1060 is shared with CT 110, and that is accepted.** A shader that hangs the GPU resets it for
   every container holding those device nodes. Tabula Zero is CPU-first with an *optional* OpenCL
   backend (`tabula-ct.sh:6-7`), so the blast radius is small and does not justify a second GPU or
   moving tabula. This is a judgement, not a measurement, and it is the first thing to revisit if
   tabula's GPU path ever becomes load-bearing.
3. **Nothing in the container listens publicly.** blackStar (CT 113) is the estate's public edge: SNI
   peek, PROXY v2 passthrough, per-address limits, backends on the internal bridge `vmbr2` at
   `10.77.0.<CT id − 100>`. `rime.peekstar.eu` becomes a blackStar route, and the gateway binds its
   `vmbr2` address. This is what makes ADR-0046's "never bind `0.0.0.0`" a property of the
   architecture instead of a rule someone has to remember — and it supersedes that note's framing of
   the DNS record as dangerous in itself: the record is fine, because the name terminates at
   blackStar and not at a machine.

## Consequences

- **`VISION.md` is deliberately NOT changed.** Luca's answer was the dual bar, which keeps the AAA
  ambition rather than re-aiming it, so there is no intent edit to make here — the ambition is the
  engine's **ceiling**, measured on the workstation. What starbase *hosts* is small games, which is
  what Luca said Rime is for, and that is a statement about the hosted tier and not about the engine's
  target. [ADR-0046](0046-exported-games-and-the-blender-boundary.md)'s "Battlefield-6-class" phrasing
  should be read that way: as the ceiling the exported-game machinery must not preclude, not as the
  bar the hosted tier is measured against. (ADRs are append-only, so that wording stands as written
  and this is the clarification rather than a rewrite.)
- `docs/ROADMAP.md`'s Track H entry gains decision 1 and 2 in this ADR's companion edit; the M20 row
  needed no change, having never named a fidelity target.
- `RIME_ADAPTER` (added for a two-GPU case that does not exist) keeps its value for a different
  reason: it makes the device a perf run landed on stateable and pinnable rather than implicit.
- The gateway needs no placement logic. Admission control is still required and is a later brick.
- `hosted-rime.md`'s VRAM analysis is written against 12 GB and must be re-read against 6 GB before
  any concurrency claim.

## What would change this

A second discrete GPU in starbase, or a decision to host from the workstation after all. The second
would need `hosted-rime.md`'s containment plan to exist on the workstation first — which is the
condition, not a formality.
