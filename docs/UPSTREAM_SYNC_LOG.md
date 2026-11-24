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

The November ports landed in `be8aa9799`. Windows x64 validation covered
full and incremental indexing, including Unicode paths. The two reviews give
the fixtures, test counts and platform limits.
[November 6 review](upstream-sync/2026-11-06-review.md) · [November 24 review](upstream-sync/2026-11-24-review.md)

## Entries

| UTC date | Upstream commit | Target commit | Status | Method | Scope | Checks | Review / notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-24 | `808100974cf89324bafdf03a3278fa83bd697ac1` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | Java own-package type priority | Windows CLANG64 C suites, CLI smoke and CI lint | Retains fork-specific external receiver modeling |
| 2026-11-24 | `9a7d53b47b342e1ec0936a29a0780e6f11691c84` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Ungrouped Cypher aggregates over empty input | Windows CLANG64 C suites, CLI smoke and CI lint | Adapted to the fork's RETURN executor |
| 2026-11-24 | `51fd445f663fefabada1cb84b500039ece157479` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | C-family .inc classification | Windows CLANG64 C suites, CLI smoke and CI lint | Uses the fork's disk classifier |
| 2026-11-24 | `5a666f2fa3431a3389027a3428e37816f7208f77` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Repo-relative search_code directory globs and empty-result hints | Windows CLANG64 C suites, CLI smoke and CI lint | Covers the PowerShell and grep backends |
| 2026-11-24 | `5c54201ef8ed8903ffdaf8f417308c9f679aa09c` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Watcher cooldown after successful reindexing | Windows CLANG64 C suites, CLI smoke and CI lint | Preserves managed remotes and pending change baselines |
| 2026-11-24 | `cbf379472f2358c6d3ca26fa64d5969779ae07e8` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Five-minute watcher cooldown cap and scheduling tests | Windows CLANG64 C suites, CLI smoke and CI lint | Watcher test API is enabled only in test builds |
| 2026-11-24 | `a031122a4dece52db1b8a41b3944ccab43acad19` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Retrofit Java/Kotlin outbound HTTP_CALLS edges | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Adapted to the fork's route creation and deduplication |
| 2026-11-24 | `c8c4b6c7dbb0ff87d3183b443e8190a3d79f642d` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | Limit Retrofit detection to interface methods with verb paths | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | JAX-RS resources and class controllers retain HANDLES |
| 2026-11-24 | `5830c55c509df33313acd163b4293c56b1e2908c` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | C-family string line splices under CRLF | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Preserves source offsets during extraction |
| 2026-11-24 | `8efb059fe8d40797eb91000439bda05802a99a1b` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | integrated | manual port | C/C++ cross-file LSP handling of CRLF splices | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Checked target method and strategy against the LF fixture |
| 2026-11-24 | `08a7a586a41587c62079a4c3482d5a6068523480` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | Dotted TypeScript alias filenames from PR #2975 | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Resolves the alias against files in the fork's graph |
| 2026-11-24 | `1862935360e7625f2746f56146fb3be5ae87c449` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | Local resolution for missing or external ES import targets | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Retains ExternalPackage nodes and dependency edges |
| 2026-11-24 | `f9158d9e6ac24164a91403bf7a4cec0c93137ab9` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | Discover deploy configs, scripts and ordinary env directories | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Adapted discovery, package walks and ignore overrides |
| 2026-11-24 | `a21efacfdebf29c170b3845666c05eef7a5341e5` | `be8aa9799453364e9d9f3bc4ac0e844e0341e31e` | partial | manual port | Incremental discovery of changes under deploy/ | Windows x64 full and incremental graph fixtures, CLANG64 C suites and CI lint | Fork fixture checks added Helm configuration after sequential and parallel incremental indexing |
