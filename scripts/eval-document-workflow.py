#!/usr/bin/env python3
"""Isolated local-repository corpus smoke checks and generated review workflows."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("document_stdio",
                                           ROOT / "scripts/eval-document-links.py")
STDIO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STDIO)
CODE_SUFFIXES = {".c", ".h", ".cpp", ".py", ".js", ".ts", ".tsx", ".jsx", ".vue",
                 ".go", ".rs", ".cs", ".gd"}
EXCLUDED_PARTS = {"vendored", "vendor", "node_modules", "dist", "build", ".git",
                  "nr-os", "public", "third_party", "third-party"}


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args],
                                   stderr=subprocess.PIPE).decode("utf-8")


def snapshot(source, destination, max_code=240, max_docs=40):
    """Copy bounded tracked regular files, never mutate or follow source symlinks."""
    source = source.resolve()
    tracked = sorted(filter(None, git(source, "ls-files", "-z").split("\0")))
    code, docs, excluded = [], [], []
    for name in tracked:
        relative = Path(name)
        path = source / relative
        if (relative.is_absolute() or ".." in relative.parts or
                any(part in EXCLUDED_PARTS for part in relative.parts) or
                path.is_symlink() or not path.is_file() or
                not path.resolve().is_relative_to(source) or path.stat().st_size > 500_000):
            excluded.append(name)
            continue
        if path.suffix.lower() in {".md", ".mdx"} and len(docs) < max_docs:
            docs.append(name)
        elif path.suffix.lower() in CODE_SUFFIXES and len(code) < max_code:
            code.append(name)
        else:
            excluded.append(name)
    if not code or not docs:
        raise ValueError(f"repository needs tracked source and Markdown: {source}")
    manifest = []
    for name in sorted(code + docs):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
        manifest.append({"path": name, "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
    return {"source_root": str(source), "source_head": git(source, "rev-parse", "HEAD").strip(),
            "kind": "real_tracked_worktree_bytes", "files": manifest, "documents": docs,
            "code_files": len(code), "excluded_count": len(excluded),
            "policy": {"max_code": max_code, "max_docs": max_docs, "max_file_bytes": 500_000,
                       "excluded_path_parts": sorted(EXCLUDED_PARTS)},
            "limitation": "Bounded snapshot; references outside copied paths can be unresolved. "
                          "Worktree bytes may differ from source HEAD; hashes record actual bytes."}


def initialize_repo(repo):
    repo.mkdir(parents=True, exist_ok=True)
    git(repo, "init", "-q")
    git(repo, "add", "--all")
    git(repo, "-c", "user.name=Document Workflow QA", "-c", "user.email=qa@example.invalid",
        "-c", "commit.gpgsign=false", "commit", "-qm", "Isolated document workflow baseline")


def open_client(binary, cache):
    client = STDIO.StdioClient(binary, cache)
    client.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                  "clientInfo": {"name": "document-workflow", "version": "1"}})
    client.proc.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    client.proc.stdin.flush()
    return client


def index(client, repo, project):
    result = client.request("tools/call", {"name": "index_repository", "arguments": {
        "repo_path": str(repo), "name": project, "mode": "full", "persistence": False}},
        timeout=900)
    if result.get("isError"):
        raise RuntimeError(str(result))
    return result


def assert_coverage(payload):
    if not isinstance(payload, dict):
        raise AssertionError("coverage must be an object")
    summary = payload.get("summary") or {}
    for key in ("indexed_code_files", "referenced_code_files", "indexed_documents",
                "limited_documents", "unknown_documents"):
        if type(summary.get(key)) is not int or summary[key] < 0:
            raise AssertionError(f"invalid coverage summary: {summary}")
    if summary["referenced_code_files"] > summary["indexed_code_files"]:
        raise AssertionError("referenced count exceeds indexed code")
    if summary["limited_documents"] + summary["unknown_documents"] > summary["indexed_documents"]:
        raise AssertionError("limited/unknown count exceeds indexed documents")
    items, pagination = payload.get("items"), payload
    if not isinstance(items, list) or pagination.get("returned") != len(items):
        raise AssertionError("coverage items do not match returned count")
    if type(pagination.get("total")) is not int or pagination["total"] < len(items):
        raise AssertionError("invalid coverage pagination total")
    if type(pagination.get("has_more")) is not bool:
        raise AssertionError("missing coverage pagination boundary")
    for item in items:
        if not item.get("file_path") or not item.get("status"):
            raise AssertionError(f"coverage row lacks file/status: {item}")


def reference(client, project, target="src/core.py", expected_state=None):
    payload = client.tool("get_related_documents", {
        "project": project, "target": "file:" + target, "limit": 100})
    matches = [item for item in payload.get("references", [])
               if item["source"]["file_path"] == "docs/guide.md"]
    if len(matches) != 1:
        raise AssertionError(f"expected one generated relation: {payload}")
    ref = matches[0]
    if expected_state and ref.get("review", {}).get("state") != expected_state:
        raise AssertionError(f"expected {expected_state} review: {ref}")
    return ref


def transition(client, project, ref, action="confirm"):
    return client.tool("update_document_review", {
        "project": project, "source_qualified_name": ref["source"]["qualified_name"],
        "target_qualified_name": ref["target"]["qualified_name"],
        "token": ref["review"]["token"], "action": action})


def rejected_transition(client, project, ref):
    result = client.request("tools/call", {"name": "update_document_review", "arguments": {
        "project": project, "source_qualified_name": ref["source"]["qualified_name"],
        "target_qualified_name": ref["target"]["qualified_name"],
        "token": ref["review"]["token"], "action": "confirm"}})
    if not result.get("isError"):
        raise AssertionError(f"stale or cross-project confirmation accepted: {result}")


def materialize_generated(repo):
    (repo / "src").mkdir(parents=True)
    (repo / "docs").mkdir()
    (repo / "src/core.py").write_text("def compute(value):\n    return value + 1\n",
                                    encoding="utf-8")
    (repo / "docs/guide.md").write_text(
        "# Core Guide\n\nThe implementation is [core](../src/core.py).\n", encoding="utf-8")
    initialize_repo(repo)


def mutation_check(binary, temp, evidence):
    """All writes and Git mutations are restricted to generated temp fixtures."""
    projects = ["workflow-mutation-a", "workflow-mutation-b"]
    repos = [temp / name for name in projects]
    cache = temp / "cache"
    client = open_client(binary, cache)
    evidence.update(kind="generated_mutation_fixtures", passed=[], observations={})

    def passed(name):
        evidence["passed"].append(name)
        print(f"PASS {name}", flush=True)

    try:
        for project, repo in zip(projects, repos):
            materialize_generated(repo)
            index(client, repo, project)
        project, repo = projects[0], repos[0]
        initial = reference(client, project, expected_state="pending")
        other = reference(client, projects[1], expected_state="pending")
        if not initial["review"].get("token") or not other["review"].get("token"):
            raise AssertionError("readable indexed reference requires a review token")
        if initial["review"]["token"] == other["review"]["token"]:
            raise AssertionError("tokens must include project identity")
        transition(client, project, initial)
        confirmed = reference(client, project, expected_state="confirmed")
        reference(client, projects[1], expected_state="pending")
        rejected_transition(client, projects[1], initial)
        passed("confirmation_and_project_isolation")

        transition(client, project, confirmed, "reopen")
        reopened = reference(client, project, expected_state="pending")
        transition(client, project, reopened)
        passed("manual_reopen")

        client.close()
        client = open_client(binary, cache)
        confirmed = reference(client, project, expected_state="confirmed")
        passed("confirmation_survives_process_restart")
        index(client, repo, project)
        confirmed = reference(client, project, expected_state="confirmed")
        passed("confirmation_survives_full_reindex")

        (repo / "src/core.py").write_text("def compute(value):\n    return value + 2\n",
                                        encoding="utf-8")
        changed = reference(client, project, expected_state="pending")
        if changed["review"]["token"] == confirmed["review"]["token"]:
            raise AssertionError("code change did not refresh token")
        rejected_transition(client, project, confirmed)
        passed("code_rechange_pending_and_stale_token_rejected")
        transition(client, project, changed)
        (repo / "docs/guide.md").write_text(
            "# Core Guide\n\nThe implementation is [core](../src/core.py).\n\nUpdated wording.\n",
            encoding="utf-8")
        doc_changed = reference(client, project, expected_state="pending")
        if doc_changed["review"]["token"] == changed["review"]["token"]:
            raise AssertionError("document change did not refresh token")
        rejected_transition(client, project, changed)
        passed("document_change_reopens_confirmation")

        review = client.tool("review_change", {"project": project, "since": "HEAD",
                                               "include_docs": True, "token_budget": 8000})
        references = review.get("related_documentation", {}).get("references", [])
        matching = [ref for ref in references if ref["source"]["file_path"] == "docs/guide.md"]
        if not matching or not matching[0].get("review", {}).get("document_changed"):
            raise AssertionError(f"co-edited documentation evidence missing: {review}")
        evidence["observations"]["coedit_review_basis"] = review["related_documentation"].get(
            "review_basis")
        passed("code_document_coedit_keeps_review_evidence")

        (repo / "src/core.py").unlink()
        unavailable = reference(client, project, expected_state="unavailable")
        if unavailable["review"].get("token"):
            raise AssertionError("missing file cannot produce confirmable token")
        rejected_transition(client, project, doc_changed)
        passed("deleted_target_unavailable_before_reindex")
        index(client, repo, project)
        document = client.tool("get_document", {"project": project, "path": "docs/guide.md"})
        if any(ref["target"]["file_path"] == "src/core.py"
               for ref in document.get("references", [])):
            raise AssertionError("deleted reference survived rebuilt current index")
        passed("deleted_target_relation_absent_after_reindex")

        (repo / "src/core.py").write_text("def compute(value):\n    return value + 2\n",
                                        encoding="utf-8")
        index(client, repo, project)
        restored = reference(client, project, expected_state="pending")
        transition(client, project, restored)
        (repo / "src/core.py").rename(repo / "src/renamed.py")
        reference(client, project, expected_state="unavailable")
        (repo / "docs/guide.md").write_text(
            "# Core Guide\n\nThe implementation is [core](../src/renamed.py).\n",
            encoding="utf-8")
        index(client, repo, project)
        reference(client, project, "src/renamed.py", "pending")
        document = client.tool("get_document", {"project": project, "path": "docs/guide.md"})
        if any(ref["target"]["file_path"] == "src/core.py"
               for ref in document.get("references", [])):
            raise AssertionError("old renamed relation survived current index")
        evidence["observations"]["history_limit"] = (
            "Reindex removes deleted/renamed old relations. This check does not imply "
            "historical reference recovery or rename tracking.")
        passed("rename_new_relation_pending_no_historical_recovery")
        reference(client, projects[1], expected_state="pending")
        passed("other_project_unchanged_after_mutations")
    finally:
        client.close()


def corpus_check(client, source, temp, ordinal):
    repo = temp / f"corpus-{ordinal}"
    evidence = snapshot(source, repo)
    initialize_repo(repo)
    project = f"workflow-corpus-{ordinal}"
    evidence["index"] = index(client, repo, project)
    evidence["coverage"] = client.tool("get_document_coverage", {"project": project})
    assert_coverage(evidence["coverage"])
    evidence["document_coverage"] = client.tool(
        "get_document_coverage", {"project": project, "view": "documents", "limit": 100})
    assert_coverage(evidence["document_coverage"])
    evidence["document_results"] = []
    statuses = {}
    for path in evidence["documents"]:
        result = client.tool("get_document", {"project": project, "path": path})
        analysis = result.get("reference_analysis") or {}
        if analysis.get("status") not in {"ok", "limited", "unknown"}:
            raise AssertionError(f"invalid analysis for {path}: {analysis}")
        evidence["document_results"].append({
            "path": path, "reference_analysis": analysis,
            "reference_count": len(result.get("references", [])),
            "references_truncated": result.get("references_truncated", False)})
        status = analysis["status"]
        statuses[status] = statuses.get(status, 0) + 1
    evidence["observed_analysis_statuses"] = statuses
    for item in evidence["files"]:
        actual = hashlib.sha256((source / item["path"]).read_bytes()).hexdigest()
        if actual != item["sha256"]:
            raise AssertionError(f"source changed during snapshot validation: {item['path']}")
    evidence["source_bytes_unchanged"] = True
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--repo", action="append", type=Path, required=True,
                        help="repeat for at least two distinct real local Git repositories")
    parser.add_argument("--temp-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    roots = list(dict.fromkeys(path.resolve() for path in args.repo))
    if len(roots) < 2:
        parser.error("at least two distinct local repositories are required")
    if args.temp_root:
        args.temp_root.mkdir(parents=True, exist_ok=True)
    report = {"scope": "bounded real local corpora and separate generated mutations",
              "precision_claim": False, "maintainer_review": "pending", "corpora": [],
              "mutations": {}, "failures": [],
              "temp_root": str(args.temp_root.resolve()) if args.temp_root else None,
              "unicode_temp_path": bool(args.temp_root and not str(args.temp_root).isascii())}
    with tempfile.TemporaryDirectory(prefix="cbm-doc-workflow-", dir=args.temp_root) as directory:
        temp = Path(directory)
        client = open_client(args.binary.resolve(), temp / "cache")
        try:
            for ordinal, source in enumerate(roots):
                report["corpora"].append(corpus_check(client, source, temp, ordinal))
        except Exception as exc:
            report["failures"].append(f"{type(exc).__name__}: {exc}")
        finally:
            client.close()
        if not report["failures"]:
            try:
                mutation_check(args.binary.resolve(), temp, report["mutations"])
            except Exception as exc:
                report["failures"].append(f"{type(exc).__name__}: {exc}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps({"corpora": len(report["corpora"]), "failures": report["failures"]}))
    raise SystemExit(bool(report["failures"]))


if __name__ == "__main__":
    main()
