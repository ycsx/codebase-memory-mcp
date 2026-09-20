#!/usr/bin/env python3
"""Check real document references in an isolated tracked-source repository copy."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("document_excerpt_eval",
                                           ROOT / "scripts/eval-document-links.py")
EXCERPTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXCERPTS)
EXPECTATIONS = ROOT / "tests/fixtures/document_links_full.json"
EXCLUDED = ("vendored/", "internal/cbm/vendored/", "node_modules/")


def materialize(repo, root=ROOT):
    tracked = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "-z"]).decode("utf-8").split("\0")
    copied, excluded = [], []
    for name in filter(None, tracked):
        source = root / name
        if name.startswith(EXCLUDED) or not source.is_file() or source.stat().st_size > 2_000_000:
            excluded.append(name)
            continue
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied.append(name)
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    return {"copied_files": len(copied), "excluded_files": excluded,
            "exclusion_policy": "vendor trees and files larger than 2000000 bytes"}


def check_case(case, expected, payload):
    analysis = payload.get("reference_analysis") or {}
    if analysis.get("status") not in ("ok", "limited"):
        raise AssertionError(f"unknown analysis: {analysis}")
    if analysis.get("status") != expected["analysis_status"]:
        raise AssertionError(f"analysis status changed: {analysis}")
    if analysis.get("reason") != expected.get("analysis_reason"):
        raise AssertionError(f"analysis reason changed: {analysis}")
    if not isinstance(payload.get("references"), list):
        raise AssertionError("missing reference analysis results")
    if payload.get("references_truncated"):
        raise AssertionError("reference evidence truncated")
    refs = [ref for ref in payload["references"]
            if ref.get("properties", {}).get("matched_text") == case["matched_text"]]
    observed = []
    for ref in refs:
        props = ref["properties"]
        if props.get("producer") != "document_links" or props.get("confidence") != 1.0:
            raise AssertionError("missing deterministic provenance")
        if props.get("source") not in ("path_match", "explicit_link", "symbol_match"):
            raise AssertionError("unexpected evidence source")
        span = props.get("document_span") or {}
        if span.get("start_line") != span.get("end_line"):
            raise AssertionError("unexpected multiline evidence")
        observed.append([ref["target"]["file_path"], span.get("start_line")])
    if sorted(observed) != sorted(expected["references"]):
        raise AssertionError(f"expected {expected['references']}, observed {observed}")


def resolve_expectation(case, data):
    analysis = data["document_analysis"].get(case["source_file"], {"status": "ok"})
    override = data["evidence_overrides"].get(case["id"])
    line = override["evidence_line"] if override else case["source_line"]
    return {"analysis_status": analysis["status"], "analysis_reason": analysis.get("reason"),
            "references": [[case["target"], line]] if case["target"] else []}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path)
    parser.add_argument("--temp-root", type=Path)
    parser.add_argument("--output", type=Path, required=True,
                        help="write evidence report, not a generated golden approval")
    parser.add_argument("--collect-only", action="store_true",
                        help="collect evidence before reviewing independent expectations")
    args = parser.parse_args()
    _, cases = EXCERPTS.load_cases()
    expected = None if args.collect_only else json.loads(EXPECTATIONS.read_text(encoding="utf-8"))
    binary = (args.binary or ROOT / "build/c" / (
        "codebase-memory-mcp.exe" if os.name == "nt" else "codebase-memory-mcp")).resolve()
    if args.temp_root:
        args.temp_root.mkdir(parents=True, exist_ok=True)
    report = {"scope": "full-document tracked first-party source snapshot",
              "review_status": "maintainer_review_pending", "documents": {}, "failures": []}
    with tempfile.TemporaryDirectory(prefix="cbm-doc-full-", dir=args.temp_root) as temp:
        temp = Path(temp)
        repo = temp / "repo"
        repo.mkdir()
        report["snapshot"] = materialize(repo)
        client = EXCERPTS.StdioClient(binary, temp / "cache")
        try:
            client.request("initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "document-links-full", "version": "1"}})
            client.proc.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            client.proc.stdin.flush()
            result = client.request("tools/call", {
                "name": "index_repository", "arguments": {
                    "repo_path": str(repo), "name": "doc-full-trial", "mode": "full"}},
                timeout=900)
            if result.get("isError"):
                raise RuntimeError(str(result))
            report["index_result"] = result
            for source in sorted({case["source_file"] for case in cases}):
                payload = client.tool("get_document", {"project": "doc-full-trial", "path": source})
                report["documents"][source] = payload
                print(f"COLLECT {source}: {len(payload.get('references', []))} references", flush=True)
            if expected is not None:
                for case in cases:
                    try:
                        check_case(case, resolve_expectation(case, expected),
                                   report["documents"][case["source_file"]])
                        print(f"PASS {case['id']} {case['source_file']}:{case['source_line']}")
                    except (AssertionError, KeyError) as exc:
                        report["failures"].append(f"{case['id']}: {exc}")
        finally:
            client.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    for failure in report["failures"]:
        print(f"FAIL {failure}")
    if report["failures"]:
        raise SystemExit(1)
    print("COLLECTED (not an acceptance pass)" if args.collect_only
          else f"PASS full-document references: {len(cases)} candidates")
    print("Scope: vendor/large-file exclusions; maintainer review pending; no precision claim.")


if __name__ == "__main__":
    main()
