---
name: release
description: Create a release proposal for the v5.x branch by cherry-picking commits from main
disable-model-invocation: true
argument-hint: "[version (optional, auto-determined from PR labels)]"
---

Create a v5.x release proposal. If a version is provided as $ARGUMENTS, use it. Otherwise, determine it automatically (see step 2).

## Prerequisites

The `branch-diff` tool must be installed globally:

```
npm install branch-diff -g
```

Fetch and fast-forward **both** branches before doing anything else. Comparing a
stale `v5.x` against a stale `main` silently produces a wrong commit list:

```
git fetch origin && git checkout v5.x && git pull && git checkout main && git pull
```

## Steps

### 1. Identify commits to cherry-pick

Use the `branch-diff` tool to list commits on `main` not yet applied to `v5.x`:

```
branch-diff v5.x main
```

Its GitHub issue-lookup errors go to stderr; the commit list is on stdout. PR numbers
appear in the trailing URL (`.../pull/393`), *not* as `(#393)` — parsing the `(#NNN)`
form instead picks up PR references that happen to appear in commit titles.

`branch-diff` matches commits, not content, so it reports a substantial number of
**false positives** — commits whose changes are already on `v5.x`. Do not cherry-pick
these. They fall into three classes:

**a. Squash-merged releases.** Releases 5.14.2, 5.14.3 and 5.14.4 were squash-merged
rather than rebased, so every commit they contained lost its identity and is reported
forever. This set is closed and will not grow — treat all of these as already released:

| Release | Proposal | PRs subsumed |
|---|---|---|
| 5.14.2 | #331 | 284, 310, 311, 315, 316, 317, 320, 323, 324, 325, 326, 327, 329 |
| 5.14.3 | #334 | 328, 332 |
| 5.14.4 | #337 | 333, 335, 336 |

**b. Superseded dependency bumps.** A Dependabot bump that never landed on `v5.x`, which
later picked up an equal-or-newer version of the same package directly. Cherry-picking one
would *downgrade* the branch. Recognise these by comparing the package version in
`v5.x:package.json` against the bump's target — skip when `v5.x` is at or ahead of it.
(Examples seen so far: #140, #344, #348, #349, #350.)

**c. The `main`-only version bump.** #154 moved `main` to `6.0.0-pre`. It must never be
cherry-picked onto a 5.x release branch.

Anything left after removing those three classes is a genuine candidate. Note that being
old is *not* by itself evidence of a false positive: #352 sat below all of these and was a
real, unapplied commit. Classify by the rules above, not by age.

Confirm the list of commits with the user before proceeding.

### 2. Determine the version number

If the user didn't provide a version, determine it from PR labels. For each commit being cherry-picked, extract the PR number from the commit message (e.g. `(#305)`) and check its labels:

```
gh pr view <number> --json labels --jq '.labels[].name'
```

- If any PR has a `semver-minor` label, the release is a **minor** bump.
- If all PRs have at most a `semver-patch` label, the release is a **patch** bump.

Get the current version from the tip of `v5.x` (the most recent version commit message), then compute the next version accordingly. Confirm the version with the user.

In the steps below, `$VERSION` refers to the determined version number.

### 3. Create a worktree

Create a git worktree from the current repo, checking out a new branch `v$VERSION-proposal` based on the `v5.x` branch:

```
git worktree add ../pprof-nodejs-v5 -b v$VERSION-proposal v5.x
```

The path is usually still occupied by the previous release's worktree. Once that
proposal's PR is merged, it is safe to clear — verify it is clean and merged first, then:

```
git worktree remove ../pprof-nodejs-v5 && git branch -D v<previous>-proposal
```

All subsequent steps run in the worktree directory.

### 4. Cherry-pick commits

Cherry-pick the agreed-upon commits in chronological order (oldest first):

```
git cherry-pick <hash1> <hash2> ...
```

If a cherry-pick has conflicts, stop and resolve with the user.

### 5. Verify the selection against `main`

Before bumping the version, diff the worktree against `main`:

```
git diff --stat main -- .
```

The goal is **minimal divergence**: ideally this reports nothing but `package.json` and
`package-lock.json` (the version, plus any dev-dep bump this release includes).

This is the check that validates step 1, and it is worth doing carefully — it is how #352
was caught, a genuinely unapplied commit that a plausible-looking age heuristic had
written off as a false positive. Any *other* file appearing here means one of two things:

- a real commit was wrongly classified as a false positive — cherry-pick it, or
- the divergence is deliberate — say so explicitly in the PR body rather than leaving it
  silently unexplained.

Note that a class-(b) superseded bump correctly shows up as a `package.json` /
`package-lock.json` difference where `v5.x` is *ahead* of `main`. That is expected and
should be left alone.

### 6. Create the version bump commit

Bump the version in package.json and package-lock.json using npm, then commit:

```
npm version $VERSION --no-git-tag-version
git add package.json package-lock.json
git commit -m "v$VERSION"
```

Keep this commit last on the branch. If a further cherry-pick turns out to be needed after
this point, drop the version commit (`git reset --hard HEAD~1`), apply the cherry-pick,
then re-run the bump — rather than stacking the new commit on top of the release commit.

### 7. Push and create a PR

Push the branch and create a PR targeting `v5.x`:

```
git push -u origin v$VERSION-proposal
```

Create the PR with `gh pr create --base v5.x`. The PR body should categorize the cherry-picked PRs by type, following this pattern:

```markdown
# New features
* #NNN

# Improvements
* #NNN

# Bug fixes
* #NNN

# Other (build, dev)
* #NNN
```

Only include sections that have entries. Reference PR numbers from the original commit messages.

See https://github.com/DataDog/pprof-nodejs/pull/295 for an example.
