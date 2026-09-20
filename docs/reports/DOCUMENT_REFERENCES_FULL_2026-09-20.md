# Full-Document Reference Trial, 2026-09-20

## Result

The local stdio trial passed **63/63 candidate checks** across eight complete
repository documents. The evaluator unit suites passed **19 tests**.
This is a first-party regression trial, not a representative precision/recall
measurement. Annotations remain **maintainer_review_pending**.

The source baseline was `14692b6e`. No HTTP service or transport acceptance was
used. The active user cache was not read or overwritten.

## Reproduction

```sh
python scripts/eval-document-links-full.py build/c/codebase-memory-mcp.exe \
  --temp-root C:/Temp/cbm-doc-full \
  --output .tmp/document-links-full-verified.json
python -m unittest discover -s tests -p 'test_eval_document_links*.py'
```

Use the non-`.exe` binary name on POSIX. `--temp-root` is optional there.
The runner imports the existing stdio client, copies current contents of
Git-tracked files into a temporary repository, creates a separate cache, indexes
project `doc-full-trial`, and removes that temporary repository and cache after
the process exits. Its JSON output preserves document payloads, index limitations,
and excluded paths after cleanup. `--collect-only` collects evidence but explicitly
does not report an acceptance pass.

The trial copied **923 actual tracked files**, including real target source code,
not target stubs. It excluded **1,063 files** under `vendored/`,
`internal/cbm/vendored/`, `node_modules/`, or larger than 2,000,000 bytes.
These are explicit evaluator exclusions, not a claim to index every dependency
or large asset. Existing repository ignore rules remained in effect.

The index reported 19,707 nodes and 111,939 edges. It also reported 61 partially
parsed files, with the detailed list truncated; those files were indexed but
some code constructs may be missing. Three image files were excluded by suffix,
and `.git`, `deploy`, and `tmp` were excluded directories. This trial validates
deterministic document-to-file references, not code-symbol extraction completeness.
Counts describe this run and are not fixed acceptance thresholds.

## Complete Documents

| Document | Returned references | Analysis |
| --- | ---: | --- |
| CONTRIBUTING.md | 17 | ok |
| INSTALL.md | 1 | ok |
| README.md | 13 | ok |
| docs/BENCHMARK.md | 2 | ok |
| docs/EVALUATION_PLAN.md | 5 | limited: unsupported_multiline_code |
| docs/PROJECT_REPORT.md | 9 | ok |
| docs/SECURITY-DISCLOSURE.md | 1 | ok |
| docs/UPSTREAM_SYNC.md | 4 | ok |

No document response was reference-truncated. `ok` means only the supported
syntax subset was processed, not full CommonMark coverage. The evaluation plan
contains multiline inline code, including lines 185-186; the full-context
expectation explicitly requires its limited status and reason. Negative cases
inside that document establish only the absence of those returned candidate
references, not a complete analysis of the document.

## Independent Context Expectations

The original single-line fixture is unchanged: 33 positive candidates and 30
negative candidates. Every positive target remains positive in the full-context
trial. Full-document evidence is checked against
`tests/fixtures/document_links_full.json`, including provenance, target path,
real source line, status/reason, duplicate evidence, and response truncation.

Seven candidates have an earlier retained evidence line because the complete
document already mentions the same target:

| Candidate | Candidate line | Retained evidence | Reason |
| --- | ---: | ---: | --- |
| doc-009 | CONTRIBUTING.md:152 | 30 | Earlier pipeline test reference |
| doc-013 | CONTRIBUTING.md:168 | 30 | Earlier pipeline test reference |
| doc-014 | CONTRIBUTING.md:216 | 95 | Earlier inline format-script reference |
| doc-015 | CONTRIBUTING.md:216 | 29 | Earlier inline test-script reference |
| doc-016 | CONTRIBUTING.md:216 | 97 | Earlier inline lint-script reference |
| doc-026 | README.md:322 | 294 | Earlier configuration link |
| doc-027 | README.md:325 | 258 | Earlier remote-MCP link |

The earlier lines were inspected in source and the unit suite verifies each still
contains its matched text. Fenced command snippets are not promoted into prose
references. No target expectation was weakened to match an unexpected output.

## Workflow Follow-Up Rerun

After the coverage and persistent-review additions on the same date, the same
923-file trial again passed 63/63 candidates. The current
`.tmp/document-links-full-verified.json` records 19,746 nodes, 112,366 edges,
and 61 partially parsed files. Earlier counts above describe the initial run;
neither run establishes whole-repository precision or recall.

## Remaining Review

- Maintainer review of the 63 annotations is still pending.
- This is one repository, with deliberate vendor and size exclusions.
- The corpus is not a random sample and cannot establish a >=95% precision claim.
- Sections, exact symbol references, incremental updates, and parser edge cases
  remain covered by their separate focused C tests; this trial does not replace
  those suites or the full CI gate.
