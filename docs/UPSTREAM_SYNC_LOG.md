# Upstream Sync Log

This ledger records deliberate integrations from the upstream project. A scheduled
workflow may update the candidate report, but it must not mark code as integrated.
An entry is added only after a reviewed cherry-pick, patch application, or merge.

## Source

- Repository: `https://github.com/DeusData/codebase-memory-mcp`
- Ref: `main`
- Configuration: `.github/upstream-sync.json`
- Machine-readable state: `docs/upstream-sync-state.json`
- Candidate report: `docs/upstream-sync/latest.md`

The fork and the configured upstream currently have unrelated Git histories. Do not
use a blind merge as the synchronization policy. The current target tree is the
behavioral baseline: review the candidate report and selectively port upstream
updates, optimizations, algorithms, or features while preserving every fork-owned
capability. A protected path is never an automatic replacement candidate, and an
unprotected path still requires compatibility review.

## Entries

| UTC date | Upstream commit | Target commit | Status | Method | Scope | Checks | Review / notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
