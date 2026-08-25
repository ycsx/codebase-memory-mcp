# Upstream Synchronization

The fork tracks useful changes from `DeusData/codebase-memory-mcp` without
automatically overwriting fork-owned features. The current repository is the
behavioral baseline: upstream changes are inputs to selectively transplant, not a
replacement tree. Synchronization has four separate states:

1. **Detected**: the scheduled or manual audit fetched a newer upstream commit.
2. **Candidate**: `docs/upstream-sync/latest.md` lists commits and changed paths
   for review.
3. **Integrated**: a maintainer selectively applies a commit or patch and records
   the source and target commit in `docs/UPSTREAM_SYNC_LOG.md`.
4. **Rejected**: a candidate was reviewed and intentionally not applied; record
   the reason when it is important for future audits.

## Automatic audit

`.github/workflows/upstream-sync.yml` runs weekly and can also be started manually.
It fetches the configured upstream, refreshes the candidate report and opens or
updates a report-only pull request. It does not merge upstream code and it does
not mark a commit as integrated. The report separates protected local feature
paths from other candidate paths, but a shared file is never considered safe for
automatic replacement.

The workflow has no dependency on the local `upstream` remote. It uses the HTTPS
URL in `.github/upstream-sync.json`, so it can run with the repository's normal
GitHub token and does not require an SSH key.

## Local commands

From the repository root:

```bash
python3 scripts/upstream_sync.py status --fetch
python3 scripts/upstream_sync.py report --fetch --write-state
```

After reviewing a candidate and applying the selected upstream change:

```bash
python3 scripts/upstream_sync.py record \
  --source-commit <upstream-full-sha> \
  --target-commit <local-full-sha> \
  --status integrated \
  --method cherry-pick \
  --scope "src/pipeline, tests" \
  --checks "scripts/lint.sh --ci; ctest --test-dir build" \
  --notes "Reason this upstream change is compatible with the fork"
```

`record` validates both commits, updates `docs/upstream-sync-state.json`, and
appends one immutable row to `docs/UPSTREAM_SYNC_LOG.md`. Integrated records must
include the checks that were run, and the command refuses to silently overwrite
an existing record for the same source and target pair.

## Integration contract

- Keep the target tree's existing behavior and product features as the baseline.
- Never use a whole-tree merge, rebase, checkout, or overwrite from upstream.
- For a file changed on both sides, port compatible symbols or hunks manually and
  resolve conflicts in favor of the local feature contract.
- For an upstream-only file or isolated algorithm, copy it only after checking its
  dependencies, public API, build behavior, and tests in this fork.
- Run the relevant lint, unit/integration, UI, and release checks before recording
  the integration. Use `partial` when only part of an upstream commit was ported.
- A path not listed as protected is only a candidate, not permission to replace it;
  the maintainer still decides which upstream behavior is compatible.

## Review rules

- Keep `graph-ui/`, `desktop/`, `src/mcp/`, `src/ui/`, cross-project pipeline code,
  and tests under explicit review. These paths are listed as protected in the
  configuration because the fork has product behavior there.
- Prefer a small, single-purpose upstream commit or a path-limited patch.
- Run the relevant lint, tests, UI build and release checks after applying a change.
- Record the full 40-character upstream SHA, not only a tag or short SHA.
- If the histories remain unrelated, treat `git diff` and patch review as the
  source of truth; do not infer a safe merge base.
