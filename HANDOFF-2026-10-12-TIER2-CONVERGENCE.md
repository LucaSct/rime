# Handoff — Tier2 solver convergence, 2026-10-12

Stopped at Luca's request: “write a handoff and finish and clean up”. No further agent work is running. Claude was conserved throughout this diagnostic continuation; Codex made the experimental edits, Astra consulted through the read-only Codex subscription, and paid devpass inference spend was $0.

## Starting point and shipped work

Main source baseline is `d47e1bf67fa1e414826514d104153dc6bb9f4551`, PR #316 (constraint velocity locals), already merged with all eight checks green. Its retained evidence is `/home/next/reports/rime-locals-review-2026-10-12/`; fixed-workload minimum velocity cost improved 1.148× with identical endpoint hashes. Hardware frame-time improvement remains unmeasured: the prior hardware run failed clock-stability checks.

This continuation investigated the **unshipped** Tier2 symmetric world inverse-inertia matrix experiment. It changes floating-point rounding and therefore collapse behavior. No experimental engine/sample changes were merged, pushed, or proposed in a PR. This handoff is the only new repository change.

## Measured diagnosis

Both headless runs use the default scripted tick window, current-main sources, and identically cooked assets. Release C++ build and all ten cook fixtures completed successfully.

| Witness | Baseline quaternion path | Experimental matrix path |
|---|---:|---:|
| Settling ticks | 69 | 600, bound exhausted |
| Final tick | 499 | 1030 |
| Server committed / accepted operations | 2391 / 2391 | 3819 / 3819 |
| Client applied operations | 2391 | 3816 |
| Refused operations / checks | 0 / 0 | 0 / 0 |
| Unmapped operations / malformed messages | 0 / 0 | 0 / 0 |
| Composition matches / mismatches | 822 / 0 | 2286 / 0 |
| Unverified composition checks | 6 | 37 |

Baseline exits 0 with 29/29 claims. Matrix exits 1 on hash agreement and the unchanged settling bound. During its 600 empty-input ticks, committed operations grow from 1058 to 3819: **empty player input is not authoritative quiescence**. At tick 1030 only health differs: one part each on NetIds `8:1`, `15:1`, and `42:1`. Their alive bits and debris composition agree.

A separate causal control repeats the original failure, then sets every server destruction instance to `Authority::Remote` at the exhausted boundary. This legally stops new local/contact damage; physics, replication and the lossy network continue. **Four ticks later all 3819 operations are applied, shared-state hashes agree exactly, and the server's hash is unchanged from the freeze.** The original `settled_after == 600` remains unchanged, so this control still exits 1 on the original bound. It is not a passing production gate or a proposal to freeze gameplay.

Conclusion bounded to this reproducible workload: the observed convergence failure comes from ongoing authoritative damage plus delivery latency, not permanent operation loss or divergent debris composition. This does not establish acceptable collapse behavior, matrix accuracy across all shapes, or performance against the newly shipped locals path.

## Astra consultation and remaining fault

Astra independently identified the same two competing explanations, emphasizing that transport reliability begins only after admission. `DestructionServer::publish` abandons rejected reliable sends without retries or counters in current main. A partially accepted multipart tick can subsequently be abandoned by the receiver. This contradicts the repository's every-drop-gets-a-counter rule and merits a separate bounded brick, **but measured refusal counts are zero in these runs**, so it does not explain this failure.

Astra also challenged the documentation's blanket treatment of all 29 headless claims as unsuitable gates: collapse thresholds are rounding-sensitive, but missing authoritative facts still require investigation. This diagnosis supports that distinction. No recommendation was adopted without local verification; no queue policy or assertion was changed.

Full consultation: `astra-spec.md`, `astra.out`, `astra.err` in the evidence directory below. Fable was not needed or called.

## Preserved artifacts and reproduction

Evidence directory: `/home/next/reports/rime-tier2-convergence-2026-10-12/`.

- `diagnostic-and-matrix.patch`: all five experimental source diffs against `d47e1bf`; full source snapshots under `experimental-source/`.
- `historical-tier2.patch`: recovered previous-session matrix experiment. The current patch adapts the #316 local impulse lambda too; merely applying the historical patch would leave that hot path using quaternions. Matrix scratch is world-owned rather than the historical thread-local vector.
- `baseline-build.log`, `cook.log`, `trace-build.log`, `matrix-build.log`, `freeze-build.log`: successful final build evidence. The initial historical patch failed to apply at moved world.cpp context; its failed build was corrected before any matrix run.
- `baseline-headless.log`, `matrix-headless.log`, `matrix-freeze-control.log`: complete diagnostic outputs. `state.json` records base, branch and changed paths.
- `baseline_the_block`, `baseline_trace_the_block`, `matrix_trace_the_block`, `matrix_freeze_the_block`, baseline/matrix `solver_bench` binaries, and `cooked/`: retained for replay after worktree removal.

Replay from the evidence directory (the cooked override is necessary after cleanup):

```sh
RIME_CONVERGENCE_TRACE=1 ./baseline_trace_the_block --headless --cooked "$PWD/cooked"
RIME_CONVERGENCE_TRACE=1 ./matrix_trace_the_block --headless --cooked "$PWD/cooked"
RIME_CONVERGENCE_TRACE=1 RIME_CONVERGENCE_FREEZE=1 ./matrix_freeze_the_block --headless --cooked "$PWD/cooked"
```

To resume edits, create a fresh isolated worktree from `d47e1bf` and apply the archived patch. It deliberately enables the matrix unconditionally **inside that experimental checkout only**; it is not ready to ship. Diagnostics and formatting are exploratory; no full suite, sanitizer or cross-platform checks were claimed for them.

A five-pair benchmark was attempted but its harness failed to parse stdout; no completed A/B measurement or new speedup is claimed. `--json-only` still emits a `solver_bench_json: ` prefix. A parser probe subsequently ran baseline successfully; stdout/stderr are retained as `bench-parser-failure.*`. Next time parse the labeled JSON line, preserve raw output and check subprocess exit status before using it. Historical Tier2 numbers predate #316 and must not be presented as current improvement.

## Next bounded work, if resumed

1. Make the headless diagnostic truthful: its current “peers agreed after 600 quiet ticks” message also prints on failure. Separate delivery convergence against a frozen authoritative snapshot from gameplay settling, without weakening the existing bound or claiming empty input stops contact damage. Decide the proof design before implementation.
2. Reprice matrix vs `d47e1bf` with five interleaved fixed-workload pairs, min/median, and explicit changed hashes. Validate physics invariants and sequential/parallel determinism before proposing the rounding-changing optimization. The owner's collapse-behavior choice remains open; keep production defaults unchanged.
3. Address refused-send observability and bounded recovery as a separate replication brick. Astra suggested per-session bounded FIFO retaining whole batches/checks, retrying before newer batches even on empty-op ticks, with explicit disconnect/resynchronization on exhaustion. This is a design suggestion, not an approved implementation. Prove forced mid-multipart rejection, resumed delivery and overflow behavior; never silently merge fracture boundaries.

## Budget and cleanup

Last fresh Claude reading: personal 95% of the five-hour window, reset **2026-10-12 21:59 Europe/Berlin**; personal week 14%. Code week 100%, reset **2026-10-16 23:00 Europe/Berlin**. Recheck `claude-usage-both` before any future launch; its recommendation compares five-hour windows only and can suggest the exhausted code week. Other sessions share these accounts. Use the real Claude binary with the explicit personal config directory when budget is available.

Experimental source and binaries were archived before removing this session's `.claude/worktrees/tier2-peer-convergence` worktree. No unrelated scratch files, other sessions, or their processes were touched. There is no unfinished PR from this diagnostic task. Coordination was closed in `/home/next/projects/coordination.txt`.
