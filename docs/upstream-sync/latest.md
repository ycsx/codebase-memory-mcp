# Upstream Sync Candidate

- Checked at: `2026-09-28T08:51:45Z`
- Source: `https://github.com/DeusData/codebase-memory-mcp.git` (`main`)
- Upstream commit: `1f9b4db12c745e1b4554f822667627aa09d4e6bd`
- Target commit: `3a5ce0c673a4ddfb4871e58be3de75684f52f5f4`
- History relation: `unrelated`
- Merge base: `none`
- Target-only commits: `48`
- Source-only commits: `3209`
- Last integrated upstream commit: `none`
- Sync policy: `selective-transplant`

> This is a review report. No upstream source code was merged by the audit. The local feature set remains the baseline.

## Source commits

- `1f9b4db12c745e1b4554f822667627aa09d4e6bd` 2026-09-28	Merge pull request #2288 from JosephStarobinets/fix/lsp-surface-quadratic-array-access
- `66d9c2be8895e935f63f22979fe019562745e52b` 2026-09-28	Merge pull request #2304 from pcristin/codex/fix-cli-fixture-lifetime
- `64cf275ffa9aea01ce119f9cd7bc8f915fc1cfc5` 2026-09-28	Merge pull request #2367 from isc-tdyar/fix/iris-cls-dispatch
- `9d65f4cb6257d6b55c6052212a55caa555e8297d` 2026-09-27	Merge pull request #2362 from knewstimek/pr/windows-mkstemp-errno
- `f4654a7c772a57e97606414982d8cb6099661456` 2026-09-27	Merge pull request #2373 from DeusData/fix/defs-walk-no-cap
- `b0fe8aef949e9e9a9e8bf3114d0ff33486695f69` 2026-09-27	Merge pull request #2376 from DeusData/test/watcher-pending-free-held-state
- `07eb426c95386e299a6aa6adbf2b4eb9f272ff3b` 2026-09-27	test(watcher): wait for the 14th reindex attempt in the sustained-failure test
- `5df8b0442c48fed654c6f11d94cb6f64f00fc368` 2026-09-27	Merge pull request #2301 from ivanlinhf/chore/scoop-manifest-0.11.0
- `6935df23a95ef3d85db1dd1670d45cb6bb562b20` 2026-09-27	Merge pull request #1816 from z2250389/codex-z2250389/fix-windows-unicode-git-root
- `f7cfccfd0cce3f18347b8f7c498802adab452d85` 2026-09-27	Merge pull request #2160 from htarnacki/fix/cli-json-array-args
- `4f7fa84e3210fb86c5268efe24f30bb58afab96f` 2026-09-27	Merge pull request #2359 from TheDarkniteFalls/codex/gh023-branch-head-regression
- `11b662f9f7fba92012b872dd4fcaef7ee0c1300d` 2026-09-27	Merge pull request #1716 from astandrik/codex/fix-1648-sanitized-build-config
- `4b0b5f258d047d08a107c16399a1cc0e98ac89a9` 2026-09-27	Merge pull request #2158 from htarnacki/fix/jaxrs-relative-path
- `262d7ca7659b3f1f6ee619cda2b5f6aad2815314` 2026-09-27	Merge branch 'main' into codex/fix-cli-fixture-lifetime
- `075f40e9d4ccbfead4df0e199c7730f94c2154ff` 2026-09-26	fix(python): make iris.cls() callee primary, drop the extra bare call
- `77ab4bea87008547acee60435a8e3df29d90a5cb` 2026-09-26	fix(extract): definitions walk stack grows without a ceiling
- `ed991154f212928df63ce5a5cd1159cfc8196f00` 2026-09-26	fix(windows): preserve mkstemp errors through cleanup
- `8f803f7262d1d1e58068f677f3e365048050b728` 2026-09-26	fix(python): resolve iris.cls("Pkg.X").Method() to the named class
- `bc960d87d663b7b4fc4c3ba558013c4609073762` 2026-09-09	test(cli): pin JSON array literal handling for array-typed arguments
- `efb231285d188ffda16a58f7a5ce8e0e6448638f` 2026-09-26	test(pipeline): cover changed-file and empty-commit branch HEAD refresh
- `5a9c72d8f8c65b38c66a29a34184b49edf3378ff` 2026-09-26	test(watcher): pin the pending-free drain on a held state, not a git probe
- `ff8dd47011c43c622d3dc82b33be32b4493e4ea9` 2026-09-26	fix(scoop): address v0.11.0 review feedback
- `3e67838b05e67915d472ec7698c80649ab8ad03a` 2026-09-25	Merge branch 'main' into fix/windows-unicode-repo-paths
- `64c23fab6c3eba3de226355c0ca1ff750116edab` 2026-09-25	Merge pull request #2282 from knewstimek/pr/windows-worker-recovery-fix
- `ff127ea44c56ce21bd5a4f9cf9f51bb27523d0aa` 2026-09-25	Merge branch 'main' into codex/fix-1648-sanitized-build-config
- `d772b1e5af0ce80d08aa6603ea47d1f51f324193` 2026-09-25	Merge branch 'main' into fix/jaxrs-relative-path
- `da3ea3633911b5ac1240cfd7c73c5056db2c52ff` 2026-09-25	Merge pull request #1832 from cdeust/feat/doclinks-markdown
- `8ab13c94f6030cf4bb7eb2c7280fbe85ee740a83` 2026-09-25	Merge pull request #2263 from tamerbak/fix/smoke-tool-inventory
- `c98323bff7e92a3bbefbffd065589a0bf7a4b118` 2026-09-25	Merge pull request #2199 from astandrik/fix/1696-smoke-runtime-isolation
- `a5550e1a9eaa7487b79d02a048eb82b079771c60` 2026-09-25	Merge pull request #1768 from bmcnaboe/fix/hook-augment-worktree-tests
- `5c69c149f9218ea3412945e82454792d0d016425` 2026-09-25	Merge pull request #2272 from DeusData/fix/test-daemon-bootstrap-sync-spawn
- `6f91a16fc2a753db248f122dea7e4b50763972d1` 2026-09-24	test(cli): keep fixture host until released
- `64297ff3e8e142887fc36c933d57fd4d32924cc2` 2026-09-24	chore(scoop): bump packaged manifest to 0.11.0
- `92cafd1ec888362ef3223ad669a5aa4f378a54d5` 2026-09-24	Merge branch 'main' into fix/hook-augment-worktree-tests
- `fcdc6bedb7d05489462639da38c4ac755137734f` 2026-09-24	Merge branch 'main' into fix/smoke-tool-inventory
- `f07c18702ac82957d0eb73f96f37d30184b7e6b5` 2026-09-24	Merge branch 'main' into fix/1696-smoke-runtime-isolation
- `a6d3d5d820b07ba8544ddae1668e8107d7961532` 2026-09-24	Merge branch 'main' into codex/fix-1648-sanitized-build-config
- `cf7eecd7d0b1288c7f2e290a1d1bfc1b1b6f8698` 2026-09-24	Merge branch 'main' into fix/jaxrs-relative-path
- `9b5bf70dc400fdfe989ed2bf1b623b2103bbf2f0` 2026-09-24	Merge branch 'main' into fix/test-daemon-bootstrap-sync-spawn
- `5958f546dc0f9af9b856f4bbc1e709382ba8425f` 2026-09-24	Merge pull request #2273 from DeusData/distill/1245-service-pattern-boundaries
- `5c0b2418779ea74baacb8a74374d8dbd4f8beff3` 2026-09-24	Merge pull request #2275 from DeusData/fix/win-endpoint-dir-create-race
- `1160fa3591ea2bbc20ab1f7b7f547466bb005a7d` 2026-09-24	Merge pull request #2274 from DeusData/distill/1245-c-pointer-return-types
- `b0691e0d2fd281b272f69eff970e157412e95bac` 2026-09-24	Merge pull request #1742 from bmcnaboe/codex/fix-daemon-runtime-sanitizer-timeout-only
- `181c03dc188082ef67e1f27372afbf1117d44edb` 2026-09-24	Merge pull request #1741 from bmcnaboe/codex/fix-precommit-git-environment
- `95c91b8d8d0fc11a01b9109b43d0eea94393f538` 2026-09-23	Merge pull request #2140 from Lowpower/cursor/cli-help-format-json-cb65
- `246e4190c3086d1074adbe8afe39bf4261db59b8` 2026-09-23	fix(routes): keep @Path("") unset instead of yielding an empty route path
- `d7eba5a71d2a99d7b7bc588c666d22516ac0b563` 2026-09-22	fix(pipeline): decode persisted LSP surfaces with linear array iteration
- `1d2bbc425988eb78fcd966dc8c9cfbdb814f0685` 2026-09-22	fix(windows): preserve worker recovery errors
- `9a9534cbc45d40afa4e8bf070030a806acea1f5f` 2026-09-22	docs: list REFERENCES_FILE in the README edge types table
- `6ae6cd4c3f280cb8e3ffec2858269055e390ffec` 2026-09-22	fix(pipeline): address doclinks review feedback

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
