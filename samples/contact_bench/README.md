# Frozen contact replay

`contact_bench` freezes the narrowphase inputs that a changing collapse cannot
hold still. It stores runtime hull geometry, float bits, canonical pair identity,
poses, velocities, timestep, CCD eligibility and the exact contact result before
warm starting. The reference replay must reproduce every saved result before a
candidate is evaluated. Candidate verification also compares the entire
exact/speculative result field by field; checksums only summarize that proof.

Build with the ordinary release preset:

```sh
cmake --build build/release --target contact_bench rime_physics_tests -j4
build/release/bin/contact_bench --input /path/to/city.contacts
build/release/bin/contact_bench --input /path/to/city.contacts --candidate child
```

Default execution verifies only. `--time` explicitly requests 21 whole-corpus
repeats after verification; it includes dispatch, bound computation and semantic
checksumming, excludes parsing, and never advances physics. Reported percentiles
describe whole-corpus repetitions, not individual-query latency. The experimental
`tight` candidate is a sensitivity control, and fails the committed city corpus.
`child` computes the compound midphase's existing 1 mm margin plus one outward ULP
per pair. `cached` uses the same private per-build bounds cache as the production
runtime. `none` always calls the unfiltered private narrowphase. The CLI validates
both exact and speculative results against that reference before timing.

For Linux CPU measurements, use the guarded ABBA runner while builds, tests and
other benchmarks are absent:

```sh
python3 samples/contact_bench/measure.py --binary build/release/bin/contact_bench \
  --input /path/to/city.contacts --candidate cached --out /path/to/report/cpu.json
```

It pins its child to one allowed CPU, records clocks, CPU/cache/governor, binary
and input SHA-256, refuses a foreign process above 50% CPU or aggregate foreign
CPU above 80%, and stops only its own child if new
competition appears. Missing clocks or run-median clock drift above 5% refuses
comparison. This is a CPU-only narrowphase measurement, never the hardware
frame-budget gate or a claim about multiplayer capacity.

## Capture actual city inputs

The preparation script generates a replacement `world.cpp` and links it into the
existing external city diagnostic. It changes no production files and never
starts a game. Source anchors must match exactly. Commands, headers, archives,
generated source and binary hashes are retained in the output provenance.

```sh
python3 samples/contact_bench/prepare_capture.py --build-dir build/release \
  --out-dir /home/next/reports/contact-capture --compile
mkdir -p /home/next/reports/contact-capture/run
RIME_CONTACT_CAPTURE=/home/next/reports/contact-capture/city.contacts \
RIME_CONTACT_HASHES=/home/next/reports/contact-capture/run/hashes.csv \
RIME_CONTACT_STRIDE=20 \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json RIME_REQUIRE_VULKAN=1 \
  /home/next/reports/contact-capture/capture_city \
  --out-dir /home/next/reports/contact-capture/run --city --hip --multi-collapse \
  --warmup 60 --frames 360 --physics-phases --contact-profile
```

Recording is a functional data-collection run. Its instrumentation, locking and
file I/O invalidate timing claims. Hash output works independently of recording,
so recording on/off and a candidate can be compared over every physics step,
including startup/warmup. Compare `(world, step)` keys, never append order.

The default records every twentieth contact build per named world, with a
500,000-row cap. The footer counts every build/query, selected builds/queries,
written rows and cap drops. Any drop, inconsistent count, missing footer,
unsupported type, nonfinite input, invalid topology, duplicate pair or trailing
junk refuses the whole corpus. World labels must uniquely identify live worlds.
This captures plain convex dispatch only; compound expansion and terrain remain
outside this corpus. CCD is replayed after a rejected current-overlap test.

An external cached-filter world requires a passing oracle first:

```sh
python3 samples/contact_bench/prepare_capture.py --build-dir build/release \
  --out-dir /home/next/reports/contact-candidate --compile \
  --candidate child --oracle /path/to/city.contacts
```

This compiles the actual production source with its cache. The default `none`
preparation defines `RIME_PHYSICS_UNFILTERED_CONTACTS` only for its replacement
world object, yielding an unfiltered reference without a runtime toggle. A
passing sampled corpus must still be followed by the full per-step hash comparison.

The text container is version 1 and diagnostic-only. Float components are decimal
uint32 bit patterns, so parsing changes neither signed zero nor precision and
does not depend on C++ object layout. Dictionary hulls are already in their
runtime centre-of-mass frame; do not re-register them. Cross-compiler rounding
differences can correctly refuse replay. Use a matching compiler/configuration
when judging an otherwise bit-identical optimization.

The committed capture and results are in
[`docs/perf/contact-oracle-2026-10-18`](../../docs/perf/contact-oracle-2026-10-18/README.md).
