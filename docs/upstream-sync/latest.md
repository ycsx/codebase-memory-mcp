# Upstream Sync Candidate

- Checked at: `2026-08-31T08:48:22Z`
- Source: `https://github.com/DeusData/codebase-memory-mcp.git` (`main`)
- Upstream commit: `ec08f76e12aa0bbe3186b7e6c81f3fd91c3bf280`
- Target commit: `c11fa3cd96935b5f1684cceb1d95bc22b2cba5de`
- History relation: `unrelated`
- Merge base: `none`
- Target-only commits: `40`
- Source-only commits: `2610`
- Last integrated upstream commit: `none`
- Sync policy: `selective-transplant`

> This is a review report. No upstream source code was merged by the audit. The local feature set remains the baseline.

## Source commits

- `ec08f76e12aa0bbe3186b7e6c81f3fd91c3bf280` 2026-08-30	Merge pull request #1899 from DeusData/dependabot/github_actions/github/codeql-action/upload-sarif-4.37.9
- `a0f6476ee787f1dec7d8ef15b9bfa93e89103f35` 2026-08-30	Merge pull request #1794 from DeusData/dependabot/github_actions/actions/attest-build-provenance-4.2.2
- `3de05cd607f72889e89908852b1877fb08c942c1` 2026-08-30	Merge pull request #1905 from DeusData/feat/868-artifact-reconcile
- `997d087b211124a904af58ae9545356de97daf2f` 2026-08-29	Merge pull request #1904 from DeusData/feat/adr-section-update
- `4cbc9e1ca2bee240b83341626b21680d69c4924d` 2026-08-29	fix(artifact): stamp the clean-basis trust check the way the rows were written
- `1189c875dbbefca3b96e98a1badc32d7aec76d7a` 2026-08-29	Merge pull request #1908 from DeusData/feat/lsp-cross-inheritance
- `a27266309c2c07e2ce07073f6ef0c462699b7c5f` 2026-08-29	Merge pull request #1888 from DeusData/feat/919-chialisp-distill
- `a9d635f2993879edecb73f754ac59dfc0758e7ec` 2026-08-29	fix(test): set git identity on the artifact test clone
- `d90f98f5f4f183eedfe2c10990b307790b944780` 2026-08-29	fix(store): keep ADR line endings intact when splicing a section
- `2910e2842f912be2dbb3a8c7fbcd939881cc6616` 2026-08-29	feat(lsp): resolve cross-file base classes for the Python/TS cross-LSP
- `5d96377aa5d4c887d03af9cd28c14f42f3068a57` 2026-08-29	Merge pull request #1903 from DeusData/feat/1324-python-weak-member
- `614009481d9f7c938318be09c6f08679d9d700b9` 2026-08-28	feat(artifact): reconcile imported hashes against git on bootstrap
- `5eae752f489654a7e1b04450a0f77256d0b4b05f` 2026-08-29	test(parallel): record the weak-member trade for the LSP-ambiguity probe
- `8f112f1640fa65886e7a4c988783403e318bf1d1` 2026-08-29	Merge pull request #1902 from DeusData/fix/version-metadata-gate
- `d013555d28f618948e0b927ab612d5737488894a` 2026-08-29	feat(mcp): splice ADR sections instead of rebuilding the document
- `7bef7f4d663eddab37f3470e8dbcf7d4a5545327` 2026-08-29	feat(mcp): add manage_adr mode='set_sections'
- `b6a22843207af16aa533952dce4a697d2e26a241` 2026-08-29	fix(pipeline): suppress weak Python member calls (distilled from #1324)
- `3b2c6cbe3b66117e7a656ca1fdff44e21862cc83` 2026-08-29	Merge pull request #1866 from DeusData/fix/issue-474-search-code-scan-deadline
- `1130f577ee27385ca2e5449e9964b7221ee95ec8` 2026-08-29	Merge pull request #1883 from DeusData/feat/879-importance-distill
- `82c4067f79593279f7b6ae5f0e62b5117631ce4a` 2026-08-29	fix(test): release the node retrieved per USAGE edge in the Chialisp contract
- `d8f8f45fda1e6bb7a44c942427f1ad7b5c0af5de` 2026-08-29	Merge pull request #1901 from DeusData/docs/issue-612-runtime-trace-model
- `4ae5a7353cefc942d3bf2bd63bea05306c60eac8` 2026-08-29	Merge pull request #1885 from DeusData/feat/issue-525-graph-compare
- `ae8fd2c903b08010107b688017e5ee423191e73d` 2026-08-29	fix(test): keep the version contract alive under pipefail with no tags
- `5821078ee503a7a4e0735b0255c647e5d9f57339` 2026-08-29	fix(pkg): correct stale packaging versions to v0.10.8 + gate the drift
- `be7b57828e2862a0123777c748ef831cedbe574c` 2026-08-29	Merge pull request #1882 from DeusData/feat/518-519-bm25-prose
- `b10c2744d6c1f180dd4b417d35f3c28dc9ba86fc` 2026-08-29	docs: define runtime trace observation model
- `7d896fee54f8a46b356d495feca3ec4082380d6b` 2026-08-29	chore(deps): Bump github/codeql-action/upload-sarif
- `31bdd4d682eb9812b1130f4a1b55b3725feed834` 2026-08-28	fix(mcp): bound search code scans
- `51c48e2093e9ad6c375b396e9624a0135cfb8351` 2026-08-29	Merge pull request #1858 from DeusData/fix/issue-1834-pi-stdin-transport
- `ef8a8dc3be801edaa9625e7e283271ed76468701` 2026-08-29	Merge pull request #1853 from DeusData/fix/issue-1746-dockerfile-eof-whitespace-resume
- `56dc82ec11415de9e46eaf9faa316ecef2916184` 2026-08-29	Merge pull request #1850 from DeusData/fix/issue-56-rust-parallel-cargo-context-resume
- `202ea657bca5c0199d9e0d0062b8e76ce3938fae` 2026-08-29	Merge pull request #1848 from DeusData/fix/issue-403-sensitive-programs-root-resume
- `489b1771a7a12843e28c871549739f412c71c3ef` 2026-08-29	Merge pull request #1847 from DeusData/docs/issue-361-measuring-savings-resume
- `e2251f304220bb8452314b910a760e93ace321d2` 2026-08-29	Merge pull request #1846 from DeusData/codex/issue-1753-clients-forwarding
- `574356343d8531e1c10ee9d627d5da47e5829b47` 2026-08-29	Merge pull request #1845 from DeusData/fix/issue-1764-idle-frontend-cpu-resume
- `dc23628c6f00a818466fe09f0f180413333aee80` 2026-08-28	test(search): pin that nodes_fts.body is indexed raw, not camelCase-split
- `b493b644cf4e77f4ff9bd5b30862b01ba7ed3198` 2026-08-28	test(search): end-to-end guard from source prose to a findable body match
- `01c7e549ef633d147191e2b06097b39227c05f7c` 2026-08-28	feat(search): index prose in BM25 via a nodes_fts body column
- `4e9fdccb137bbb63bfa0bc142dcf8da89e0c3a1e` 2026-08-29	Merge pull request #1879 from DeusData/fix/venue-parity-probe-sigpipe
- `ee0a2e6dc6e72e574e84c59133941e23e7d51a0c` 2026-08-28	docs(grammars): recount the manifest's ABI distribution from the tree
- `e6398fd9579f428a9ae418c75ec95dfd134c27dd` 2026-08-28	style(tests): keep the Chialisp test diffs to the lines that changed
- `706bb2ce81da399f88b6d010d6c28272434aa9f5` 2026-08-28	feat(lang): add Chialisp support with a first-party tree-sitter grammar
- `7ba26f5216b466c6e939e1d2ac58313cd65ed9a3` 2026-08-28	fix: sanitize graph comparison metadata
- `079d8dcb891ee2a0d45345124e1207a0c4b1993d` 2026-08-28	fix: harden graph comparison identities
- `cf6e620431954034523a2705c35f9d4f553c4688` 2026-08-28	feat(mcp): add bounded graph comparison tool
- `396ac348b5abe0e1a7c6cc7bb9d604e8ee76cefc` 2026-08-28	fix(pipeline): score importance over the staging store on the delta route
- `e5f3aab02e1c8bdb3d3e04f6b52d78d741e1241f` 2026-08-28	feat(pipeline): index-time importance scoring (weighted degree)
- `cbd241d7f1b33248501010cbaac50b07785373b8` 2026-08-28	fix(tests): venue-parity interface probes must not pipe into grep -q
- `31b611a30e01aa8f306f9fcba32492cad0702f99` 2026-08-28	Merge pull request #1872 from DeusData/feat/1412-cfml-distill
- `d706c33ac2f92c9c51e70e1f11503a572569efd3` 2026-08-28	Merge pull request #1337 from abendrothj/bash-tool-hook-augment

## Tree differences

- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/pr-acknowledgement.md`
- `D	.github/upstream-sync.json`
- `M	.github/workflows/_build.yml`
- `M	.github/workflows/_lint.yml`
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
- `D	deploy/nginx/codebase-memory-mcp.conf`
- `D	deploy/systemd/codebase-memory-mcp.service`
- `D	desktop/.gitignore`
- `D	desktop/README.md`
- `D	desktop/assets/icon.png`
- `D	desktop/assets/icon.svg`
- `D	desktop/electron-builder.yml`
- `D	desktop/package-lock.json`
- `D	desktop/package.json`
- `D	desktop/scripts/generate-icon.cjs`
- `D	desktop/src/binary-locator.cjs`

## Protected paths touched

- `D	desktop/.gitignore`
- `D	desktop/README.md`
- `D	desktop/assets/icon.png`
- `D	desktop/assets/icon.svg`
- `D	desktop/electron-builder.yml`
- `D	desktop/package-lock.json`
- `D	desktop/package.json`
- `D	desktop/scripts/generate-icon.cjs`
- `D	desktop/src/binary-locator.cjs`

## Candidate upstream paths

- `D	.env.example`
- `M	.gitattributes`
- `M	.github/CODEOWNERS`
- `M	.github/ISSUE_TEMPLATE/config.yml`
- `A	.github/pr-acknowledgement.md`
- `D	.github/upstream-sync.json`
- `M	.github/workflows/_build.yml`
- `M	.github/workflows/_lint.yml`
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
- `D	deploy/nginx/codebase-memory-mcp.conf`
- `D	deploy/systemd/codebase-memory-mcp.service`

## Next step

Treat the current target tree as authoritative for existing behavior. Review the listed commits and paths, port only compatible upstream algorithms or features, preserve all local features, run the relevant checks, then use `scripts/upstream_sync.py record` with full source and target SHAs.
