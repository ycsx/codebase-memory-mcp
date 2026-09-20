# Real-Document Reference Golden Corpus

`document_links_golden.json` contains 63 selected candidate references from eight
existing repository Markdown documents: 33 expected matches and 30 expected
non-matches. Each record pins a source path, original line number, exact candidate
text, and expected repository-relative target (or null).

Annotations were prepared during development and remain
`maintainer_review_pending`; they are not user-approved ground truth.

## Running

```sh
python scripts/eval-document-links.py --check-sources
python scripts/eval-document-links.py build/c/codebase-memory-mcp
python -m unittest discover -s tests -p test_eval_document_links.py
```

On Windows, use the `.exe` binary and optionally `--temp-root C:/msys64/tmp`
to select an ASCII temporary path outside system directories.

The evaluator uses a temporary isolated cache and local MCP stdio only. It checks
the source locations against the checkout, indexes one verbatim source line per
case at the same directory depth, and supplies small file stubs at real target
paths. It then verifies `get_document` target paths, provenance, confidence, and
line evidence. Source drift or missing positive targets requires fixture review,
not silent re-annotation.

## Boundaries

- The selected unit is a candidate reference, not an entire Markdown document.
- Other references on the same source line are not scored.
- Surrounding block structure and real target code bodies are intentionally
  outside this fast corpus; parser and incremental C tests cover those separately.
- Negative cases cover stale paths, short names, globs, directories, external
  project examples, and remote URLs. The "Key test files" list in
  `CONTRIBUTING.md` references the absent `tests/test_httplink.c`, so that record
  is intentionally negative.
- A passing result is regression evidence, not a representative whole-repository
  precision/recall measurement or completion of all W11 acceptance criteria.
