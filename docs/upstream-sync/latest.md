# Upstream Sync Candidate

Commit `be8aa9799` contains the November 6 ports and four fixes from
the 355-commit November 24 audit. They cover TypeScript/Vue imports, C/C++ line
splices on Windows, Retrofit client routes, and deploy/env directory discovery.
Desktop MCP process sharing landed separately in `7509062a2`.
[November 24 results](2026-11-24-review.md) · [November 6 ports](2026-11-06-review.md)

- Checked at: `2026-11-24T06:34:59Z`
- Source: `https://github.com/DeusData/codebase-memory-mcp.git` (`main`)
- Upstream commit: `9a4da7a314833d8d965b74612e1f25fef5371861`
- Target at audit time: `3a5ce0c673a4ddfb4871e58be3de75684f52f5f4`
- History relation: `unrelated`
- Merge base: `none`
- Target-only commits: `48`
- Source-only commits: `5700`
- Last integrated upstream commit at audit time: `none`
- Sync policy: `selective-transplant`

The history counts and lists below describe the audit snapshot.

## Source commits

- `9a4da7a314833d8d965b74612e1f25fef5371861` 2026-11-23	Merge pull request #3052 from DeusData/feat/import-path-rule-proto-make
- `c642bb1a32399e00b9ab073e1b7bdd092488423e` 2026-11-23	Merge pull request #3054 from DeusData/perf/calls-lsp-index-tail
- `e8ba00ed7f3e13c86e967f900d1e5ccfcef09406` 2026-11-23	Merge pull request #3049 from DeusData/perf/calls-shared-lsp-index
- `fb690ae93d4fa3184b5aaa3ef3c2201300ae072e` 2026-11-23	Merge pull request #3048 from DeusData/fix/cpp-test-body-caller
- `0676695c019843d4d3a47119dea308c6eed96e5c` 2026-11-23	Merge pull request #2979 from DeusData/fix/objectscript-v1.11.0-1261
- `2b5ca24e4028efa01b385603f9cf3e8053e34cb0` 2026-11-23	Merge pull request #2809 from DavidHLP/fix/local-validation-boundaries
- `1127e8e6af399687e11435297df916e484a3e0d4` 2026-11-23	Merge pull request #2996 from b84330808/fix-dep-manifest-bom-utf16
- `a849b92b17ec3626faa7cb5c8c489ab01e62c2a3` 2026-11-23	Merge pull request #3038 from DeusData/fix/ts-import-name-fallback
- `cf7581a6d6f07a9a5c917722368e3ffd7691950c` 2026-11-19	perf(calls): engine-only calls and JVM tail matches read the per-file index
- `3800ec41a63b708b42b95706e8adf189ed713c6d` 2026-11-19	docs(pipeline): the path rule's context and manifest comments name the indexed files
- `98c89d3cd887ca1a9efdac2fb07550e1033a437d` 2026-11-19	Merge origin/main into fix/ts-import-name-fallback
- `f75e3587dd9d746afc4e4eac10a29caea4e412d9` 2026-11-19	Merge origin/main into fix/objectscript-v1.11.0-1261
- `c87a23ac10969dfe27634ef0009a909ca5333546` 2026-11-19	Merge remote-tracking branch 'origin/main' into shared/scope-a1
- `d8a5932abbc4d8fba198c0a4d9d32c26a8968dc9` 2026-11-19	fix(pipeline): split the UTF-8 encoder; guard short and BOM-only manifests
- `12ed5de74a3f4110e0d392271eb23814243e1223` 2026-11-19	Trim to the accepted fixes (#2814)
- `1111d9a9e1380a0429ef2250f63223706af6289a` 2026-11-19	Merge pull request #3016 from Cianidos/fix/search-graph-empty-semantic-query
- `f05a684179c87487b91ca4165a3625df7e32d28b` 2026-11-19	perf(calls): PHP's unindexed-receiver check reads the per-file index
- `0c66bba0352e87a8da0a227852a51f5f23b9b44b` 2026-11-19	perf(calls): the sequential resolver looks engine answers up through the parallel resolver's index
- `7ea7612a789e6b0dc320a99094aff5a74e9a3164` 2026-11-19	Merge remote-tracking branch 'origin/main' into fix/cpp-test-body-caller-land
- `3a8f82b3d63607ef7b2e07f317a4fd2ea8bdcde3` 2026-11-19	Merge pull request #3040 from DeusData/refactor/lsp-engine-table-type-scope-rows
- `c5137e2f1c618bb4009fa6c743693d03be726b63` 2026-11-19	fix(pipeline): the path rule reads the run's indexed files and never guesses
- `cbf4254e0fbdc53cb422d59f03cfed96b8bdc7c4` 2026-11-19	Merge pull request #3041 from DeusData/feat/site-hook-and-scope-facts
- `4f8b66e2d6a7021c728adb78e1dc3f5ad284d009` 2026-11-19	Merge pull request #3037 from DeusData/fix/discover-deploy-env-landing
- `f6f12246fab3fadf6feab5aa868632421458fd2f` 2026-11-19	Merge pull request #3035 from DeusData/fix/ci-tests-stop-hashing-the-runner
- `25a7e248a60ae2379e9d185c6814081905d051cc` 2026-11-19	Merge remote-tracking branch 'origin/main' into shared/scope-land
- `74816034e99769b0efea2700a552cbfbafe2300d` 2026-11-19	Merge remote-tracking branch 'origin/main' into shared/core-land
- `3649ba3349fa18ded5769dd6f023c1dee5270b01` 2026-11-19	Merge pull request #3030 from DeusData/fix/c-include-lookup-repo-root
- `271af13b64081cc79956a3356407726afd178054` 2026-11-19	Merge origin/main into fix/objectscript-v1.11.0-1261
- `68d94701514d040a8480d884e2cdcccedfb3ec59` 2026-11-18	feat(lsp): the declared-type step's fields for the further engines
- `dc5dc25e4a24ae66e0f1d4b501c85bb6cf65f506` 2026-11-18	refactor(lsp): one table of engines in the cross-file pass

## Tree differences

- `A	.claude-plugin/marketplace.json`
- `A	.codebase-memory.json`
- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/codeql/codeql-config.yml`
- `M	.github/dependabot.yml`
- `A	.github/pr-acknowledgement.md`
- `A	.github/scancode-constraints.txt`
- `D	.github/upstream-sync.json`
- `M	.github/workflows/_build.yml`
- `M	.github/workflows/_lint.yml`
- `A	.github/workflows/_memwaste.yml`
- `M	.github/workflows/_security.yml`
- `M	.github/workflows/_smoke.yml`
- `M	.github/workflows/_soak.yml`
- `M	.github/workflows/_test.yml`
- `M	.github/workflows/bug-repro.yml`
- `A	.github/workflows/cache-warm.yml`
- `M	.github/workflows/codeql.yml`
- `M	.github/workflows/dco.yml`
- `M	.github/workflows/dry-run.yml`
- `M	.github/workflows/fast-repro.yml`
- `A	.github/workflows/freebsd.yml`
- `M	.github/workflows/issue-labeler.yml`
- `M	.github/workflows/nightly-soak.yml`
- `M	.github/workflows/pages.yml`
- `A	.github/workflows/pr-acknowledgement.yml`
- `M	.github/workflows/pr.yml`

## Protected paths touched

- None detected.

## Candidate upstream paths

- `A	.claude-plugin/marketplace.json`
- `A	.codebase-memory.json`
- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/codeql/codeql-config.yml`
- `M	.github/dependabot.yml`
- `A	.github/pr-acknowledgement.md`
- `A	.github/scancode-constraints.txt`
- `D	.github/upstream-sync.json`
- `M	.github/workflows/_build.yml`
- `M	.github/workflows/_lint.yml`
- `A	.github/workflows/_memwaste.yml`
- `M	.github/workflows/_security.yml`
- `M	.github/workflows/_smoke.yml`
- `M	.github/workflows/_soak.yml`
- `M	.github/workflows/_test.yml`
- `M	.github/workflows/bug-repro.yml`
- `A	.github/workflows/cache-warm.yml`
- `M	.github/workflows/codeql.yml`
- `M	.github/workflows/dco.yml`
- `M	.github/workflows/dry-run.yml`
- `M	.github/workflows/fast-repro.yml`
- `A	.github/workflows/freebsd.yml`
- `M	.github/workflows/issue-labeler.yml`
- `M	.github/workflows/nightly-soak.yml`
- `M	.github/workflows/pages.yml`
- `A	.github/workflows/pr-acknowledgement.yml`
- `M	.github/workflows/pr.yml`

## Integration record

The ledger records the source SHA, target SHA, scope and validation for each
selected port. [Upstream sync log](../UPSTREAM_SYNC_LOG.md#entries)
