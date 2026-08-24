#!/usr/bin/env bash
set -euo pipefail

# Offline W3 evaluator. It intentionally creates its own tiny repository so
# results do not depend on a user's cache, network access, or a particular
# checkout's symbols.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GOLDEN_FILE="${CBM_BUILD_CONTEXT_GOLDEN:-$ROOT_DIR/tests/fixtures/build_context_golden.json}"
if [[ $# -ge 1 ]]; then
    CBM_BIN="$1"
elif [[ -n "${CBM_BIN:-}" ]]; then
    CBM_BIN="$CBM_BIN"
else
    CBM_BIN=""
    for candidate in \
        "$ROOT_DIR/build/c/codebase-memory-mcp" \
        "$ROOT_DIR/build/c/codebase-memory-mcp.exe" \
        "$ROOT_DIR/build/codebase-memory-mcp" \
        "$ROOT_DIR/build/codebase-memory-mcp.exe"; do
        if [[ -f "$candidate" ]]; then
            CBM_BIN="$candidate"
            break
        fi
    done
    CBM_BIN="${CBM_BIN:-$ROOT_DIR/build/c/codebase-memory-mcp}"
fi

if [[ ! -f "$CBM_BIN" || ! -x "$CBM_BIN" ]]; then
    echo "SKIP: build_context evaluator needs an executable binary: $CBM_BIN"
    exit 77
fi
if [[ ! -f "$GOLDEN_FILE" ]]; then
    echo "FAIL: golden file not found: $GOLDEN_FILE" >&2
    exit 1
fi
PYTHON_BIN=""
for candidate in python3 python py; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON_BIN="$candidate"
        break
    fi
done
if [[ -z "$PYTHON_BIN" ]]; then
    echo "SKIP: Python 3 is required by the offline evaluator"
    exit 77
fi

EVAL_TMP="$(mktemp -d "${TMPDIR:-/tmp}/cbm-build-context-eval.XXXXXX")"
trap 'rm -rf "$EVAL_TMP"' EXIT
REPO_DIR="$EVAL_TMP/repo"
mkdir -p "$REPO_DIR/src" "$REPO_DIR/tests" "$REPO_DIR/docs"

cat >"$REPO_DIR/src/core.py" <<'PY'
def cache_value(value):
    return value.strip()


def shared_helper(value):
    return value.strip().lower()


def load_config(path):
    return {"path": path, "mode": "safe"}


def compute(value):
    normalized = shared_helper(value)
    return cache_value(normalized)


class Service:
    def run(self, value):
        return compute(value)


def open_page(value):
    return Service().run(value)
PY
cat >"$REPO_DIR/src/admin.py" <<'PY'
def shared_helper(value):
    return value.strip()


def render_panel(value):
    return "<admin>" + shared_helper(value) + "</admin>"
PY
cat >"$REPO_DIR/src/user.py" <<'PY'
def shared_helper(value):
    return value.strip()


def render_panel(value):
    return "<user>" + shared_helper(value) + "</user>"
PY
cat >"$REPO_DIR/tests/test_core.py" <<'PY'
from src.core import compute


def test_compute():
    assert compute(" value ") == "value"


def test_render_panel():
    assert "admin" in "admin"
PY
cat >"$REPO_DIR/README.md" <<'EOF'
# Build context evaluation fixture

The `compute` API feeds the renderer. Changes should preserve `render_panel`.
EOF
cat >"$REPO_DIR/docs/architecture.md" <<'EOF'
# Architecture

The core service calls compute; admin and user modules expose render_panel.
EOF

if command -v git >/dev/null 2>&1; then
    git -C "$REPO_DIR" init -q
    git -C "$REPO_DIR" config user.email cbm-eval@example.invalid
    git -C "$REPO_DIR" config user.name cbm-eval
    git -C "$REPO_DIR" add .
    git -C "$REPO_DIR" commit -qm baseline
    printf '\n# evaluation change\n' >>"$REPO_DIR/src/core.py"
    git -C "$REPO_DIR" add src/core.py
    git -C "$REPO_DIR" commit -qm change
else
    echo "WARNING: git is unavailable; diff_ref cases will be sent but cannot be authoritative"
fi

CBM_EVAL_CACHE_DIR="$EVAL_TMP/cache" "$PYTHON_BIN" - "$CBM_BIN" "$GOLDEN_FILE" "$REPO_DIR" <<'PY'
import json
import os
import queue
import subprocess
import sys
import threading
import time

binary, golden_path, repo = sys.argv[1:]
with open(golden_path, encoding="utf-8") as fh:
    golden = json.load(fh)
cases = golden.get("cases", [])
if len(cases) < 30:
    print(f"FAIL: expected at least 30 golden cases, found {len(cases)}")
    raise SystemExit(1)

out_queue = queue.Queue()
env = os.environ.copy()
env["CBM_CACHE_DIR"] = os.environ.get("CBM_EVAL_CACHE_DIR", os.path.join(repo, ".cbm-cache"))
proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)

def read_stdout():
    for line in proc.stdout:
        try:
            out_queue.put(json.loads(line))
        except json.JSONDecodeError:
            continue
    out_queue.put(None)

threading.Thread(target=read_stdout, daemon=True).start()
request_id = 0

def request(method, params=None, timeout=120):
    global request_id
    request_id += 1
    ident = request_id
    message = {"jsonrpc": "2.0", "id": ident, "method": method}
    if params is not None:
        message["params"] = params
    proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            item = out_queue.get(timeout=max(0.05, deadline - time.time()))
        except queue.Empty:
            break
        if item is None:
            break
        if item.get("id") == ident:
            return item
    raise RuntimeError(f"timeout waiting for {method} id={ident}")

def context_payload(response):
    if "error" in response:
        raise RuntimeError(str(response["error"]))
    result = response.get("result") or {}
    if result.get("isError"):
        raise RuntimeError(str(result))
    content = result.get("content") or []
    text = content[0].get("text") if content and isinstance(content[0], dict) else None
    if not isinstance(text, str):
        return result
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        raise RuntimeError("tool content is not JSON")

failures = []
try:
    request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                            "clientInfo": {"name": "build-context-eval", "version": "1"}})
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
    proc.stdin.flush()
    # Full mode retains documentation directories so the include_docs contract
    # is exercised against the complete graph, including architecture notes.
    index_args = {"repo_path": repo, "name": golden["project"], "mode": "full"}
    indexed = request("tools/call", {"name": "index_repository", "arguments": index_args}, 180)
    if "error" in indexed or (indexed.get("result") or {}).get("isError"):
        raise RuntimeError(f"index_repository failed: {indexed}")

    def run_case(case):
        args = {k: case[k] for k in (
            "project", "task", "target", "diff_ref", "token_budget",
            "include_docs", "include_tests", "evidence_level") if k in case}
        args.setdefault("project", golden["project"])
        response = request("tools/call", {"name": "build_context", "arguments": args})
        payload = context_payload(response)
        serialized = json.dumps(payload, ensure_ascii=False, sort_keys=True)
        for term in case.get("expected_terms", []):
            if term not in serialized:
                raise AssertionError(f"missing expected term {term!r}")
        candidates = payload.get("candidates") or []
        if case.get("expect_candidates") and len(candidates) < 2:
            raise AssertionError(f"expected ambiguity, candidates={len(candidates)}")
        budget = payload.get("budget") or {}
        requested = budget.get("requested_tokens")
        estimated = budget.get("estimated_tokens")
        if requested is not None and estimated is not None:
            ratio = float(case.get("max_estimated_ratio", 1.10))
            if estimated > requested * ratio + 1:
                raise AssertionError(f"estimated={estimated} exceeds budget={requested} ratio={ratio}")
        if case.get("expect_partial"):
            meta_result = (payload.get("analysis_meta") or {}).get("result") or {}
            if not budget.get("truncated") and meta_result.get("status") != "partial":
                raise AssertionError("expected partial/truncated result")
        return payload

    for case in cases:
        first = run_case(case)
        if case.get("stable"):
            second = run_case(case)
            def order(payload):
                return ([x.get("qualified_name") for x in payload.get("candidates", [])],
                        [x.get("qualified_name") for x in payload.get("evidence", [])])
            if order(first) != order(second):
                raise AssertionError("candidate/evidence ordering changed between identical calls")
        print(f"PASS {case['id']}")
except Exception as exc:
    failures.append(str(exc))
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

if failures:
    for failure in failures:
        print(f"FAIL {failure}")
    raise SystemExit(1)
print(f"PASS build_context golden: {len(cases)} cases")
PY
