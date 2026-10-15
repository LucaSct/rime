# Runtime arms and optical lens: hardware run, comparison refused

Command: `scripts/perf.sh --preset dev --sample lit-rooms --frames 600 --commit`.
The worktree's `build/dev` was configured as **RelWithDebInfo**, sanitizer off; the
directory name does not indicate this binary's optimization level.

Measured on the RTX 3060, NVIDIA **615.78.08**, at 1920×1080. Raw frame p50/p99:
**1.918 / 2.391 ms** over 600 frames. These are observations, not a regression verdict:

- The existing baseline uses driver **615.71.09**. The fingerprint comparison refused it.
- Loaded graphics clocks varied **1837–1935 MHz (5.3%)**, exceeding the 2% gate.
- `perf.sh` correctly refused to file its JSON as a valid baseline. No override was used.
- Noninteractive clock pinning was unavailable (`sudo` requires a password); no GPU
  clock or persistence setting was changed.

The rejected raw report and log are retained locally under
`/home/next/reports/rime-grip-lock-2026-10-15/`. A stable run on the current driver and
an explicit new baseline are still needed for a hardware regression comparison.
This run exercises the shared rigid draw path; it does **not** measure sniper-mode
frame time. Selecting the sniper adds a separate 512×512 scene render by design.
