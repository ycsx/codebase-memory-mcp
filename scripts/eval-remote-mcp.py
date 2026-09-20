#!/usr/bin/env python3
"""Read-only HTTP transport soak, not multi-principal ACL or graph-query acceptance."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import http.client
import ipaddress
import json
import math
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class CheckError(Exception):
    """Safe, fixed diagnostic without response bodies or credentials."""

    def __init__(self, code, session=None):
        super().__init__(code)
        self.session = session


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def validate_url(url, allow_remote):
    try:
        parsed = urllib.parse.urlsplit(url)
        port = parsed.port
        host = parsed.hostname
    except ValueError:
        raise CheckError("invalid_url") from None
    if (parsed.scheme not in ("http", "https") or not host
            or parsed.username is not None or parsed.password is not None
            or parsed.query or parsed.fragment or parsed.path != "/mcp"
            or port == 0):
        raise CheckError("invalid_url")
    # Only literal loopback addresses bypass explicit remote opt-in.
    try:
        loopback = ipaddress.ip_address(host).is_loopback
    except ValueError:
        loopback = False
    if not loopback and not allow_remote:
        raise CheckError("remote_requires_allow_remote")
    if not loopback and parsed.scheme != "https":
        raise CheckError("remote_requires_https")


def exchange(opener, url, token, timeout, payload=None, session=None, method="POST"):
    headers = {"Authorization": "Bearer " + token, "Accept": "application/json",
               "Content-Type": "application/json", "MCP-Protocol-Version": "2025-03-26"}
    if session:
        headers["Mcp-Session-Id"] = session
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    sid = None
    try:
        with opener.open(request, timeout=timeout) as response:
            status = response.status
            sid = response.headers.get("Mcp-Session-Id")
            body = response.read(8 * 1024 * 1024 + 1)
    except urllib.error.HTTPError as exc:
        code = exc.code
        exc.close()
        raise CheckError("http_" + str(code)) from None
    except (OSError, ValueError, urllib.error.URLError, http.client.HTTPException):
        raise CheckError("transport_error", sid) from None
    if len(body) > 8 * 1024 * 1024:
        raise CheckError("response_too_large", sid)
    expected = 204 if method == "DELETE" else (200 if "id" in payload else 202)
    if status != expected:
        raise CheckError("unexpected_http_" + str(status), sid)
    if expected != 200:
        return None, sid
    try:
        result = json.loads(body)
    except (ValueError, UnicodeError):
        raise CheckError("invalid_json", sid) from None
    if not isinstance(result, dict) or result.get("jsonrpc") != "2.0":
        raise CheckError("invalid_rpc", sid)
    if result.get("id") != payload["id"]:
        raise CheckError("rpc_id_mismatch", sid)
    if "error" in result:
        raise CheckError("rpc_error", sid)
    if not isinstance(result.get("result"), dict):
        raise CheckError("missing_result", sid)
    if result["result"].get("isError"):
        raise CheckError("tool_error", sid)
    return result["result"], sid


def run_soak(args, token):
    """No retries: every attempted workload request contributes to the report."""
    validate_url(args.url, args.allow_remote)
    if (args.sessions < 1 or args.requests < args.sessions
            or not math.isfinite(args.timeout) or args.timeout <= 0):
        raise CheckError("invalid_limits")
    barrier = threading.Barrier(args.sessions)
    lock = threading.Lock()
    seen = set()
    errors = Counter()
    counts = Counter()
    latencies = []
    started = time.perf_counter()

    def error(phase, exc):
        with lock:
            errors[phase + ":" + str(exc)] += 1

    def worker(index):
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        session = None
        ready = False
        try:
            payload = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-03-26", "capabilities": {},
                "clientInfo": {"name": "cbm-transport-soak", "version": "1"}}}
            try:
                result, session = exchange(opener, args.url, token, args.timeout, payload)
                if not session or len(session) > 128 or not session.isalnum():
                    session = None
                    raise CheckError("invalid_session")
                with lock:
                    duplicate = session in seen
                    seen.add(session)
                    counts["sessions_initialized"] += 1
                if duplicate:
                    raise CheckError("duplicate_session")
                if result.get("protocolVersion") != "2025-03-26":
                    raise CheckError("protocol_mismatch")
                exchange(opener, args.url, token, args.timeout,
                         {"jsonrpc": "2.0", "method": "notifications/initialized"},
                         session)
                ready = True
            except CheckError as exc:
                if not session and exc.session and len(exc.session) <= 128 and exc.session.isalnum():
                    session = exc.session
                error("initialize", exc)
            # Failed initialization still joins the barrier so peers can finish.
            try:
                barrier.wait(timeout=args.timeout * 2 + 1)
            except threading.BrokenBarrierError:
                error("synchronize", CheckError("barrier_broken"))
                ready = False
            if ready:
                total = args.requests // args.sessions + (index < args.requests % args.sessions)
                for number in range(total):
                    method = "tools/list" if (number + index) % 2 == 0 else "tools/call"
                    params = {} if method == "tools/list" else {
                        "name": "list_projects", "arguments": {}}
                    payload = {"jsonrpc": "2.0", "id": number + 2,
                               "method": method, "params": params}
                    before = time.perf_counter()
                    try:
                        result, sid = exchange(opener, args.url, token, args.timeout,
                                               payload, session)
                        if sid is not None and sid != session:
                            raise CheckError("session_mismatch")
                        if method == "tools/list" and not isinstance(result.get("tools"), list):
                            raise CheckError("invalid_tools_list")
                    except CheckError as exc:
                        error("workload", exc)
                    finally:
                        elapsed = (time.perf_counter() - before) * 1000
                        with lock:
                            counts[method] += 1
                            latencies.append(elapsed)
        except Exception:
            # Do not print arbitrary exception text: it can include request headers.
            error("worker", CheckError("unexpected_error"))
            barrier.abort()
        finally:
            if session:
                try:
                    exchange(opener, args.url, token, args.timeout,
                             session=session, method="DELETE")
                    with lock:
                        counts["sessions_deleted"] += 1
                except CheckError as exc:
                    error("cleanup", exc)
                except Exception:
                    error("cleanup", CheckError("unexpected_error"))

    try:
        with ThreadPoolExecutor(max_workers=args.sessions) as pool:
            list(pool.map(worker, range(args.sessions)))
    except Exception:
        error("worker_pool", CheckError("unexpected_error"))
    ordered = sorted(latencies)
    attempted = len(ordered)
    return {
        "scope": "http_transport_soak",
        "not_validated": ["multi_principal_acl", "graph_query_performance",
                          "team_operation_acceptance"],
        "passed": not errors and attempted == args.requests,
        "sessions_requested": args.sessions,
        "sessions_initialized": counts["sessions_initialized"],
        "unique_sessions": len(seen),
        "sessions_deleted": counts["sessions_deleted"],
        "workload_requests_requested": args.requests,
        "workload_requests_attempted": attempted,
        "workload_requests_skipped": args.requests - attempted,
        "workload_methods": {name: counts[name] for name in ("tools/list", "tools/call")},
        "workload_latency_ms_p95": round(ordered[math.ceil(len(ordered) * .95) - 1], 3)
        if ordered else None,
        "elapsed_seconds": round(time.perf_counter() - started, 3),
        "error_count": sum(errors.values()),
        "errors": dict(sorted(errors.items())),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:9766/mcp")
    parser.add_argument("--token-env", default="CBM_REMOTE_MCP_TOKEN")
    parser.add_argument("--allow-remote", action="store_true",
                        help="Explicitly permit a non-loopback HTTPS endpoint")
    parser.add_argument("--sessions", type=int, default=20)
    parser.add_argument("--requests", type=int, default=1000,
                        help="Workload only; excludes initialize, notification and DELETE")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    token = os.environ.get(args.token_env, "")
    try:
        if not token or any(ord(char) < 33 or ord(char) > 126 for char in token):
            raise CheckError("missing_or_invalid_token_environment")
        report = run_soak(args, token)
    except CheckError as exc:
        print(json.dumps({"scope": "http_transport_soak", "passed": False,
                          "configuration_error": str(exc)}))
        return 2
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
