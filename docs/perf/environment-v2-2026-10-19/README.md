# Environment V2: rejected hardware measurement

This receipt is **not an accepted baseline or performance qualification**.
The unchanged performance gates remain open. `scripts/perf.sh` exited 1 and
refused its normal `--commit` filing; the staged JSON is retained under
`rejected-hardware/` so reviewers can inspect the failure.

Command, at implementation commit `36bdf07`:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  scripts/perf.sh --preset release --sample the-block \
  --frames 600 --width 1280 --height 720 --commit
```

Measured on the RTX 3060 / NVIDIA 615.78.08:

- Graphics clocks ranged from 210 to 1912 MHz, a 92.7% spread of the
  1837 MHz median across 116 loaded samples; the stability limit is 2%.
- Frame p99 was 21.611 ms against 16.600 ms, and sim.block p99 was
  8.010 ms against 6.000 ms. These are failed observations from an unqualified
  run, not comparable performance estimates.
- The existing baseline used another driver and 1920×1080. The fingerprint
  mismatch also prevented regression comparison; no re-baseline was authorized
  or performed.

The default benchmark had no enabled authored environment descriptions
(`atlas=0`, no flames/smoke/phase surfaces). It does not measure the cost of the
V2 showcase. The `dirty` suffix includes the pre-existing local courtyard edit,
which was preserved. No gate, GPU power/clock setting or baseline was changed.
See [the complete output](rejected-hardware.log) and
[functional verification](../../verification/environment-v2-2026-10-19/README.md).
