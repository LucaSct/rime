# Playable environment and stable local GI — hardware measurement

Command: `scripts/perf.sh --preset release --sample lit-rooms --frames 300 --commit`.
The repository's release preset is `RelWithDebInfo`; GPU was RTX 3060 / NVIDIA 615.78.08.

The sample executed, but the performance gate **failed**: the previous fingerprint uses a
different driver and no valid regression comparison was made. Only 5 utilization-qualified
clock samples were collected; clock stability is `null`, not verified. No guard was overridden.

Raw report: [rejected hardware run](2026-10-15-11-lit-rooms-environment-optics-rejected.json).
Its filename deliberately excludes it from accepted baseline selection. The tree was dirty;
its recorded commit is the tested pre-final integration parent, not a clean final revision.

This is the generic lighting workload, with the new opt-ins at their default values. It does
not measure the playable environment's extra foliage, textures, 128-ray lattice or second scope
view, and establishes no unchanged-frame-time claim for that content. A comparable clock-stable
before/after content benchmark remains unverified.

Visual stability was measured separately at fixed world-space wall points. Those HDR deltas
are image evidence, not timing evidence; see ADR-0084 for the distinction and the local report
`/home/next/reports/rime-environment-2026-10-15/lighting-report.md` for the diagnostic controls.
