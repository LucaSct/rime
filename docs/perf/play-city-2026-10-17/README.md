# Native city play capture

This is an external, bounded diagnostic of the live `run_play` path. It generates a copied main source and a wrapper, records 451 rendered frames in a native window, then exits. The existing app, sample, and engine sources are not edited. The run uses 30 warmup frames, 120 hip frames, 30 aim settling frames, 120 ADS frames, 30 turn settling frames, 120 turning frames, and one final frame. Frame intervals are attributed to the preceding render. Owner-thread zones are `frame.pump`, `sim.tick`, `frame.acquire`, `frame.declare`, `frame.execute`, and `frame.present`.

From this checkout, with an existing Ninja build that already contains the required engine and sample objects:

```sh
ROOT="$PWD"
BUILD=/home/next/projects/rime/build/release
OUT=/home/next/reports/rime-city-perf-2026-10-17/city
python3 docs/perf/play-city-2026-10-17/prepare.py --root "$ROOT" --build-dir "$BUILD" --out-dir "$OUT" --compile
"$OUT/capture_play"
python3 docs/perf/play-city-2026-10-17/analyze.py --root "$ROOT" --build-dir "$BUILD" --out-dir "$OUT"
```

For the eight-building city control, use a distinct output directory and append `--disable-city` to `prepare.py`. This changes exactly the generated `Demo::start` city assignment to `false`; the streamed world, building support, environment, viewmodel, and presentation path remain enabled. The normal generated source modifies the `run_play` body and adds the capture header include. The control's assignment is the one explicit edit outside that body.

`prepare.py` only compiles and links when `--compile` is passed. It never launches the window. It derives compilation from the `samples/99-the-block/main.cpp` entry in `compile_commands.json` and linking from the last `ninja -t commands bin/the_block` command, preserving its other leaf objects. Its output includes the original source, instrumented source, wrapper, header, exact command arrays, logs, hashes, both checkout Git states, and the actual CMake build type. The build directory may be absolute and separate from the source checkout. It must be compatible with the requested source; this harness does not rebuild engine libraries or sample support objects.

`analyze.py` reads `capture.frames.csv` by default, or `--csv PATH`. It rejects absent, truncated, noncontiguous, nonfinite, nonpositive interval, wrong phase, changing extent, or short steady-phase captures. It writes `capture.summary.json` and `capture.intervals.csv`. The summary is descriptive and explicitly reports `baseline_proven: false` and no perf-gate pass. It records observed shots and charge; it does not script firing or destruction. Display refresh, compositor, driver, scene assets, source/build compatibility, and run-to-run variance affect the result. A single run cannot establish a performance regression or pass a gate.
