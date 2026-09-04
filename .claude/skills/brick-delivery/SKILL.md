---
name: brick-delivery
description: How to land a brick in Rime — the branch-to-merge pipeline, the perf-run requirement, driving PRs through the GitHub REST API, running clang-format over CI's exact file set, and the stacked-PR rebase that every child needs the moment its parent squash-merges. Use when pushing, opening a PR, retargeting a stack, or landing a series of stacked PRs.
---

# Brick delivery

Per-brick: own branch → build → lavapipe green → small commits → push → PR → CI (3-OS +
format + ASan/UBSan + TSan) → merge.

**A perf-touching brick also commits a `docs/perf/` run** (`scripts/perf.sh --commit`,
Release, on hardware — ADR-0035 §2c): the regression gate is only as good as the freshness
of what it compares against, and putting the report in the diff is what makes an absent
measurement visible in review rather than merely absent.

## Pushing and PRs

Stacked PRs are **pre-retargeted to `main` before parents merge**:

```bash
gh api repos/LucaSct/rime/pulls/<N> -X PATCH -f base=main
```

**Drive PRs through the REST API** rather than `gh pr edit`/`gh pr checks`: those wrappers
need `read:org`, and whether you have it depends on how this machine authenticated — an
interactive `gh auth login` grants it, a bare `GH_TOKEN`/PAT generally does not. The REST
path works under either, so it is the one to reach for by default. `gh auth status` prints
the scopes you actually hold.

**Run clang-format before pushing** — it lives on the dev server at
`~/.rime-tools/bin/clang-format` (v20.1.8, the exact pinned CI version). Mirror CI's exact
file set — `find`, **not** `git ls-files` (which skips *untracked* new files and so
silently misses a brand-new source file, the trap that red-CI'd M6.8):

```bash
~/.rime-tools/bin/clang-format -i $(find engine tests \( -name '*.cpp' -o -name '*.hpp' -o -name '*.mm' \))
```

CI's format job is the backstop, not the first line of defence — skipping the local run
cost M6.3, M6.4 and M6.8 a red-CI round-trip each.

## Landing a stack: every child conflicts the moment its parent merges

We merge to `main` by **squash**, so `main` ends up holding the parent's *squashed*
equivalent while each child branch still carries the parent's *original* commits. Git's
three-way merge then falls back to the pre-stack merge base, sees both sides rewriting the
same regions differently, and reports a genuine conflict — with no rebase or force-push
having happened. `mergeable: true` across the whole stack before you start is therefore
worthless: it is only ever true of the *next* PR. Landing #114–#120 hit this five times in
a row. Per child, after each parent merges:

```bash
git rebase --onto origin/main <old-parent-tip> <child-branch>
git diff <child-original-tip> HEAD          # MUST be empty — see below
git push --force-with-lease origin <child-branch>
```

That `git diff` is the load-bearing step, not a sanity check. `main`'s tree after the
squash is byte-identical to the old parent tip's tree, so replaying only the child's own
commits is *provably* tree-preserving — and an empty diff **carries the child's existing
green CI onto the new SHA**, because an identical tree cannot test differently. Check it;
never assume it. Also pass an explicit `commit_message` when merging: the repo's
`squash_merge_commit_message` is `COMMIT_MESSAGES`, which on a stacked PR concatenates all
10–17 commits, parents included.

One consequence worth expecting: `concurrency: ci-${{ github.ref }}` with
`cancel-in-progress` means each merge's `main` run is cancelled by the next merge, so a
7-PR stack leaves six cancelled `main` runs and one completed. The tree-identity argument
above is then the only thing standing behind those intermediate commits.
