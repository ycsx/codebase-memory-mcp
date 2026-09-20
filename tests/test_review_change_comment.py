"""Offline regression tests; these are not real-PR acceptance samples."""

import argparse
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "review-change-comment.py"
SPEC = importlib.util.spec_from_file_location("review_change_comment", SCRIPT)
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class ReviewResultTests(unittest.TestCase):
    def setUp(self):
        self.args = argparse.Namespace(
            binary="codebase-memory-mcp", project="test", since="HEAD",
            depth=2, token_budget=4000, evidence_level="analysis",
            include_docs=True, include_tests=True, root=".",
        )

    def run_result(self, result, returncode=0, stderr=""):
        process = subprocess.CompletedProcess(
            [], returncode, stdout=json.dumps(result), stderr=stderr
        )
        with patch.object(review.subprocess, "run", return_value=process):
            return review.run_review(self.args)

    def test_plain_result(self):
        self.assertEqual(self.run_result({"status": "pass"}), {"status": "pass"})

    def test_structured_content(self):
        self.assertEqual(
            self.run_result({"structuredContent": {"status": "warn"}}),
            {"status": "warn"},
        )

    def test_text_envelope(self):
        self.assertEqual(
            self.run_result({"content": [{"text": '{"status": "block"}'}]}),
            {"status": "block"},
        )

    def test_unknown_is_preserved(self):
        self.assertEqual(self.run_result({"status": "unknown"})["status"], "unknown")

    def test_nonzero_exit_rejected_even_with_valid_json(self):
        with self.assertRaisesRegex(RuntimeError, "failed"):
            self.run_result({"status": "pass"}, returncode=1, stderr="failed")

    def test_mcp_error_rejected_before_unwrapping(self):
        with self.assertRaisesRegex(RuntimeError, "MCP error"):
            self.run_result({"isError": True, "structuredContent": {"status": "pass"}})

    def test_missing_or_invalid_status_rejected(self):
        for result in ({}, {"error": "missing graph"}, {"status": "success"},
                       {"content": [{"text": '["not a review"]'}]}):
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                self.run_result(result)

    def test_main_failure_never_posts(self):
        with patch.object(review, "parse_args", return_value=self.args), \
             patch.object(review, "run_review", side_effect=RuntimeError("failed")), \
             patch.object(review, "api_json") as api, \
             patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(review.main(), 1)
            api.assert_not_called()

    def test_dry_run_and_explicit_gate(self):
        for status, gate, expected in (("block", False, 0), ("block", True, 2),
                                       ("unknown", True, 0), ("pass", True, 0)):
            with self.subTest(status=status, gate=gate):
                self.args.rule_action = None
                self.args.dry_run = True
                self.args.fail_on_block = gate
                with patch.object(review, "parse_args", return_value=self.args), \
                     patch.object(review, "run_review", return_value={"status": status}), \
                     patch.object(review, "commit_id", return_value="abc"), \
                     patch.object(review, "read_codeowners", return_value=(None, [])), \
                     patch.object(review, "api_json") as api, \
                     patch("sys.stdout", new_callable=io.StringIO) as stdout:
                    self.assertEqual(review.main(), expected)
                    self.assertIn(f"Status: **{status}**", stdout.getvalue())
                    api.assert_not_called()


class CommentTests(unittest.TestCase):
    body = "<!-- cbm-review:test:abc -->\nReview"

    def test_github_creates_comment(self):
        endpoint = "https://api.github.com/repos/org/repo/issues/7/comments"
        with patch.object(review, "api_json", side_effect=[[], {}]) as api:
            review.github_comment(self.body, "token", "org/repo", "7", "https://api.github.com")
        self.assertEqual(api.call_args_list[0].args,
                         ("GET", endpoint + "?per_page=100&page=1", "token"))
        self.assertEqual(api.call_args_list[1].args,
                         ("POST", endpoint, "token", {"body": self.body}))

    def test_github_updates_comment_on_second_page(self):
        first_page = [{"body": "unrelated"}] * 100
        second_page = [{"id": 42, "body": self.body}]
        with patch.object(review, "api_json", side_effect=[first_page, second_page, {}]) as api:
            review.github_comment(self.body, "token", "org/repo", "7", "https://api.github.com")
        self.assertTrue(api.call_args_list[1].args[1].endswith("page=2"))
        self.assertEqual(api.call_args_list[2].args,
                         ("PATCH", "https://api.github.com/repos/org/repo/issues/comments/42",
                          "token", {"body": self.body}))

    def test_gitlab_base_url_variants(self):
        for base in ("https://gitlab.com", "https://gitlab.com/",
                     "https://gitlab.com/api/v4", "https://gitlab.com/api/v4/"):
            with self.subTest(base=base):
                with patch.object(review, "api_json", side_effect=[[], {}]) as api:
                    review.gitlab_comment(self.body, "token", "org/repo", "7", base)
                endpoint = "https://gitlab.com/api/v4/projects/org%2Frepo/merge_requests/7/notes"
                self.assertEqual(api.call_args_list[0].args[1],
                                 endpoint + "?per_page=100&page=1")
                self.assertEqual(api.call_args_list[1].args,
                                 ("POST", endpoint, "token", {"body": self.body}))

    def test_gitlab_updates_note_on_second_page(self):
        responses = [[{"body": None}] * 100, [{"id": 42, "body": self.body}], {}]
        with patch.object(review, "api_json", side_effect=responses) as api:
            review.gitlab_comment(self.body, "token", "org/repo", "7", "https://gitlab.com")
        self.assertEqual(api.call_args_list[2].args,
                         ("PUT", "https://gitlab.com/api/v4/projects/org%2Frepo/merge_requests/7/notes/42",
                          "token", {"body": self.body}))

    def test_full_page_without_match_reaches_empty_page(self):
        with patch.object(review, "api_json", side_effect=[[{"body": "other"}] * 100, []]) as api:
            self.assertIsNone(review.find_comment("endpoint", self.body, "token"))
        self.assertEqual(api.call_count, 2)

    def test_invalid_api_response_does_not_post(self):
        with patch.object(review, "api_json", return_value={"error": "denied"}) as api:
            with self.assertRaises(RuntimeError):
                review.github_comment(self.body, "token", "org/repo", "7", "https://api.github.com")
        self.assertEqual(api.call_count, 1)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        # Shape from handle_review_change; intentionally synthetic, not a PR sample.
        self.result = {
            "status": "warn", "risk": "high", "summary_zh": "Static review",
            "changed_files": ["src/api.c"],
            "changed_symbols": [{
                "id": 1, "name": "handle", "qualified_name": "demo.api.handle",
                "label": "Function", "file_path": "src/api.c",
                "start_line": 10, "end_line": 18,
            }],
            "impacts": [{
                "id": 2, "name": "caller", "qualified_name": "demo.worker.caller",
                "label": "Function", "file_path": "src/worker.c", "hop": 2,
                "risk": "medium", "changed_by": "demo.api.handle",
            }],
            "summary": {"direct_impacts": 12, "indirect_impacts": 1},
            "analysis_meta": {
                "freshness": {"status": "stale", "reason_codes": ["worktree_dirty"]},
                "coverage": {"status": "unknown", "signal": "best_effort"},
            },
            "rules": [
                {"id": "change.public_api", "status": "warn",
                 "message": "Check compatibility", "evidence": ["changed_symbols"]},
                {"id": "change.cross_service", "status": "warn",
                 "message": "Check consumers", "evidence": ["impacts"]},
                {"id": "change.high_impact", "status": "block",
                 "message": "Check regression tests", "evidence": ["summary.direct_impacts"]},
                {"id": "change.missing_tests", "status": "warn",
                 "message": "No tests found", "evidence": ["src/api.c"]},
                {"id": "change.graph_freshness", "status": "warn",
                 "message": "Stale graph", "evidence": ["analysis_meta.freshness"]},
                {"id": "change.coverage", "status": "unknown",
                 "message": "Unknown coverage", "evidence": ["analysis_meta.coverage"]},
            ],
        }

    def render(self):
        with patch.object(review, "commit_id", return_value="abc"), \
             patch.object(review, "read_codeowners", return_value=(None, [])):
            return review.markdown(self.result, "demo", "HEAD~1", Path("."))

    def test_actual_schema_resolves_sources_and_reachability(self):
        body = self.render()
        self.assertIn("src/api.c:10-18", body)
        self.assertIn("demo.api.handle", body)
        self.assertIn("demo.worker.caller", body)
        self.assertIn("src/worker.c (line unavailable)", body)
        self.assertIn("hop ` 2 `", body)
        self.assertIn("intermediate path not provided", body)
        self.assertIn("Impact counts: direct ` 12 `, indirect ` 1 `", body)
        self.assertIn("Changed file: ` src/api.c `", body)
        self.assertIn('"status": "stale"', body)
        self.assertIn('"status": "unknown"', body)
        self.assertNotIn("](", body)

    def test_absent_or_unresolvable_evidence_is_explicit(self):
        self.result["rules"] = [
            {"id": "missing", "status": "warn", "evidence": []},
            {"id": "git", "status": "unknown", "evidence": ["git"]},
            {"id": "bad", "status": "warn", "evidence": [None]},
        ]
        body = self.render()
        self.assertIn("Evidence unavailable: no source or graph reference provided", body)
        self.assertIn("Unresolved evidence reference: ` git `", body)
        self.assertIn("Evidence unavailable: malformed reference", body)

    def test_no_fabricated_line_number(self):
        self.result["changed_symbols"][0]["start_line"] = 0
        self.assertIn("src/api.c (line unavailable)", self.render())
        self.assertNotIn("src/api.c:0", self.render())

    def test_missing_reference_targets_are_explicit(self):
        self.result["changed_symbols"] = []
        self.result["impacts"] = []
        self.result["analysis_meta"] = None
        body = self.render()
        self.assertIn("has no source nodes", body)
        self.assertIn("Evidence unavailable: ` analysis_meta.freshness `", body)

    def test_markdown_and_html_in_messages_are_escaped(self):
        self.result["rules"][0]["message"] = "[click](https://evil.test)\n<img src=x> **bold** `x`"
        self.result["summary_zh"] = "<script>alert(1)</script>"
        body = self.render()
        self.assertIn(r"\[click\]\(https\://evil\.test\)", body)
        self.assertIn("&lt;img src=x&gt;", body)
        self.assertNotIn("<script>", body)
        self.assertNotIn("<img", body)
        self.assertNotIn("**bold**", body)

    def test_untrusted_reference_is_literal_not_link(self):
        self.result["rules"][0]["evidence"] = ["[click](https://evil.test)`<img src=x>"]
        body = self.render()
        self.assertIn("Unresolved evidence reference: `` [click](https://evil.test)`<img src=x> ``", body)
        self.assertNotIn('<a href=', body)

    def test_evidence_count_and_field_length_limits_are_disclosed(self):
        self.result["changed_symbols"] *= 20
        self.result["rules"][0]["message"] = "x" * 10000
        body = self.render()
        self.assertEqual(body.count("Changed symbol:"), 5)
        self.assertIn("Evidence truncated: additional entries omitted", body)
        self.assertIn(r"\[truncated\]", body)
        self.assertLess(len(body), 50000)

    def test_rules_and_whole_comment_are_bounded(self):
        self.result["rules"] *= 100
        body = self.render()
        self.assertIn("Rules truncated: additional rules omitted", body)
        self.assertLess(len(body), 50000)

    def test_total_comment_limit_is_disclosed(self):
        self.result["changed_symbols"][0]["qualified_name"] = "q" * 1000
        self.result["changed_symbols"][0]["file_path"] = "p" * 1000
        self.result["changed_symbols"] *= 6
        self.result["rules"] = [self.result["rules"][0]] * 20
        body = self.render()
        self.assertIn("Comment truncated: additional content omitted.", body)
        self.assertLess(len(body), 50000)

    def test_marker_cannot_be_broken_by_project_name(self):
        with patch.object(review, "commit_id", return_value="abc"), \
             patch.object(review, "read_codeowners", return_value=(None, [])):
            body = review.markdown(self.result, "demo-->\n<script>", "HEAD", Path("."))
        self.assertEqual(body.splitlines()[0], "<!-- cbm-review:demo--%3E%0A%3Cscript%3E:abc -->")

    def test_legacy_marker_for_normal_project_names_is_preserved(self):
        with patch.object(review, "commit_id", return_value="abc"), \
             patch.object(review, "read_codeowners", return_value=(None, [])):
            body = review.markdown(self.result, "org/repo name", "HEAD", Path("."))
        self.assertEqual(body.splitlines()[0], "<!-- cbm-review:org/repo name:abc -->")


if __name__ == "__main__":
    unittest.main()
