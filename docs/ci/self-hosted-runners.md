# Self-hosted CI runners

Two persistent GitHub Actions runners build Rime's Linux jobs on hardware we own, beside the hosted
matrix in [`ci.yml`](../../.github/workflows/ci.yml), which is unchanged and remains the gate.

## What this buys, measured

The hosted job durations on one healthy run (#279, 2026-10-07):

| hosted job | duration |
|---|---|
| sanitizer (ASan + UBSan) | **25 min** |
| build & test (windows-latest) | 23 min |
| build & test (macos-latest) | 18 min |
| build & test (ubuntu-latest) | 14 min |
| SDK consumer / Editor smoke | 8 min each |
| sanitizer (TSan, Clang) | 5 min |
| format, lint & license | 1 min |

So the critical path of a healthy run is a **Linux** job, and the two longest Linux jobs are the two
this workflow runs. That is the case for doing this at all:

- **It shortens the run, not only the feedback.** An earlier version of this document said the
  opposite — "a run ends when the hosted macOS and Windows legs end" — which was generalised from
  the starved, apt-hung runs of the evening it was written. The table above corrected it. Keeping
  the wrong claim visible here is deliberate: the measurement is the point, not the prose.
- **Warm beats fast.** Each runner keeps its Conan cache, ccache, cargo target directory and build
  tree between runs, and installs no toolchain per job. This is how a 2012 i7 stays useful.
- **It takes the Linux jobs out of the hosted queue.** On 2026-10-06 a 24-run cascade starved the
  hosted macOS pool for six hours, until GitHub killed two jobs at the max job duration while they
  were still *queued*. On 2026-10-07 GitHub's apt mirrors hung three Linux jobs in
  `Install Linux deps` for over three hours across three separate runners. A warm self-hosted runner
  does not run apt at all.
- **It unlocks what hosted runners cannot do** — a real GPU for render proofs, perf measurement on
  fixed hardware (ADR-0047's starbase tier), caches larger than the hosted disk. Not wired up yet;
  the runners are the prerequisite.

**TSan stays hosted on purpose.** It is 5 minutes, and it is the only **Clang** build in the matrix —
the reason `CLAUDE.md` asks for a `clang++ -fsyntax-only` pass on every new translation unit. Moving
it would swap CI's pinned Clang for whatever each runner happens to have (18 on Ubuntu 24.04, 22 on
Arch), trading a stable signal for five minutes.

**Each job checks out into its own subdirectory** (`ci-build/`, `ci-asan/`), because
`scripts/build.sh` layers `--sanitizer address` onto the *same* `build/dev` directory as a plain
build — its own comment at `scripts/build.sh:89-102` records that the directory remembers what it
was last told. On a hosted runner that is harmless; on a persistent one, a shared tree would make
the two jobs reconfigure and invalidate each other every run, destroying the warm cache the setup
exists for.

## The runners

| name | machine | labels | resources | service |
|---|---|---|---|---|
| `starbase` | CT 123 `rime-ci` on the Proxmox host (i7-3770K, 2012, 20 GB) | `self-hosted, Linux, X64, rime-linux, lavapipe, starbase` | container 6 cores (builds at `-j4`), **8 GB + 2 GB swap**, 120 GB, `cpuunits 50`; runner service `MemoryMax=7G`, `OOMPolicy=continue`, `Nice=10`, `IOSchedulingClass=idle` | `actions.runner.*.service` inside the container |
| `nextos` | the workstation (Ryzen 9 9950X3D) | `self-hosted, Linux, X64, rime-linux, lavapipe, nextos, `**`bigmem`** | `CPUQuota=1600%` (16 of 32 threads), `MemoryMax=12G`, `Nice=10`, `IOSchedulingClass=idle` | `actions.runner.*.service`, account `ghrunner`, home `/var/lib/github-runner` |

Two kinds of label, and the distinction is what makes the scheduling legible:

- **Capability** labels are what the workflow selects on. `rime-linux` means "has the Linux
  toolchain"; `bigmem` means "can hold a working set that does not fit in starbase's container"; and
  `lavapipe` records that both runners pin the software rasterizer.
- **Machine** labels — `starbase`, `nextos` — exist for pinning a job to one box when diagnosing,
  through the workflow's `runner_label` dispatch input.

Builds run at `-j4` on starbase and `-j16` on the workstation
(`CMAKE_BUILD_PARALLEL_LEVEL`/`CARGO_BUILD_JOBS` in each service's drop-in). The starbase number is
lower than its core count on purpose: a heavy C++20 translation unit can take 1–2 GB in g++, and
`-j6` against the container's 8 GB invites the OOM killer, which surfaces as a flaky *compiler error*
rather than as "out of memory".

**Both runners pin lavapipe** (`VK_DRIVER_FILES` to the `lvp_icd` ICD) even though both machines have
real GPUs. Rime's render proofs are structural with margins **taken against lavapipe**; letting the
loader choose would mean a green run measured a different device depending on which machine happened
to pick up the job. A real-GPU job is a deliberate, separately-labelled thing to add later.

### starbase took its time to earn the shared label

It is worth recording that the shared `rime-linux` label was once **removed** from starbase, because
one render proof disagreed only there:
`tests/render/virtual_geometry_resolve_pass_test.cpp` compared the forward path's albedo against the
visibility-buffer resolve path's, and on starbase 2 of 2164 covered pixels differed by up to 73,
deterministically, while the UVs agreed to 5.96e-07 and the gradients to 1.18e-05.

That was **not** a machine fault and not a tolerance to widen. Lanes returning early from the resolve
shader left their texture gradients undefined, and llvmpipe derives one texture LOD per 8-lane SIMD
group — so on a CPU without AVX2 a live pixel took its LOD from a dead lane. The shader now has every
lane take part in the `textureGrad` and discard the sample (#283), and #288 measured the cost and
recommended changing nothing further. Mesa/LLVM version, vector width and a mip-boundary theory were
each tested and ruled out en route.

The label was restored once the fix landed. The lesson that generalises: **a runner that has only
ever run one job shape is not provisioned, it is merely untested** — starbase's first `build & test`
also failed for a missing `libssl-dev` that no earlier job had needed.

### Why ASan is the one job pinned to `bigmem`

`rime_render_tests` under ASan is a **single process**, and its anonymous working set grows past
**6.87 GiB** — ASan's redzones and allocator quarantine over lavapipe's framebuffers, which are host
memory because lavapipe *is* the CPU. The starbase container is capped at 8 GB, so the job does not
fit there.

It is worth being precise about what the evidence showed, because the obvious reading of it is wrong.
Three cgroup OOM kills on 2026-10-08 named `anon-rss` 5.22, 5.25 and 5.29 GiB against a
`MemoryMax=5500M` (5.37 GiB) cap, which looks like "the cap is a little too low". Raising it to 7 GiB
produced a fourth kill at **6.87 GiB**. The process grows until it meets the ceiling, so an
`anon-rss` figure from an OOM report tells you where the cap was, **not** what the job needs.

Giving it room on starbase would mean overriding the rule
`universe/starbase/scripts/rime-ci-runner.sh` exists to state — CI must always lose to the service
containers on that host. Pinning the job to a machine that fits honours that rule instead.

Speed says the same thing: **10.2 min on nextos** (run 37809196770) against **26.3 min on starbase**
before it died. So the label costs a queue wait on one machine and buys both a 2.6× faster job and a
job that finishes at all. starbase keeps the other five Linux jobs, all of which run there in under
five minutes.

A `workflow_dispatch` pin still overrides the label, deliberately: pinning ASan to starbase is how
the ceiling gets re-measured, and it should keep failing there until the ceiling changes.

**`OOMPolicy=continue` is the other half, and it is the part worth copying.** systemd's default is
`OOMPolicy=stop`, which kills the whole *service* when any process in its cgroup is OOM-killed. That
turned one over-budget test into `##[error]The runner has received a shutdown signal` attributed to
`UNKNOWN STEP` — indistinguishable from a human cancelling, a superseded run, or GitHub's 6-hour job
limit, and it would have taken any other job on that runner with it. With `continue`, the same event
reads `99% tests passed, 1 tests failed out of 82` under the step that owns it. A resource cap should
surface as a failing test, not as a dead runner.

### Logs

- starbase: `sb 'pct exec 123 -- journalctl -u actions.runner.* -n 100'`; provisioning logs are
  `/root/universe-maint/rime-ci-{ct,toolchain}.log` on the host, and every step appends one line to
  `/root/universe-maint/CHANGELOG`.
- workstation: `journalctl -u actions.runner.* -n 100`.
- Per-job logs also live in `_diag/` inside each runner's home, which is what GitHub support asks for.

## Scheduling, honestly

GitHub does **not** do any of the things one might hope:

- **No preference between matching runners.** Labels express eligibility, not priority. A job goes to
  whichever matching runner is idle; there is no way to say "prefer the fast one".
- **No fallback to hosted runners.** If a job names `self-hosted` labels and no matching runner is
  online, it queues — for up to 24 hours — and then fails. It will not quietly run on GitHub's
  hardware instead.
- **No migration of a running job.** If a runner disappears mid-job (the workstation sleeps, loses
  network, or is shut down), that job fails or hangs until it times out. The work is lost.

**The retry procedure** is therefore the whole operational story, and it is short:

```bash
# which runners does the repo see, and are they online?
gh api repos/LucaSct/rime/actions/runners -q '.runners[]|"\(.name) \(.status) busy=\(.busy)"'

# rerun just the failed jobs of a run (only once the whole run has finished — GitHub refuses
# while any job of that run is still queued)
gh run rerun <run-id> --failed
```

The workstation runner has **no systemd inhibitor**, deliberately: CI must never block a shutdown or
a suspend. The cost is the bullet above — a job interrupted that way needs a rerun. On resume the
runner's long poll re-establishes itself; `Restart=always` plus `network-online.target` cover boot
and network recovery.

To take the workstation out of the pool on purpose (its jobs then queue to starbase):

```bash
sudo systemctl stop  'actions.runner.*'   # pause
sudo systemctl start 'actions.runner.*'   # resume
```

## Security

`LucaSct/rime` is a **public** repository, and that is the whole reason this section exists.

- **There is no `pull_request` trigger on the self-hosted workflow.** On a `pull_request` event
  GitHub runs the workflow file *from the pull request's head*, so a fork can add a job that targets
  `self-hosted`; an `if:` guard inside the workflow cannot prevent that, because the fork controls
  the guard too. Pull requests stay on hosted runners in `ci.yml`.
- **Fork-PR approval is set to `all_external_contributors`** (was `first_time_contributors`), so no
  outside contributor's workflow runs anywhere without an explicit approval click.
- **`permissions: {}`** at workflow level, `contents: read` on the one job; `persist-credentials:
  false` on checkout, so no token is left in `.git/config` on a machine that keeps its disk.
- **No secrets are available to these jobs**, and neither runner account can read the interactive
  user's home, the SSH keys or any host administration credential. No privileged container, no Docker
  socket.
- **`github.actor == github.repository_owner`** on the push path is defence in depth, not the
  control. Write access is execution trust: anyone who can push to `main` or `feat/**` can run code
  on these machines, by design.

**The residual risk, stated rather than papered over:** if an external pull request were approved by
hand, its workflow could target the self-hosted runners. The protection is the approval click.

**The stronger design, if this repo ever takes outside contributions:** move the runners to a private
`rime-ci` repository and have a hosted bridge job in `rime` dispatch the exact SHA to it. Then no
workflow file under a contributor's control can reach the runners at all. It costs a second
repository, a cross-repo dispatch credential and checks that no longer appear natively on the commit
— which is why it is written down here instead of built today.

## Caches, disk, cleanup

The caches are the point of the machines, so they deliberately live **outside** the workspace and
survive every run: `~/.conan2`, `~/.ccache` (20 GB on the workstation, 15 GB on starbase),
`~/.cargo`, and the `build/dev` tree in the workspace itself.

The workflow fails early if the runner has **under 15 GB free**, because a full disk otherwise
surfaces as a baffling Conan or compiler error. When that fires:

```bash
ccache -C                      # drop the compiler cache
conan cache clean "*" --source --build --download    # drop Conan's intermediates
rm -rf _work/rime/rime/build   # force one cold rebuild
```

A periodic cold rebuild is healthy regardless: it is the only thing that catches a build that only
succeeds because of a stale artefact.

## Updates

- **The runner updates itself.** The service is installed with updates enabled, which is the default;
  GitHub pushes a new version and the runner replaces its own binaries between jobs. Pinned version
  at install time: **2.338.0**, SHA-256 verified at download.
- **The toolchain does not.** Re-run the provisioning script to move it
  (`universe/starbase/scripts/rime-ci-toolchain.sh`), which installs the versions `ci.yml` pins
  rather than versions written down in a second place. That is deliberate: a toolchain disagreement
  between CI and a second machine costs a day before anyone suspects the toolchain.

## Provisioning (repeatable)

In the `universe` estate repository, following its conventions (`sb 'bash -s' < script`, a
`systemd-run` unit so an SSH drop cannot kill a long install, a log, and a hard guard against
touching an existing container):

| script | what it does |
|---|---|
| `starbase/scripts/rime-ci-ct.sh` | creates CT 123 `rime-ci`: unprivileged, no nesting, no GPU, no host mounts, firewall in = SSH from the workstation only, out = DNS/NTP/80/443 only, plus the hookscript that disables IPv6 |
| `starbase/scripts/rime-ci-toolchain.sh` | installs CI's exact Linux package set, the `runner` account, rust-stable + rustfmt/clippy, Conan 2 in a venv |
| `starbase/scripts/rime-ci-runner.sh` | installs and registers the runner; reads the token from a 0600 file and shreds it |
| `workstation/github-runner-install.sh` | the same three steps for the workstation, as a `ghrunner` system account with the resource limits above |

Two traps worth keeping written down, both found the hard way here:

- **IPv6 must be off in the container.** The Ubuntu mirrors publish AAAA records; a container with
  `ip6=manual` and no IPv6 route makes `apt-get update` **hang** rather than fail. An unprivileged
  container cannot set the sysctl itself, so the host does it in the container's netns after every
  start — the mechanism CT 121 already used, which this setup had to rediscover.
- **`rustup --component` takes one value per occurrence.** `--component rustfmt clippy` makes rustup
  treat `clippy` as a positional argument and exit non-zero, failing the install halfway.

## Removal

```bash
# deregister (invalidates the runner's credentials server-side), then delete the machine
gh api -X POST repos/LucaSct/rime/actions/runners/remove-token -q .token   # mint a removal token
#   starbase:  sb 'pct exec 123 -- su - runner -c "cd ~/actions-runner && ./svc.sh uninstall && ./config.sh remove --token <TOKEN>"'
#   then       sb 'pct stop 123 && pct destroy 123'
#   workstation: cd /var/lib/github-runner && sudo ./svc.sh uninstall && sudo -u ghrunner ./config.sh remove --token <TOKEN>
#   then         sudo userdel -r ghrunner
```

Deleting `.github/workflows/ci-self-hosted.yml` is enough to stop using them; `ci.yml` never
referenced them.

## Windows and macOS

- **Windows 11** is the leg that actually gates a run's wall clock, and it is the next thing worth
  building: a KVM guest on the workstation (UEFI + emulated TPM 2.0, which Win11 requires), on an
  external drive the owner is attaching, because the internal disk has ~81 GB free and the guest
  wants most of it. The starbase host is not a candidate: an Ivy Bridge CPU is not a supported
  Windows 11 platform, and its RAM is committed to 15 service containers. **Blocked on:** the drive,
  an installation ISO, and a licence key.
- **macOS has no path on this hardware.** Apple's licence permits macOS only on Apple hardware, so
  there is nothing to configure and no workaround to write down. The one Apple machine on the LAN is
  too old for a current macOS, which also rules out current iPhone/arm64 toolchains. **Blocked on:**
  Apple Silicon hardware (a purchase). Note the distinction that matters for planning: an Intel Mac
  could *cross-compile* arm64 binaries, but running Apple Silicon **tests** needs Apple Silicon.
  Until then `macos-latest` stays hosted, and that is the right answer rather than a gap.
