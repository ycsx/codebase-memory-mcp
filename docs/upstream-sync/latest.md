# Upstream Sync Candidate

- Checked at: `2026-10-05T09:25:50Z`
- Source: `https://github.com/DeusData/codebase-memory-mcp.git` (`main`)
- Upstream commit: `268a9d8886642eb7f9b2ce45f5ce27cdecf0f519`
- Target commit: `3a5ce0c673a4ddfb4871e58be3de75684f52f5f4`
- History relation: `unrelated`
- Merge base: `none`
- Target-only commits: `48`
- Source-only commits: `3436`
- Last integrated upstream commit: `none`
- Sync policy: `selective-transplant`

> This is a review report. No upstream source code was merged by the audit. The local feature set remains the baseline.

## Source commits

- `268a9d8886642eb7f9b2ce45f5ce27cdecf0f519` 2026-10-04	Merge pull request #2470 from DeusData/chore/codeql-action-4.38.1
- `64f5e918bbc9534170a7dadb419f68181db9464c` 2026-10-04	Merge pull request #2256 from DeusData/distill/1245-int-arg-range
- `2803d6a5fea7a1e46e87d386a6caa08008403d1e` 2026-10-04	Merge pull request #2255 from DeusData/distill/1245-route-literal-exts
- `8ab555ee38a7b17995a61602c00f9c92f090d251` 2026-10-04	Merge pull request #2253 from DeusData/distill/1245-cypher-partial-where
- `100ea8f3b29a7d6a17b75964aedac8c676a6cf9f` 2026-10-04	Merge pull request #2489 from DeusData/fix/path-alias-skip-links
- `2e7ed659dd09f030aa1fed6c16f86318ee697fcb` 2026-10-04	Merge pull request #2488 from DeusData/test/daemon-runtime-connection-cap-deterministic
- `8a7519dfd6f24fa0fd662a99c3222f4b64800c6c` 2026-10-04	Merge pull request #2480 from DeusData/fix/setup-one-install-path
- `7bb1de0ed52f8359bfc9eaff76d737495e5d1ccb` 2026-10-04	Merge pull request #2479 from DeusData/fix/workspace-resolved-home
- `f40b1a564b6ad957aa0125a9689647ca8fefd32a` 2026-10-04	Merge pull request #2475 from DeusData/fix/regex-compile-size-budget
- `5c774862ac01cf89b027110a3c554c28d0665df5` 2026-10-04	Merge pull request #2474 from DeusData/fix/search-arg-bounds
- `94801254fd23e949f36a9eb9ead6ab415748f9c7` 2026-10-04	Merge pull request #2473 from DeusData/fix/pkg-wrapper-archive-limits
- `b05a68ce7b52755cfe58262c3b3ee08d20254054` 2026-10-04	Break-glass merge PR #2544
- `d8ece6ca9b60319472dc61f73f5c4356ffa8bf7e` 2026-10-04	fix(build): reconcile three call sites after concurrent merges
- `982621dd08e4f7c8c92cacefe6a3df5da6563327` 2026-10-04	Merge pull request #2459 from DeusData/fix/doc-comment-capture
- `b7f67c29a7b39227083d35fb5e0bb09dcb448fa1` 2026-10-04	Merge pull request #2526 from DeusData/fix/underscore-project-discovery-v2
- `86f49a0f8714758e1f55650d82817ef7552a030a` 2026-10-04	Merge pull request #2524 from DeusData/fix/issue-1366-search-hint-v2
- `369cf768c2335a335de49a447a556ba42600a765` 2026-10-04	Merge pull request #2523 from DeusData/fix/issue-514-constructor-fields-v2
- `266e2814e30d8dfcf6e664401550b1fbf20f7e80` 2026-10-04	Merge pull request #2518 from DeusData/fix/search-code-scratch-isolation-v2
- `5e6fa657ee17b6333115503c8fca1ab5d8e2ff3c` 2026-10-04	Merge pull request #2517 from DeusData/fix/uninstall-keeps-indexes-v2
- `20fd3aadb57217f0fbf615e167defe61bb11ff8a` 2026-10-04	Merge pull request #2511 from DeusData/fix/issue-1364-distinct-before-cap
- `873a9665048e5f84240f50a45fae0e6c43f794e4` 2026-10-04	Merge pull request #2510 from DeusData/fix/issue-1484-close-range-v2
- `02abf310167c7d54810fde845ce7fc4579407ddd` 2026-10-04	Merge pull request #2509 from DeusData/fix/issue-1186-psr4-imports-v2
- `dffec08c40c94f6b01cbc00ae59857024fd12ba5` 2026-10-04	Merge pull request #2498 from DeusData/fix/issue-1277-py-crossfile-fields-v2
- `3a26acf402490e0d70152b347c62eaf7b240cf2f` 2026-10-04	Merge pull request #2496 from DeusData/fix/issue-1357-default-branch-v2
- `79363adbc731387afb60dff69ec6833f8cdc4532` 2026-10-04	Merge pull request #2495 from DeusData/fix/issue-598-graphql-route-name
- `5a9f6e7637e25463a4a44672a796f7022d4f9a7e` 2026-10-04	Merge pull request #2494 from DeusData/fix/issue-1300-silent-worker-exit
- `bad3e482e243b8207b896b07b96c5e464ee339d3` 2026-10-04	Merge pull request #2469 from DeusData/fix/issue-1419-sqlite-write-amplification
- `b5649e9823bc3ba8019cccc03ff93247114f4961` 2026-10-04	Merge pull request #2468 from DeusData/fix/issue-1167-1180-client-detect
- `8c12de9b165e8de590deb51e1cb53fb9a04697fb` 2026-10-04	Merge pull request #2467 from DeusData/fix/issue-2228-toml-foreign-tables
- `a5d2ea0e187e049fa941deee350511e0e9f6b7a7` 2026-10-04	Merge pull request #2466 from DeusData/fix/issue-1153-instance-receivers
- `3aef1c0bd83f9323bf76b31af6a6b1be80e25346` 2026-10-04	Merge pull request #2464 from DeusData/fix/route-registration-before-xlang-suppression
- `971afc069d20eb351d021f07e2d8a74949135e74` 2026-10-04	Merge pull request #2463 from DeusData/fix/http-axios-capitalized-import
- `59a44d80a983784eb3b4b4146d6b968c0c2d86a5` 2026-10-04	Merge pull request #2462 from DeusData/fix/http-typed-call-no-url-twin
- `c4e1ad4c87a07e17518f12893eb8ee6f694904a3` 2026-10-04	Merge pull request #2448 from DeusData/fix/hook-augment-log-path
- `6644fc433df59969dc0040aae18b01168f84fa8f` 2026-10-04	Merge pull request #2447 from DeusData/fix/issue-1146-laravel-withrouting-prefix
- `f6fe11dde9243594b9eccbedfbba1e2b4c71fe83` 2026-10-04	Merge pull request #2446 from DeusData/fix/issue-1690-skill-project-name
- `01fd6cb5e680662d55b040703d6b08b128b8e10f` 2026-10-04	Merge pull request #2342 from DeusData/feat/callable-identity-plumbing
- `db1dcf4600c473465acb4b89ecc6f23659d8d555` 2026-10-04	Merge pull request #2333 from DeusData/fix/issue-1948
- `510fc2d6954e5301a71159aabdab1c45ae16f814` 2026-10-04	Merge pull request #2329 from DeusData/fix/issue-1748
- `4793550568f6b822ac1bc1f3fef1607b2971a2bd` 2026-10-03	style(cli): format uninstall help with clang-format 20
- `a8abd97644c63dd8e1fe242943ad1f7dd5954b49` 2026-10-03	fix(mcp): discover projects with underscore-prefixed names
- `91117bdf8909cbe6203b8f770bee794400bca891` 2026-10-03	fix(mcp): avoid misleading empty-search filter hints
- `a4a65ed0ea91542b9d25b425028fc6d5053427a1` 2026-10-03	fix(typescript): resolve constructor properties and new-initialized fields
- `c38bfff4c229ad5b8b4d2a5c9fafafdc1b82993f` 2026-10-03	test(mcp): isolate search scratch cleanup fixtures
- `ba6307f0379c47ad5500e3bef739a070804611c6` 2026-10-03	fix(cli): keep project indexes by default during uninstall
- `fe8dd9df7e90860c7434bacb164774e5f30c8737` 2026-10-03	fix(mcp): release path filters on search setup errors
- `bf2daa36036745b6b4bac5b118cd2ba778efa30c` 2026-10-03	fix(lsp): detach field arrays before overlay refinement
- `91ba8a3aa922ff8258d55a867e9473afcd4b406d` 2026-10-03	fix(subprocess): close inherited descriptors with kernel range operation
- `830f0ccfae65349b8fed2e92d37163b5cc34347c` 2026-10-03	fix(pipeline): resolve PSR-4 imports to exact class files
- `519c765e1fd67ec12127ab41ec5f231401900ba8` 2026-10-03	fix(python): retain typed fields across file boundaries

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
