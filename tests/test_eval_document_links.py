"""Unit checks for the local real-document golden evaluator."""

import copy
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "eval_document_links", ROOT / "scripts/eval-document-links.py")
EVAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVAL)


class DocumentGoldenTests(unittest.TestCase):
    def setUp(self):
        self.case = {"matched_text": "src/core.c", "target": "src/core.c"}
        self.payload = {
            "reference_analysis": {"status": "ok"},
            "references_truncated": False,
            "references": [{
                "target": {"file_path": "src/core.c"},
                "properties": {
                    "matched_text": "src/core.c",
                    "producer": "document_links",
                    "confidence": 1.0,
                    "source": "path_match",
                    "document_span": {"start_line": 1, "end_line": 1},
                },
            }],
        }

    def test_real_corpus_provenance(self):
        data, cases = EVAL.load_cases()
        self.assertEqual(data["review_status"], "maintainer_review_pending")
        self.assertEqual(len(cases), 63)
        self.assertEqual(sum(bool(case["target"]) for case in cases), 33)

    def test_positive(self):
        EVAL.check_case(self.case, self.payload)

    def test_negative(self):
        self.case["target"] = None
        self.payload["references"] = []
        EVAL.check_case(self.case, self.payload)

    def test_false_positive(self):
        self.case["target"] = None
        with self.assertRaisesRegex(AssertionError, "unexpected reference"):
            EVAL.check_case(self.case, self.payload)

    def test_false_negative(self):
        self.payload["references"] = []
        with self.assertRaisesRegex(AssertionError, "expected one"):
            EVAL.check_case(self.case, self.payload)

    def test_wrong_target(self):
        self.payload["references"][0]["target"]["file_path"] = "src/other.c"
        with self.assertRaisesRegex(AssertionError, "wrong target"):
            EVAL.check_case(self.case, self.payload)

    def test_wrong_evidence(self):
        self.payload["references"][0]["properties"]["document_span"]["start_line"] = 2
        with self.assertRaisesRegex(AssertionError, "evidence span"):
            EVAL.check_case(self.case, self.payload)

    def test_incomplete_analysis(self):
        self.payload["reference_analysis"]["status"] = "limited"
        with self.assertRaisesRegex(AssertionError, "incomplete"):
            EVAL.check_case(self.case, self.payload)

    def test_truncated_analysis(self):
        self.payload["references_truncated"] = True
        with self.assertRaisesRegex(AssertionError, "truncated"):
            EVAL.check_case(self.case, self.payload)

    def test_missing_analysis_not_a_negative_pass(self):
        self.case["target"] = None
        with self.assertRaisesRegex(AssertionError, "missing"):
            EVAL.check_case(self.case, {})

    def test_duplicate_evidence(self):
        self.payload["references"].append(copy.deepcopy(self.payload["references"][0]))
        with self.assertRaisesRegex(AssertionError, "expected one"):
            EVAL.check_case(self.case, self.payload)


if __name__ == "__main__":
    unittest.main()
