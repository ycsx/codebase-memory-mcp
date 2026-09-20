"""Full-context evaluator must not silently accept missing or limited evidence."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("full_eval", ROOT / "scripts/eval-document-links-full.py")
EVAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVAL)


class FullDocumentTests(unittest.TestCase):
    def setUp(self):
        self.case = {"id": "doc-001", "source_file": "README.md", "source_line": 8,
                     "matched_text": "src/a.c", "target": "src/a.c"}
        self.expected = {"analysis_status": "ok", "references": [["src/a.c", 8]]}
        self.payload = {"reference_analysis": {"status": "ok"}, "references_truncated": False,
                        "references": [{"target": {"file_path": "src/a.c"}, "properties": {
                            "producer": "document_links", "confidence": 1.0, "source": "path_match",
                            "matched_text": "src/a.c", "document_span": {"start_line": 8, "end_line": 8}}}]}

    def test_real_line_evidence(self):
        EVAL.check_case(self.case, self.expected, self.payload)

    def test_excerpt_line_not_accepted(self):
        self.payload["references"][0]["properties"]["document_span"] = {"start_line": 1, "end_line": 1}
        with self.assertRaises(AssertionError):
            EVAL.check_case(self.case, self.expected, self.payload)

    def test_unknown_cannot_pass_negative(self):
        self.expected["references"] = []
        self.payload.update(reference_analysis={"status": "unknown"}, references=[])
        with self.assertRaisesRegex(AssertionError, "unknown"):
            EVAL.check_case(self.case, self.expected, self.payload)

    def test_limited_requires_explicit_expectation_and_reason(self):
        self.payload["reference_analysis"] = {"status": "limited", "reason": "read_failed"}
        with self.assertRaisesRegex(AssertionError, "status"):
            EVAL.check_case(self.case, self.expected, self.payload)
        self.expected.update(analysis_status="limited", analysis_reason="unsupported_multiline_code")
        with self.assertRaisesRegex(AssertionError, "reason"):
            EVAL.check_case(self.case, self.expected, self.payload)
        self.payload["reference_analysis"]["reason"] = "unsupported_multiline_code"
        EVAL.check_case(self.case, self.expected, self.payload)

    def test_truncation_fails(self):
        self.payload["references_truncated"] = True
        with self.assertRaisesRegex(AssertionError, "truncated"):
            EVAL.check_case(self.case, self.expected, self.payload)

    def test_duplicate_fails(self):
        self.payload["references"].append(copy.deepcopy(self.payload["references"][0]))
        with self.assertRaises(AssertionError):
            EVAL.check_case(self.case, self.expected, self.payload)

    def test_all_cases_keep_original_targets(self):
        data = json.loads(EVAL.EXPECTATIONS.read_text(encoding="utf-8"))
        cases = EVAL.EXCERPTS.load_cases()[1]
        self.assertEqual(len(cases), 63)
        for case in cases:
            expected = EVAL.resolve_expectation(case, data)
            self.assertEqual([ref[0] for ref in expected["references"]],
                             [case["target"]] if case["target"] else [])
            if expected["references"]:
                line = expected["references"][0][1]
                text = (ROOT / case["source_file"]).read_text(encoding="utf-8").splitlines()
                self.assertIn(case["matched_text"], text[line - 1])
        self.assertEqual(len(data["evidence_overrides"]), 7)

    def test_materialize_preserves_real_bytes_excludes_vendor(self):
        with tempfile.TemporaryDirectory() as temp:
            root, repo = Path(temp) / "root", Path(temp) / "copy"
            (root / "src").mkdir(parents=True)
            (root / "vendored").mkdir()
            (root / "src/a.c").write_bytes(b"int actual_source(void) { return 17; }\n")
            (root / "vendored/a.c").write_bytes(b"vendor\n")
            repo.mkdir()
            with patch.object(EVAL.subprocess, "check_output", return_value=b"src/a.c\0vendored/a.c\0"):
                with patch.object(EVAL.subprocess, "run"):
                    result = EVAL.materialize(repo, root)
            self.assertEqual((repo / "src/a.c").read_bytes(), (root / "src/a.c").read_bytes())
            self.assertFalse((repo / "vendored/a.c").exists())
            self.assertEqual(result["copied_files"], 1)


if __name__ == "__main__":
    unittest.main()
