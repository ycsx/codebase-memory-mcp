#!/usr/bin/env python3
"""Offline stdio golden checks using candidate references from repository docs."""

import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
GOLDEN = ROOT / "tests/fixtures/document_links_golden.json"


def load_cases(root=ROOT, golden=GOLDEN):
    data = json.loads(golden.read_text(encoding="utf-8"))
    cases = data["cases"]
    if not 50 <= len(cases) <= 100:
        raise ValueError("golden corpus must contain 50-100 candidate references")
    seen = set()
    texts = {}
    resolved = []
    for number, (source, line, token, target) in enumerate(cases, 1):
        key = (source, line, token)
        if key in seen:
            raise ValueError(f"duplicate candidate: {key}")
        seen.add(key)
        if source not in texts:
            texts[source] = (root / source).read_text(encoding="utf-8").splitlines()
        lines = texts[source]
        if line < 1 or line > len(lines) or token not in lines[line - 1]:
            raise ValueError(f"source drift: {source}:{line}: {token!r}; review the fixture")
        if target and not (root / target).is_file():
            raise ValueError(f"expected real target is missing: {target}")
        path = Path(source)
        resolved.append({
            "id": f"doc-{number:03}",
            "source_file": source,
            "source_line": line,
            "matched_text": token,
            "target": target,
            "text": lines[line - 1],
            "fixture_path": (path.parent / f"golden-{number:03}-{path.name}").as_posix(),
        })
    return data, resolved


class StdioClient:
    def __init__(self, binary, cache):
        env = os.environ.copy()
        env["CBM_CACHE_DIR"] = str(cache)
        self.proc = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8", env=env,
        )
        self.messages = queue.Queue()
        self.sequence = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            try:
                self.messages.put(json.loads(line))
            except json.JSONDecodeError:
                continue
        self.messages.put(None)

    def request(self, method, params, timeout=120):
        self.sequence += 1
        ident = self.sequence
        self.proc.stdin.write(json.dumps({
            "jsonrpc": "2.0", "id": ident, "method": method, "params": params,
        }) + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                response = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if response is None:
                raise RuntimeError(f"MCP process exited during {method}")
            if response.get("id") != ident:
                continue
            if "error" in response:
                raise RuntimeError(str(response["error"]))
            return response["result"]
        raise TimeoutError(f"MCP timeout during {method}")

    def tool(self, name, arguments):
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(str(result))
        return json.loads(result["content"][0]["text"])

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()


def materialize(repo, data, cases):
    targets = {case["target"] for case in cases if case["target"]}
    targets.update(data.get("decoy_files", []))
    # File-reference parsing depends on indexed paths, not target code bodies.
    # Empty code stubs keep this corpus fast and distinct from full-repo indexing.
    for target in sorted(targets):
        path = repo / target
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# Golden target\n" if path.suffix == ".md" else "\n", encoding="utf-8")
    for case in cases:
        path = repo / case["fixture_path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(case["text"] + "\n", encoding="utf-8")


def check_case(case, payload):
    if not isinstance(payload.get("references"), list):
        raise AssertionError("missing reference analysis results")
    if (payload.get("reference_analysis") or {}).get("status") != "ok":
        raise AssertionError("document reference analysis is incomplete")
    if payload.get("references_truncated"):
        raise AssertionError("reference evidence unexpectedly truncated")
    refs = [
        ref for ref in payload.get("references", [])
        if (ref.get("properties") or {}).get("matched_text") == case["matched_text"]
    ]
    if not case["target"]:
        if refs:
            raise AssertionError(f"unexpected reference for {case['matched_text']!r}")
        return
    if len(refs) != 1:
        raise AssertionError(f"expected one reference, got {len(refs)}")
    ref = refs[0]
    if ref["target"]["file_path"] != case["target"]:
        raise AssertionError(f"wrong target: {ref['target']}")
    props = ref["properties"]
    if props.get("producer") != "document_links" or props.get("confidence") != 1.0:
        raise AssertionError("missing deterministic provenance")
    if props.get("document_span") != {"start_line": 1, "end_line": 1}:
        raise AssertionError(f"wrong fixture evidence span: {props.get('document_span')}")
    if props.get("source") not in ("explicit_link", "path_match"):
        raise AssertionError(f"unexpected evidence source: {props.get('source')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path)
    parser.add_argument("--check-sources", action="store_true", help="only verify corpus provenance")
    parser.add_argument("--temp-root", type=Path, help="optional writable non-system temp directory")
    args = parser.parse_args()
    data, cases = load_cases()
    positives = sum(bool(case["target"]) for case in cases)
    print(f"Corpus: {len(cases)} candidates, {positives} positive, "
          f"{len(cases) - positives} negative; {data['review_status']}")
    if args.check_sources:
        print("PASS source locations and real target paths")
        return
    binary = args.binary or ROOT / "build/c" / (
        "codebase-memory-mcp.exe" if os.name == "nt" else "codebase-memory-mcp")
    binary = binary.resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")
    if args.temp_root:
        args.temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cbm-doc-golden-", dir=args.temp_root) as temp:
        temp = Path(temp)
        repo = temp / "repo"
        repo.mkdir()
        materialize(repo, data, cases)
        client = StdioClient(binary, temp / "cache")
        try:
            client.request("initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "document-links-golden", "version": "1"},
            })
            client.proc.stdin.write(
                '{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            client.proc.stdin.flush()
            project = "document-links-golden"
            client.tool("index_repository", {
                "repo_path": str(repo), "name": project, "mode": "full",
            })
            failures = []
            for case in cases:
                try:
                    payload = client.tool("get_document", {
                        "project": project, "path": case["fixture_path"],
                    })
                    check_case(case, payload)
                    print(f"PASS {case['id']} {case['source_file']}:{case['source_line']}")
                except (AssertionError, RuntimeError, KeyError) as exc:
                    failures.append(f"{case['id']}: {exc}")
            if failures:
                for failure in failures:
                    print(f"FAIL {failure}")
                raise SystemExit(1)
        finally:
            client.close()
    print(f"PASS document-links golden: {len(cases)} candidate checks")
    print("Scope: isolated real-line excerpts with file stubs; not full-repository "
          "recall/precision or maintainer-approved annotations.")


if __name__ == "__main__":
    main()
