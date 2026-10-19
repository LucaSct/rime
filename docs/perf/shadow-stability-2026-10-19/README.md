# Walking shadow stability: rejected hardware measurement

Command: `./scripts/perf.sh --sample the-block --preset release --frames 600 --commit`.
Feature tree: `9507cb2-dirty` (documentation and the preserved user courtyard were
dirty). RTX 3060, NVIDIA 615.78.08, 1920×1080, all-lighting-gates profile, 600
measured frames. The standard procedural scene is a renderer workload, not the
authored editor city's effects inventory.

The script refused to file a qualified baseline. Graphics clocks ranged from
1012 to 1935 MHz, a **50.2%** spread over their median (limit 2%); memory clocks
also varied. The existing baseline uses NVIDIA 615.71.09, so fingerprint comparison
refused. Frame p99 **21.594 ms** and simulation p99 **8.179 ms** exceed their
16.6/6 ms budgets. These raw values are observations from an unqualified run,
not comparable performance or a regression verdict.

The raw JSON and complete rejection log are retained in this subdirectory so they
cannot silently become the next baseline. No clock setting, budget, fingerprint
rule or old baseline was changed. No FPS/performance/shipment gate closes.
