# Qwen Estimator Verdict — m17.8b Hold-Loop Audit

**Date:** 2026-09-17
**Analyst:** qwen3.7-max (second opinion, independent of the Fable pass)
**Data:** h1/ (8 runs, order T F F T T F F T) and h2/ (6 runs, order F T T F F T, real contention)

---

## 1. Per-Arm Retained / Dropped Frame Counts

All runs: 1500 hold frames, skip=100, so 1400 candidate frames per run.

### h1 (clean box)

| Run   | Arm | Kept | Dropped | Retain% | Kept gr_med | Dropped gr_med |
|-------|-----|------|---------|---------|-------------|----------------|
| r01-T | T   | 1325 | 75      | 94.6%   | 1920        | 960            |
| r04-T | T   | 1234 | 166     | 88.1%   | 1935        | 1672           |
| r05-T | T   | 1323 | 77      | 94.5%   | 1935        | 1087           |
| r08-T | T   | 1289 | 111     | 92.1%   | 1920        | 1387           |
| r02-F | F   | 1316 | 84      | 94.0%   | 1942        | 1065           |
| r03-F | F   | 1285 | 115     | 91.8%   | 1942        | 1297           |
| r06-F | F   | 1192 | 208     | 85.1%   | 1905        | 1560           |
| r07-F | F   | 1164 | 236     | 83.1%   | 1920        | 1642           |

**h1 arm summary:** T retains med=93.3% (drops med=94); F retains med=88.5% (drops med=162). **Difference: +4.8 pp.**

### h2 (contention)

| Run   | Arm | Kept | Dropped | Retain% | Kept gr_med | Dropped gr_med |
|-------|-----|------|---------|---------|-------------|----------------|
| r02-T | T   | 1336 | 64      | 95.4%   | 1920        | 1177           |
| r03-T | T   | 1319 | 81      | 94.2%   | 1942        | 1320           |
| r06-T | T   | 1235 | 165     | 88.2%   | 1935        | 1695           |
| r01-F | F   | 1231 | 169     | 87.9%   | 1920        | 1575           |
| r04-F | F   | 1243 | 157     | 88.8%   | 1942        | 1492           |
| r05-F | F   | 1189 | 211     | 84.9%   | 1935        | 1635           |

**h2 arm summary:** T retains med=94.2% (drops med=81); F retains med=87.9% (drops med=169). **Difference: +6.3 pp.**

### Clock distributions (all clock samples, not just frame-aligned)

**h1:** T has 41.9% of samples at gr>=1900; F has 33.6%. F spends 14.3% of samples in the 210-400 MHz park band vs T's 5.9%. The GPU parks more readily under F's lighter load.

**h2:** T has 40.7% at gr>=1900; F has 36.4%. Same direction, smaller gap (contention keeps the GPU warmer in both arms).

---

## 2. Bias Verdict

### **BIASED — the selection mechanism differs by arm, but the bias in the effect estimate is small.**

The selection mechanism is clearly treatment-dependent:
- F drops **1.7x more frames** than T (h1: 162 vs 94; h2: 169 vs 81)
- The GPU parks at low clocks more readily under F's lighter load (14.3% vs 5.9% in the 210-400 band in h1)
- Dropped frames are systematically **slower** than kept frames in both arms (dropped gpu_sum is 0.05–0.41 ms higher)

This is the collider shape you described: treatment → GPU load → boost state → filter survival.

**However, the bias in the central estimate is empirically small.** Comparing filtered vs unfiltered adjacent-pair A/B medians:

| Metric       | h1 filtered A/B med | h1 unfiltered A/B med | h2 filtered A/B med | h2 unfiltered A/B med |
|--------------|---------------------|-----------------------|---------------------|-----------------------|
| gpu_sum_p50  | 0.1088              | 0.1087                | 0.1085              | 0.1078                |
| ssr_p50      | 0.1004              | 0.1009                | 0.1004              | 0.0993                |
| frame_p50    | 0.1181              | 0.1150                | 0.1150              | 0.1092                |

The gpu_sum_p50 (the most direct GPU cost measure) differs by **<0.001 ms** between filtered and unfiltered. The frame_p50 differs by ~0.003–0.006 ms. The filter is not materially distorting the estimate in this dataset.

**Why the bias is small despite the selection difference:** The per-run median is robust. With ~1400 frames per run, the ~10% of frames at low clocks are a minority that the median naturally downweights. The filter and the median converge to the same answer because the low-clock frames are a tail, not a mode.

**Uncertainty flag:** This is a property of THIS dataset (this GPU, this scene, this hold-loop length). On a GPU with more aggressive power management, or with a shorter hold (fewer frames → median less robust), the bias could be larger. I have not measured that regime.

---

## 3. Proposed Estimator

### Primary: **Unfiltered per-run median, adjacent-pair differences**

Drop the frame-level clock filter entirely. For each run:
1. Skip the first 100 hold frames (settling).
2. Compute the per-run median of the metric of interest (gpu_sum, ssr, etc.) over ALL remaining frames.
3. For each adjacent T-F pair in the series, compute the signed difference (T-F).
4. Report the median of the paired differences as the effect estimate.
5. Report the A/A adjacent-pair differences (same-arm neighbours) as the empirical null.

**Why this works:**
- The median over ~1400 frames is robust to the ~10% low-clock outliers.
- No conditioning on a treatment-affected variable → no collider bias.
- The A/A pairs give a direct noise floor without any distributional assumption.
- Simpler to explain and audit: "we took all the frames and compared neighbours."

**What to keep from the current harness:**
- The clock trace (`.clk` file) is still valuable as a **diagnostic**, not a filter. Report per-run clock summaries (fraction of samples at gr>=1700, mem>=7000) to flag runs where the GPU was in an unusual state. If a run has >50% of samples below boost, flag it for investigation but do not silently drop its frames.
- The `.cpu` trace and `foreign_max` column remain useful for detecting background contention.

**What to add:**
- A **run-level clock summary** in the output table: `pct_boosted` = fraction of clock samples with gr>=1700 AND mem>=7000. If T and F runs have very different `pct_boosted`, note it in the report but do not filter on it.
- A **sensitivity check**: report both filtered and unfiltered estimates side by side. If they agree (as they do here), the result is robust. If they diverge, investigate.

### Why not other alternatives:

- **Conditioning on something the treatment cannot affect:** There is no observable that is (a) correlated with GPU clock state and (b) unaffected by the treatment. The simulation is frozen in the hold loop, so scene complexity is constant. The only thing that changes is the material, which IS the treatment. There is no instrumental variable available here.

- **Fixed clock threshold in advance:** Same collider problem, just decided earlier. The threshold being pre-committed does not fix the selection bias; it only makes it harder to p-hack.

- **Regression with clock as covariate:** Plausible, but adds model assumptions (linearity, no interaction) for a gain of ~0.001 ms over the unfiltered median. Not worth the complexity for this measurement.

---

## 4. MDE Arithmetic

### Pooled A/A noise (h1 + h2, 5 A/A pairs total)

| Metric       | A/A n | A/A sd (ms) | MDE at k=7 pairs (ms) | Observed A/B med (ms) | k needed for 0.13 ms |
|--------------|-------|-------------|-----------------------|-----------------------|----------------------|
| gpu_sum_p50  | 5     | 0.0091      | 0.010                 | 0.109                 | 1                    |
| ssr_p50      | 5     | 0.0034      | 0.004                 | 0.100                 | 1                    |
| frame_p50    | 5     | 0.0227      | 0.024                 | 0.118                 | 1                    |
| submit_p50   | 5     | 0.0115      | 0.012                 | 0.114                 | 1                    |

Formula: MDE(k) = (z_{α/2} + z_β) × sd / √k, with z = 1.96 + 0.84 = 2.80 for α=0.05 two-sided, 80% power.

### Is 0.13 ms detectable?

**Yes, easily.** The observed effect on gpu_sum_p50 is ~0.11 ms, with A/A sd of ~0.009 ms. Even a single A/B pair gives MDE ≈ 0.024 ms (for frame_p50) or 0.010 ms (for gpu_sum_p50). With k=7 pairs available, the MDE is an order of magnitude below the target.

**However:** the observed effect is **0.11 ms, not 0.13 ms**. If the true effect is 0.13 ms, we are measuring ~85% of it. The remaining 0.02 ms could be:
- Measurement noise (the A/B range is 0.088–0.117 ms for gpu_sum_p50, so 0.13 is within the noise envelope)
- A real difference between the hold-loop cost and the in-gameplay cost (the hold loop is render-only; see §5)
- Residual bias from the filter (though §2 suggests this is <0.001 ms)

**Plain statement:** 0.13 ms is detectable on this box without pinning. The existing data already detects an effect of ~0.11 ms with 7/7 pairs same-sign and A/A sd an order of magnitude smaller. The measurement is not struggling; it is the interpretation that is hard, not the detection.

---

## 5. ADR-0041 Claim Wording

The hold loop renders the same frozen post-collapse scene repeatedly with no simulation step. It measures the GPU cost of the material **in a render-only loop**, not the cost as it reaches a gameplay frame (which includes simulation, culling variance, and frame-pacing effects that the hold loop deliberately removes).

**Proposed one-sentence ADR wording:**

> "The hold-loop protocol measures the GPU pass-time cost of the cooked BC7 ground material under render-only conditions (simulation frozen, scene static): approximately 0.11 ms per frame at 1080p on an RTX 3060, with sub-0.01 ms A/A noise over 7 paired runs. This is the material's isolated rendering cost, not its end-to-end gameplay frame impact."

**What this claim does NOT support:**
- That the cost reaches a gameplay frame (simulation is frozen; culling, LOD, and frame-pacing are not exercised)
- That the cost is the same on other GPUs (only RTX 3060 measured)
- That the cost is stable across thermal states (the hold loop is ~5s; longer sessions may thermally throttle)

---

## 6. Summary of Uncertainties

1. **The bias is real but small in this dataset.** I cannot guarantee it stays small on other GPUs or with shorter hold loops. The sensitivity check (filtered vs unfiltered side by side) is the guard.

2. **The observed effect is 0.11 ms, not 0.13 ms.** I do not know whether the true effect is 0.11 or 0.13 or somewhere between. The A/B range (0.088–0.117 for gpu_sum_p50) is consistent with both.

3. **The A/A sample is small (n=5).** The sd estimate has wide confidence intervals. If the true A/A sd is 2x the observed (plausible with n=5), the MDE doubles but is still well below 0.13 ms.

4. **h2 has only 6 runs (3 T, 3 F), not 8.** Two runs are missing. I do not know why. If they were dropped for cause (e.g., the run crashed), the h2 estimates may be survivor-biased at the run level.

5. **The hold loop is render-only.** The ADR must not claim gameplay-frame impact. See §5 for the exact wording I would use.
