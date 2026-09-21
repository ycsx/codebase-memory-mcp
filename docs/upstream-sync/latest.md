# Upstream Sync Candidate

- Checked at: `2026-09-21T08:06:55Z`
- Source: `https://github.com/DeusData/codebase-memory-mcp.git` (`main`)
- Upstream commit: `def38f3ebcd6c00978373315c9785ec56270a2bf`
- Target commit: `3a5ce0c673a4ddfb4871e58be3de75684f52f5f4`
- History relation: `unrelated`
- Merge base: `none`
- Target-only commits: `48`
- Source-only commits: `3088`
- Last integrated upstream commit: `none`
- Sync policy: `selective-transplant`

> This is a review report. No upstream source code was merged by the audit. The local feature set remains the baseline.

## Source commits

- `def38f3ebcd6c00978373315c9785ec56270a2bf` 2026-09-21	Merge pull request #2257 from DeusData/fix/mcp-index-args-cleanup-helper
- `c1128db8187db2dac27a7ad14120fc09802c8018` 2026-09-20	fix(test): the worker-scope script sends the policy a supervisor sends
- `af3e003ad8e67cd1fe3b20dafaf4d33a4b351852` 2026-09-20	fix(mcp): one release point for handle_index_repository's argument strings
- `92abefa3f57a94591a92bcecd6a6f102da373575` 2026-09-20	Merge pull request #1723 from liuchong/feat/index-resource-discovery
- `9b6c99e794487fec973bc1e64e1baebafdf42e4b` 2026-09-20	Merge pull request #2230 from yasmoradi/fix/dotnet-xml-extensions
- `5b4be65b75cc701128b528eedf1120a7f5805842` 2026-09-20	Merge pull request #2203 from DavidHLP/fix/issue-2175
- `36f770842af8aa615e6c01e403643f6df62e7fbc` 2026-09-20	Merge pull request #2248 from DeusData/fix/1714-freshness-merge-loss
- `96a2fc35dfcf14662cee491430366f4e8d8b8149` 2026-09-20	test(mcp): ask for JSON where the coverage test asserts a JSON field
- `f347d59650acad3513d8b3a1d5993054e805c122` 2026-09-20	Merge pull request #1787 from umi008/fix/issue-1714-freshness-mtime
- `d6082f2025d8f91366ec7a1fc8ddd427c8e9fa56` 2026-09-20	Merge pull request #2002 from metehanulusoy/fix/cypher-nonnumeric-limit-silently-dropped
- `5aeebbac683b4dc6772c0a919f2f1cdd51674bd1` 2026-09-20	Merge pull request #2197 from astandrik/fix/1696-memlab-runtime-isolation
- `6cd1900643e883d0690fa2808a523c0bff3372c2` 2026-09-20	Merge pull request #2196 from astandrik/fix/1696-soak-runtime-isolation
- `d1be39287dedcdbf31be107fe1dc1b17aa389721` 2026-09-20	Merge pull request #2241 from lorenzozanee/fix/optional-match-count-unbound
- `60c307e8d53a9bcd5d188e8f31baf83c3a25f6a5` 2026-09-20	Merge pull request #2242 from lorenzozanee/fix/typescript-awaited-generic-calls
- `c58eb61f04d5943b6a924ef19ff39de1099e09ee` 2026-09-20	Merge pull request #2243 from DeusData/build/codeql-action-4.38.0
- `45c45be5be6ff7e47ef6074826e0560569981a88` 2026-09-20	Merge pull request #2186 from DeusData/dependabot/github_actions/github/codeql-action/upload-sarif-4.38.0
- `243b405afeb3998707ea4d556f3178c8e8419cab` 2026-09-20	build(deps): bump codeql-action init and analyze together to v4.38.0
- `f1e51133e9ec0708756e0ca9311b6b0f3a8cca34` 2026-09-20	Merge pull request #2233 from DeusData/feat/memwaste-sanitizer
- `e8a46c16e233bff111092dff18985cca94890bcc` 2026-09-20	fix(typescript): preserve awaited generic call edges
- `6166934defd9bc5b687ab9c8de3779e71a844395` 2026-09-20	fix(cypher): ignore unbound optional variables in count
- `9db483402ea2fd4e0a5ff3bdd2b1fc50a7518ec9` 2026-09-19	fix(mem,spill): close two data races the worker threads exposed
- `9db5217bdbb779212c5d066ed1a84f7ca6fc0282` 2026-09-19	fix(vm): require the completion marker of the full leg only
- `dae1e7dff225d7e2a878c10cd0b00856da2bfd1a` 2026-09-19	style: format slow_move's attribute so both clang-format versions agree
- `8c1a9d61c27292bf7a52f10fb54186d4e24de28f` 2026-09-19	fix(extract): no clock decides what ends up in the graph
- `c7ccb479b454680ce104e9bffdf143f68bd312c9` 2026-09-19	fix(grammar): give the properties scanner per-parser state
- `6f290e11d0f46c065b052db2ff70a7bbceede5dd` 2026-09-19	build: make each grammar object depend on the sources it includes
- `2f33999044533b3e86d1d4de25e5b8d6ce58714d` 2026-09-19	test(vm): let the log decide the Windows leg, not the exit status
- `11cb476dff14194ba772f3dda6b9be640e3f6e0e` 2026-09-19	fix(semantic): inject callee tokens in a fixed order
- `e176ae552b99f0fd08a2dde8bdf52025a3e20059` 2026-09-19	Merge branch 'main' into fix/issue-2175
- `1d58385b513a414ead34544346e86549dda3c4b1` 2026-09-18	refactor(mem): route this branch's allocations through the memory core
- `6b48097471e8d1a3e5cf16447304899a21e12d7a` 2026-09-18	fix(routes): make Route extraction independent of worker merge order
- `9a2bdc8c1dfedd124db0076ebde7d2590ce78382` 2026-09-18	fix(pipeline): spilling must not change the graph
- `c3714f11339e7c312c5c313a065624e92c2eec56` 2026-09-18	fix(mem): size the budget from free memory, and relieve on real scarcity
- `92e7e6a91374e9f89c74ca45e01f4e525e331837` 2026-09-18	fix: keep large thread-locals off the static TLS block
- `f0a86ef2c2bc352a479162cca79d1aa64598f09e` 2026-09-18	feat(ci): seeded fuzz leg, and register both new legs as canonical entries
- `959f4de50cca16bb0eb07308886f9600e6613130` 2026-09-18	perf: cut the allocation churn and repeated work the sanitizer found
- `632c19d904da691f33fce571d6879e34e59ce54e` 2026-09-18	fix(mem): charge the larger of tracked-live and allocator commit
- `81264814f162ebfbecfc609a951afa93aa43f8c9` 2026-09-18	feat(qa): memory + CPU waste sanitizer over the memory core
- `e0becdeb094dd5e898997a2add826a9f84befbd7` 2026-09-17	fix(discover): map MSBuild, resx, XAML and app-manifest files to XML
- `59a05eb1bf9e11deb060d782cd7d3a29f2ae2866` 2026-09-16	Merge pull request #2220 from DeusData/chore/sync-version-0.11.0
- `ce0b4b214e1e29f3ea642dc348f52a90d14abbc2` 2026-09-16	Merge pull request #2221 from DeusData/docs/readme-test-count-0.11.0
- `22ce867efd88dff9c219f789967ed2612329ee6d` 2026-09-16	docs(readme): current test count — 8,060 tests across 141 suites
- `62fd80b842f00bd72e655d23c6afbfe12e199b6b` 2026-09-16	chore(pkg): sync packaging metadata to v0.11.0
- `8972ea69c6ad94b1ef1d4ffbf0a92d78d2db1798` 2026-09-15	Break-glass merge PR #2212
- `172787de9c18352bb29545da2426d9d61b260fc3` 2026-09-15	fix(mem): no glibc malloc_trim; the static Linux release links again
- `2058d49a04b785315c9f5bb56b6e2365822b576b` 2026-09-14	Merge pull request #2202 from DeusData/feat/memory-core
- `d70b79417978a9f3ab0d1aa849879dbf049793cf` 2026-09-14	fix(mem): the MSan guard in the spill store goes through sanitized.h
- `d89ccabef064661794269c0de726021736d72b51` 2026-09-14	fix(mem): the parked result image is declared defined for MemorySanitizer
- `9e69cb7d9b7ad4b40fa5f8cbbb653dc70e108130` 2026-09-14	fix(mem): free the thread locale at exit; a settling gate keeps its latch
- `9724d903046626df0640ced218259cb58070e4e8` 2026-09-14	fix(mem): dedicated SQLite heap only in the worker; guard the committed stat

## Tree differences

- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/pr-acknowledgement.md`
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
- `M	.github/workflows/issue-labeler.yml`
- `M	.github/workflows/nightly-soak.yml`
- `M	.github/workflows/pages.yml`
- `A	.github/workflows/pr-acknowledgement.yml`
- `M	.github/workflows/pr.yml`
- `M	.github/workflows/release.yml`
- `M	.github/workflows/scorecard.yml`
- `M	.github/workflows/smoke.yml`
- `D	.github/workflows/soak.yml`
- `M	.github/workflows/stale.yml`
- `D	.github/workflows/upstream-sync.yml`
- `M	.gitignore`
- `M	CODE_OF_CONDUCT.md`
- `M	CONTRIBUTING.md`
- `T	Formula`
- `D	INSTALL.md`
- `M	MAINTAINERS.md`
- `M	Makefile.cbm`
- `M	README.md`
- `M	SECURITY.md`
- `M	THIRD_PARTY.md`
- `D	deploy/install-server.sh`
- `D	deploy/nginx/codebase-memory-mcp.conf`
- `D	deploy/systemd/codebase-memory-mcp.service`
- `D	desktop/.gitignore`
- `D	desktop/README.md`
- `D	desktop/assets/icon.png`
- `D	desktop/assets/icon.svg`
- `D	desktop/electron-builder.yml`
- `D	desktop/package-lock.json`
- `D	desktop/package.json`

## Protected paths touched

- `D	desktop/.gitignore`
- `D	desktop/README.md`
- `D	desktop/assets/icon.png`
- `D	desktop/assets/icon.svg`
- `D	desktop/electron-builder.yml`
- `D	desktop/package-lock.json`
- `D	desktop/package.json`

## Candidate upstream paths

- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/pr-acknowledgement.md`
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
- `M	.github/workflows/issue-labeler.yml`
- `M	.github/workflows/nightly-soak.yml`
- `M	.github/workflows/pages.yml`
- `A	.github/workflows/pr-acknowledgement.yml`
- `M	.github/workflows/pr.yml`
- `M	.github/workflows/release.yml`
- `M	.github/workflows/scorecard.yml`
- `M	.github/workflows/smoke.yml`
- `D	.github/workflows/soak.yml`
- `M	.github/workflows/stale.yml`
- `D	.github/workflows/upstream-sync.yml`
- `M	.gitignore`
- `M	CODE_OF_CONDUCT.md`
- `M	CONTRIBUTING.md`
- `T	Formula`
- `D	INSTALL.md`
- `M	MAINTAINERS.md`
- `M	Makefile.cbm`
- `M	README.md`
- `M	SECURITY.md`
- `M	THIRD_PARTY.md`
- `D	deploy/install-server.sh`
- `D	deploy/nginx/codebase-memory-mcp.conf`
- `D	deploy/systemd/codebase-memory-mcp.service`

## Next step

Treat the current target tree as authoritative for existing behavior. Review the listed commits and paths, port only compatible upstream algorithms or features, preserve all local features, run the relevant checks, then use `scripts/upstream_sync.py record` with full source and target SHAs.
