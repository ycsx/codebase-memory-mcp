"""Safety and evidence checks for isolated cross-repository validation."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "workflow_eval", ROOT / "scripts/eval-document-workflow.py")
EVAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVAL)


class WorkflowSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / "original"
        self.source.mkdir()
        self.destination = Path(self.temp.name) / "copy"

    def put(self, name, data=b"real bytes\n"):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def snapshot(self, paths, **kwargs):
        with patch.object(EVAL, "git", side_effect=["\0".join(paths) + "\0", "abc123\n"]):
            return EVAL.snapshot(self.source, self.destination, **kwargs)

    def test_bytes_preserved_and_hashes_recorded(self):
        code = self.put("src/main.py", b"def answer():\n    return 42\n")
        doc = self.put("docs/guide.md", b"# Guide\n\nSee ../src/main.py\n")
        report = self.snapshot(["src/main.py", "docs/guide.md"])
        self.assertEqual((self.destination / "src/main.py").read_bytes(), code.read_bytes())
        self.assertEqual((self.destination / "docs/guide.md").read_bytes(), doc.read_bytes())
        self.assertEqual(report["files"][0]["sha256"], hashlib.sha256(doc.read_bytes()).hexdigest())
        self.assertEqual(report["source_head"], "abc123")

    def test_vendor_and_large_files_excluded(self):
        names = ["a.py", "a.md", "node_modules/b.py", "vendored/c.py", "huge.py"]
        for name in names:
            self.put(name, b"x" * 500_001 if name == "huge.py" else b"ok")
        report = self.snapshot(names)
        self.assertEqual(report["excluded_count"], 3)
        self.assertEqual(len(report["files"]), 2)

    def test_bounded_selection_deterministic(self):
        names = ["z.py", "a.py", "z.md", "a.md"]
        for name in names:
            self.put(name)
        report = self.snapshot(names, max_code=1, max_docs=1)
        self.assertEqual([item["path"] for item in report["files"]], ["a.md", "a.py"])

    def test_path_escape_rejected(self):
        self.put("a.py")
        self.put("a.md")
        (self.source.parent / "outside.py").write_bytes(b"outside")
        report = self.snapshot(["a.py", "a.md", "../outside.py"])
        self.assertEqual(report["excluded_count"], 1)
        self.assertEqual((self.destination.parent / "outside.py").read_bytes(), b"outside")

    def test_missing_corpus_category_rejected(self):
        self.put("a.py")
        with self.assertRaisesRegex(ValueError, "source and Markdown"):
            self.snapshot(["a.py"])

    def test_source_symlink_excluded(self):
        self.put("a.py")
        self.put("a.md")
        target = self.put("real.py")
        try:
            (self.source / "link.py").symlink_to(target)
        except OSError:
            self.skipTest("symlink privilege unavailable")
        report = self.snapshot(["a.py", "a.md", "link.py"])
        self.assertEqual(report["excluded_count"], 1)


class WorkflowContractTests(unittest.TestCase):
    def setUp(self):
        self.ref = {"source": {"file_path": "docs/guide.md", "qualified_name": "demo.doc"},
                    "target": {"file_path": "src/core.py", "qualified_name": "demo.core"},
                    "review": {"state": "pending", "token": "token1"}}

    def test_reference_state_must_match(self):
        from unittest.mock import Mock
        client = Mock()
        client.tool.return_value = {"references": [self.ref]}
        self.assertEqual(EVAL.reference(client, "demo", expected_state="pending"), self.ref)
        with self.assertRaisesRegex(AssertionError, "confirmed"):
            EVAL.reference(client, "demo", expected_state="confirmed")

    def test_reference_missing_cannot_pass(self):
        from unittest.mock import Mock
        client = Mock()
        client.tool.return_value = {"references": []}
        with self.assertRaisesRegex(AssertionError, "one generated relation"):
            EVAL.reference(client, "demo")

    def test_stale_token_requires_tool_error(self):
        from unittest.mock import Mock
        client = Mock()
        client.request.return_value = {"isError": False}
        with self.assertRaisesRegex(AssertionError, "accepted"):
            EVAL.rejected_transition(client, "demo", self.ref)
        client.request.return_value = {"isError": True}
        EVAL.rejected_transition(client, "demo", self.ref)

    def test_transition_passes_exact_reference_and_token(self):
        from unittest.mock import Mock
        client = Mock()
        EVAL.transition(client, "demo", self.ref, "reopen")
        client.tool.assert_called_once_with("update_document_review", {
            "project": "demo", "source_qualified_name": "demo.doc",
            "target_qualified_name": "demo.core", "token": "token1", "action": "reopen"})

    def test_coverage_requires_summary_and_pagination(self):
        payload = {"summary": {"indexed_code_files": 2, "referenced_code_files": 1,
                               "indexed_documents": 1, "limited_documents": 0,
                               "unknown_documents": 0},
                   "items": [{"file_path": "src/core.py", "status": "referenced"}],
                   "total": 2, "returned": 1, "has_more": True}
        EVAL.assert_coverage(payload)
        payload["summary"]["referenced_code_files"] = 3
        with self.assertRaisesRegex(AssertionError, "exceeds indexed code"):
            EVAL.assert_coverage(payload)
        with self.assertRaisesRegex(AssertionError, "summary"):
            EVAL.assert_coverage({})


if __name__ == "__main__":
    unittest.main()
