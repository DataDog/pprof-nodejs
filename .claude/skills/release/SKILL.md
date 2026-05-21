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

## Steps

### 1. Identify commits to cherry-pick

Use the `branch-diff` tool to list commits on `main` not yet applied to `v5.x`:

```
branch-diff v5.x main
```

Review the output with the user. Skip:
- Version bump commits (e.g. "Bump package version on to 6.0.0-pre")
- Commits that would result in empty cherry-picks (already applied or superseded)

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

All subsequent steps run in the worktree directory.

### 4. Cherry-pick commits

Cherry-pick the agreed-upon commits in chronological order (oldest first):

```
git cherry-pick <hash1> <hash2> ...
```

If a cherry-pick has conflicts, stop and resolve with the user.

### 5. Create the version bump commit

Bump the version in package.json and package-lock.json using npm, then commit:

```
npm version $VERSION --no-git-tag-version
git add package.json package-lock.json
git commit -m "v$VERSION"
```

### 6. Push and create a PR

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
