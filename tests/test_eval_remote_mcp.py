"""Local fake-server checks, not evidence of real remote M3 acceptance."""

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "eval-remote-mcp.py"
SPEC = importlib.util.spec_from_file_location("eval_remote_mcp", SCRIPT)
soak = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(soak)
TOKEN = "test-token-never-print-this"


@contextmanager
def server(mode="ok"):
    state = {"created": 0, "deleted": [], "workload": 0}
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, status, body=None, session=None):
            data = json.dumps(body).encode() if body is not None else b""
            self.send_response(status)
            self.send_header("Content-Length", str(len(data)))
            if session:
                self.send_header("Mcp-Session-Id", session)
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.headers.get("Authorization") != "Bearer " + TOKEN:
                self.reply(401)
                return
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            method = payload["method"]
            if method == "initialize":
                if mode == "initialize":
                    self.reply(503)
                    return
                with lock:
                    state["created"] += 1
                    sid = "same" if mode == "duplicate" else str(state["created"])
                self.reply(200, {"jsonrpc": "2.0",
                                "id": 999 if mode == "initialize_rpc" else payload["id"],
                                "result": {"protocolVersion": "2025-03-26"}}, sid)
                return
            if method == "notifications/initialized":
                self.reply(503 if mode == "notification" else 202)
                return
            with lock:
                state["workload"] += 1
            result = {"tools": []} if method == "tools/list" else {"content": []}
            body = {"jsonrpc": "2.0", "id": payload["id"], "result": result}
            if mode == "rpc":
                body = {"jsonrpc": "2.0", "id": payload["id"],
                        "error": {"message": TOKEN, "code": -1}}
            if mode == "tool":
                body["result"]["isError"] = True
            if mode == "id":
                body["id"] = 9999
            if mode == "http":
                self.reply(503)
            else:
                self.reply(200, body)

        def do_DELETE(self):
            with lock:
                state["deleted"].append(self.headers.get("Mcp-Session-Id"))
            self.reply(500 if mode == "cleanup" else 204)

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:%s/mcp" % httpd.server_port, state
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


class SoakTests(unittest.TestCase):
    def run_mode(self, mode="ok"):
        with server(mode) as (url, state):
            args = argparse.Namespace(url=url, allow_remote=False, sessions=3,
                                      requests=11, timeout=3)
            report = soak.run_soak(args, TOKEN)
        self.assertNotIn(TOKEN, json.dumps(report))
        self.assertEqual(len(state["deleted"]), 3)
        return report

    def test_success_counts_only_workload(self):
        report = self.run_mode()
        self.assertTrue(report["passed"])
        self.assertEqual(report["workload_requests_attempted"], 11)
        self.assertEqual(report["unique_sessions"], 3)
        self.assertEqual(report["sessions_deleted"], 3)
        self.assertEqual(sum(report["workload_methods"].values()), 11)
        self.assertGreaterEqual(report["workload_latency_ms_p95"], 0)

    def test_failures_counted_without_retry_and_cleanup_runs(self):
        for mode, key in (("rpc", "rpc_error"), ("tool", "tool_error"),
                          ("id", "rpc_id_mismatch"), ("http", "http_503")):
            with self.subTest(mode=mode):
                report = self.run_mode(mode)
                self.assertFalse(report["passed"])
                self.assertEqual(report["errors"]["workload:" + key], 11)
                self.assertEqual(report["workload_requests_attempted"], 11)

    def test_duplicate_sessions_fail(self):
        report = self.run_mode("duplicate")
        self.assertFalse(report["passed"])
        self.assertEqual(report["unique_sessions"], 1)
        self.assertEqual(report["errors"]["initialize:duplicate_session"], 2)
        self.assertGreater(report["workload_requests_skipped"], 0)

    def test_cleanup_failure_fails_report(self):
        report = self.run_mode("cleanup")
        self.assertFalse(report["passed"])
        self.assertEqual(report["errors"]["cleanup:http_500"], 3)

    def test_initialize_failure_does_not_hang_or_claim_workload(self):
        with server("initialize") as (url, state):
            args = argparse.Namespace(url=url, allow_remote=False, sessions=3,
                                      requests=11, timeout=3)
            report = soak.run_soak(args, TOKEN)
        self.assertFalse(report["passed"])
        self.assertEqual(report["errors"]["initialize:http_503"], 3)
        self.assertEqual(report["workload_requests_attempted"], 0)
        self.assertEqual(report["workload_requests_skipped"], 11)
        self.assertIsNone(report["workload_latency_ms_p95"])
        self.assertEqual(state["deleted"], [])

    def test_initialize_rpc_and_notification_failures_clean_up(self):
        for mode, key in (("initialize_rpc", "rpc_id_mismatch"),
                          ("notification", "http_503")):
            with self.subTest(mode=mode):
                report = self.run_mode(mode)
                self.assertFalse(report["passed"])
                self.assertEqual(report["errors"]["initialize:" + key], 3)
                self.assertEqual(report["sessions_deleted"], 3)
                self.assertEqual(report["workload_requests_skipped"], 11)

    def test_unexpected_worker_exception_is_safe_and_does_not_hang(self):
        original = soak.exchange
        injected = threading.Event()

        def fail_once(*args, **kwargs):
            if not injected.is_set():
                injected.set()
                raise RuntimeError(TOKEN)
            return original(*args, **kwargs)

        with server() as (url, state), patch.object(soak, "exchange", side_effect=fail_once):
            args = argparse.Namespace(url=url, allow_remote=False, sessions=3,
                                      requests=11, timeout=1)
            report = soak.run_soak(args, TOKEN)
        self.assertFalse(report["passed"])
        self.assertEqual(report["errors"]["worker:unexpected_error"], 1)
        self.assertNotIn(TOKEN, json.dumps(report))
        self.assertEqual(len(state["deleted"]), 2)

    def test_unexpected_pool_exception_is_safe(self):
        args = argparse.Namespace(url="http://127.0.0.1/mcp", allow_remote=False,
                                  sessions=3, requests=11, timeout=1)
        with patch.object(soak, "ThreadPoolExecutor", side_effect=RuntimeError(TOKEN)):
            report = soak.run_soak(args, TOKEN)
        self.assertFalse(report["passed"])
        self.assertEqual(report["errors"]["worker_pool:unexpected_error"], 1)
        self.assertNotIn(TOKEN, json.dumps(report))

    def test_invalid_limits_fail_before_network(self):
        for sessions, requests, timeout in ((0, 10, 3), (3, 2, 3), (3, 10, float("nan"))):
            args = argparse.Namespace(url="http://127.0.0.1/mcp", allow_remote=False,
                                      sessions=sessions, requests=requests, timeout=timeout)
            with self.subTest(args=args), self.assertRaises(soak.CheckError):
                soak.run_soak(args, TOKEN)

    def test_remote_is_explicit_and_https_only(self):
        for url, allowed in (("http://example.com/mcp", False),
                             ("http://example.com/mcp", True),
                             ("http://localhost/mcp", False),
                             ("http://u:p@127.0.0.1/mcp", False),
                             ("http://127.0.0.1/mcp?token=x", False)):
            with self.subTest(url=url), self.assertRaises(soak.CheckError):
                soak.validate_url(url, allowed)
        soak.validate_url("http://127.0.0.1/mcp", False)
        soak.validate_url("http://[::1]/mcp", False)
        soak.validate_url("https://example.com/mcp", True)

    def test_redirects_are_not_followed(self):
        self.assertIsNone(soak.NoRedirect().redirect_request(
            None, None, 302, "", {}, "https://example.com/mcp"))

    def test_missing_credentials_never_contacts_server(self):
        with patch.dict(soak.os.environ, {}, clear=True), patch.object(
                soak, "run_soak") as run, patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(soak.main([]), 2)
        run.assert_not_called()
        self.assertEqual(json.loads(out.getvalue())["configuration_error"],
                         "missing_or_invalid_token_environment")


if __name__ == "__main__":
    unittest.main()
