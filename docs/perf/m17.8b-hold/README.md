# m17.8b hold-loop A/B retake

The evidence behind ADR-0041's 2026-09-17 amendment ("the table above is superseded — clock
pinning is gone, the re-take needed a different method"): three interleaved 8-run T/F sessions
(`h1`, `h2`, `r2`, 24 runs total) measuring the cooked BC7 ground material's isolated GPU
pass-time cost with a render-only hold loop, simulation frozen. See
[`docs/adr/0041-the-visual-bar-m17.md`](../../adr/0041-the-visual-bar-m17.md), the "Amendment
(2026-09-17, m17.8b)" section.

## What's committed here vs what isn't

Committed: `runs.csv` (one row per run, every per-run statistic the analysis consumes) and the
scripts that produced and consume it, plus verbatim copies of the original harness tooling and its
design audit, for provenance.

**Not committed**, per ADR-0035 §2c's size intent: the raw per-frame data (`rNN-{T,F}.hold.csv`,
~500 KB each, ~13 MB total across the three sessions) and the raw `.clk`/`.cpu`/`.log`/`.meta`
sidecars. Those remain on the reference workstation at `~/rime-perf-harness-m17.8b/`. `runs.csv`
is the small derived summary the ADR's "the numbers live in the repo" rule asks for — every number
cited in the amendment is regenerable from it alone.

No `.json` files live in this directory, deliberately: `scripts/perf.sh` looks up a sample's
baseline with a flat, non-recursive glob directly in `docs/perf/` (`ls -1
"${baseline_dir}"/*-"${name}"-"${slug}".json`, `scripts/perf.sh:344`), so it would not even see a
`.json` in a subdirectory — but nothing here risks it being picked up as a baseline regardless.

## Regenerating the numbers

```
python3 pool.py
```

reads only `runs.csv` and prints three sections. The first two reproduce the original tooling
byte-for-byte: per-session A/B/A-A lists and medians as `analyze_unfiltered.py <session> 100`
prints them, then the pooled tables as `pool_sessions.py` prints them — including that script's
incidental quirk of pooling per-pair deltas already rounded to 4 decimals (it parses its own
printed text back out via subprocess). Verified byte-identical against a fresh run of the original
tooling, and against its saved `pooled-analysis.txt`. The third section recomputes the same
hold-loop pooled table at full precision, with no such rounding — the version ADR-0041's own table
cites. The two differ in exactly one cell: `ssr_p50`'s ratio, 49.1× rounded vs 48.0× full precision.

`runs.csv` was produced by:

```
python3 summarize.py ~/rime-perf-harness-m17.8b
```

(the harness directory is also the default, so `python3 summarize.py` alone works on the reference
workstation). It replicates `analyze_unfiltered.py <session> 100`'s per-run math exactly — `100` is
the settling-frame skip that call uses, dropping each run's first 100 hold-loop rows before any
statistic is taken.

## The hold loop itself

`--hold`/`--hold-out` (patched in via `patch_hold.py`) is a **scratch-only** source patch to
`samples/99-the-block/main.cpp`, applied only in a disposable `git worktree` — it was never applied
to this tree and ships nowhere. `run_hold.sh`/`run_hold_r2.sh`/`hold_series.sh`/
`hold_series_r2.sh` are the drivers that built those worktrees and ran the interleaved sessions.
`QWEN-ESTIMATOR-VERDICT.md` is the design audit that picked the unfiltered-median estimator
`analyze_unfiltered.py`/`pool.py` implement over the clock-gated one `clk.py` supports.
