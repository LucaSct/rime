# Play stability: hardware run withheld from the baseline pool

Tree measured: `a119a55`, Release preset (RelWithDebInfo), RTX 3060 / Ryzen 9950X3D,
1920x1080, 90 warmup frames and 600 measured frames, 2026-10-14.

The reflection fix makes the filtered sky chain available to SSR even with DDGI enabled.
This earns a hardware measurement, but **does not yet earn a comparable performance report**.
Two `scripts/perf.sh --preset release --sample the-block --frames 600 --width 1920
--height 1080 --commit` attempts were rejected by the script's clock-stability precondition:

| Attempt | Graphics clock | Spread | Frame p99 | Simulation p99 |
|---|---|---|---|---|
| Cold | 1170–1935 MHz | 41.6% | 15.87 ms | 6.629 ms |
| After a full warmup run | 795–1935 MHz | 73.5% | 16.00 ms | 6.882 ms |

Both failed the simulation's 6 ms gate. The GPU was also occupied by an unrelated local
`llama-server` process using about 10 GiB. That process was left alone. These observations do
not establish the rendering change's cost or a regression against the older baseline.
The script correctly declined to file either JSON into the eligible baseline pool.

Raw logs and both refused report JSONs are retained outside the checkout in
`/home/next/reports/rime-next-2026-10-14/perf-cold-refused/` and `perf-warm-refused/`, with
`perf-final.log` and `perf-warm-final.log` beside them. A new comparable run remains due when
the GPU is available and its clock stays within the existing 2% spread limit.

This scripted perf route does not enable play's streamed terrain. Its separate GPU smoke route
measures bounded resources and churn, not frame time. No hitch-free streaming or playable-frame-rate
claim follows from either test.
