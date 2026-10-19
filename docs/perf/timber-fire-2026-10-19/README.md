# Timber/fire follow-up: rejected hardware run

At `18a1fbe`, ran the existing Release hardware command:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  scripts/perf.sh --preset release --sample the-block \
  --frames 600 --width 1280 --height 720 --commit
```

Exit **1**, no accepted baseline. Graphics clocks varied 975–1882 MHz (49.4%
spread, 2% limit). Frame p99 was 22.272 ms against 16.600 ms; sim.block p99 was
8.777 ms against 6.000 ms. Baseline fingerprint mismatch prevented regression
comparison. These observations from an unqualified run do not establish
performance or a regression. The script refused filing; its staged JSON and
[complete output](rejected-hardware.log) are retained explicitly as rejected.

The default benchmark had zero authored sources/surfaces and no phase atlas.
It does not measure the new basin geometry or dynamic fire workload. Those have
functional windowed checks, not performance qualification. The dirty suffix
includes the preserved local courtyard experiment. No baseline, gate or GPU
setting was changed. Existing collapse/performance gates remain open.
