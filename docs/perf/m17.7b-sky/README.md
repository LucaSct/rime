# m17.7b — what the sky costs the block's frame

The brick that gave `99-the-block` a sky (m17.7b) is the first one where the sky's lighting is
actually ON in the demo, so it is the first that can be measured at all. This is that measurement.

## How it was taken, and why not with `perf.sh`

**Interleaved A/B on one sitting, three runs per arm, order `on off on off on off`**, 240 frames
each, Release, RTX 3060. The two arms differ by ONE line — `SkyParams::enabled` in the block's
`authored_sky()` — rebuilt between runs, so the scene, the tick sequence, the camera and every
other setting are identical. `ab.sh` is the driver and `runs.txt` is its raw output.

**The clocks were not pinned, because pinning is withdrawn on this box** (2026-09-16), and the card
idles parked at 210/2100 graphics and 405/7501 memory. That is why this is an interleaved A/B on
one sitting rather than a `perf.sh --commit` baseline: adjacent runs share whatever the governor
was doing, so the *difference* survives even though neither arm's absolute number is comparable to
a run taken on another day. It also means **nothing here should be read as a baseline** — it is a
delta, and only a delta.

## The result

Regenerate with `python3 docs/perf/m17.7b-sky/pool.py`; the saved output is `pooled.txt`. These
numbers are the script's, not typed.

```
metric        on median  off median    delta  on spread  off spread  separated
render_p50        4.300       4.200   +0.100      0.050       0.030        yes
render_p99        5.050       4.680   +0.370      0.430       0.110         NO
player_p99       10.110      10.180   -0.070      0.380       0.160         NO
```

**"Separated" means the arms' medians differ by more than the wider arm's own run-to-run spread.**
It is a blunt test on purpose: on an unpinned box a delta smaller than the noise floor is not a
measurement, it is a coincidence.

### What this supports

**The sky costs about 0.10 ms of render time at p50** — 4.300 against 4.200 ms, against within-arm
spreads of 0.050 and 0.030. That is the per-pixel SH evaluation in the forward pass plus the LUT
samples on SSR's and DDGI's miss paths; the two bake dispatches are not in it, because the sky is
static here and `sky_lighting_stats()` reports **one** bake for the whole run.

### What this does NOT support, and is recorded so nobody quotes it

- **`render` p99 is not separated.** The delta is +0.370 ms and the on-arm's own spread is 0.430 —
  wider than the effect. There may be a p99 cost; this series cannot see it.
- **`frame.player` shows nothing.** −0.070 ms against spreads of 0.380 and 0.160. Expected: the
  block's frame is physics-dominated (`physics.client.step.per_frame` p99 5.734 ms against a whole
  `render` of ~4.3), so a 0.1 ms render change is far below what that number can resolve.

### Against the budget

0.10 ms of a ratified 16.600 ms frame is **0.6%**. ADR-0041 Ruling 1 is "the budget is earned
before the bar is spent", and on this evidence the sky's lighting is cheap enough that it is not
the thing standing between this demo and its budget — `sim.block` still is.

The number that will matter is m17.7e's. Aerial perspective touches every lit pixel rather than
adding one buffer read to it, and a published figure for a complete atmosphere of this shape is
under 1.5 ms on an RTX 4080. That is the brick to hold this measurement up against.
