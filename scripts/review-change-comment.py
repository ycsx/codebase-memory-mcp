#!/usr/bin/env python3
"""Render review_change as a deduplicated GitHub/GitLab pull-request comment.

The adapter deliberately uses only the Python standard library. It can be run
in CI without installing another action or package, and it never blocks a
merge unless --fail-on-block is explicitly supplied.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import fnmatch
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen


def api_json(method: str, url: str, token: str, payload: dict[str, Any] | None = None) -> Any:
    body = json.dumps(payload).encode("utf-8") if payload is not None else None
    request = Request(url, data=body, method=method)
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("Content-Type", "application/json")
    request.add_header("Authorization", f"Bearer {token}")
    request.add_header("PRIVATE-TOKEN", token)
    with urlopen(request, timeout=30) as response:
        raw = response.read().decode("utf-8")
    return json.loads(raw) if raw else None


def run_review(args: argparse.Namespace) -> dict[str, Any]:
    command = [
        args.binary,
        "cli",
        "--json",
        "review_change",
        "--project",
        args.project,
        "--since",
        args.since,
        "--depth",
        str(args.depth),
        "--token-budget",
        str(args.token_budget),
        "--evidence-level",
        args.evidence_level,
    ]
    if args.include_docs:
        command.append("--include-docs")
    if args.include_tests:
        command.append("--include-tests")
    completed = subprocess.run(
        command,
        cwd=args.root,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or f"review_change exited with {completed.returncode}"
        raise RuntimeError(detail)
    if not completed.stdout.strip():
        detail = completed.stderr.strip() or f"review_change exited with {completed.returncode}"
        raise RuntimeError(detail)
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"review_change returned invalid JSON: {exc}") from exc
    if not isinstance(result, dict):
        raise RuntimeError("review_change returned a non-object JSON value")
    if result.get("isError"):
        raise RuntimeError("review_change returned an MCP error")
    # --json intentionally exposes the MCP envelope. Unwrap the text payload
    # so the adapter also works with older binaries that do not emit
    # structuredContent for composite tools.
    if isinstance(result.get("structuredContent"), dict):
        result = result["structuredContent"]
    elif isinstance(result.get("content"), list):
        text_items = [
            item.get("text")
            for item in result["content"]
            if isinstance(item, dict) and isinstance(item.get("text"), str)
        ]
        if text_items:
            try:
                nested = json.loads(text_items[0])
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"review_change text payload is invalid JSON: {exc}") from exc
            if isinstance(nested, dict):
                result = nested
    if result.get("status") not in ("pass", "warn", "block", "unknown"):
        raise RuntimeError("review_change returned no valid review status")
    return result


def read_codeowners(root: Path) -> tuple[Path | None, list[tuple[str, list[str]]]]:
    for relative in (Path(".github/CODEOWNERS"), Path("CODEOWNERS"), Path("docs/CODEOWNERS")):
        candidate = root / relative
        if not candidate.is_file():
            continue
        rules: list[tuple[str, list[str]]] = []
        for raw_line in candidate.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 2 or fields[0].startswith("!"):
                continue
            rules.append((fields[0], fields[1:]))
        return candidate, rules
    return None, []


def codeowners_for(path: str, rules: list[tuple[str, list[str]]]) -> list[str]:
    normalized = path.replace("\\", "/").lstrip("/")
    owners: list[str] = []
    for pattern, candidates in rules:
        pattern = pattern.replace("\\", "/")
        directory = pattern.endswith("/")
        pattern = pattern.lstrip("/")
        if directory:
            pattern += "**"
        matched = fnmatch.fnmatchcase(normalized, pattern)
        if "/" not in pattern and not matched:
            matched = fnmatch.fnmatchcase(normalized.rsplit("/", 1)[-1], pattern)
        if matched:
            owners = list(dict.fromkeys(candidates))
    return owners


def commit_id(root: Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def record_rule_actions(result: dict[str, Any], args: argparse.Namespace, root: Path) -> None:
    if not args.rule_action:
        return
    actions: list[tuple[str, str]] = []
    for raw in args.rule_action:
        rule_id, separator, action = raw.partition("=")
        if not separator or not rule_id or action not in ("confirm", "ignore"):
            raise ValueError(f"invalid --rule-action {raw!r}; use RULE=confirm or RULE=ignore")
        actions.append((rule_id, action))
    telemetry = Path(args.telemetry_file) if args.telemetry_file else root / ".codebase-memory" / "review-telemetry.jsonl"
    telemetry.parent.mkdir(parents=True, exist_ok=True)
    record = {
        "tool": "review_change",
        "project": args.project,
        "commit": commit_id(root),
        "status": result.get("status", "unknown"),
        "recorded_at": datetime.now(timezone.utc).isoformat(),
    }
    with telemetry.open("a", encoding="utf-8") as stream:
        for rule_id, action in actions:
            stream.write(json.dumps({**record, "rule": rule_id, "action": action}, ensure_ascii=True) + "\n")


def markdown_text(value: Any, limit: int = 600) -> str:
    text = " ".join(str(value).split())
    if len(text) > limit:
        text = text[:limit] + " [truncated]"
    text = html.escape(text, quote=True)
    return re.sub(r"([\\`*_{}\[\]()#+.!|~:\-])", r"\\\1", text)


def evidence_code(value: Any) -> str:
    text = " ".join(str(value).split())
    if len(text) > 300:
        text = text[:300] + " [truncated]"
    fence = "`" * (max((len(match) for match in re.findall(r"`+", text)), default=0) + 1)
    return f"{fence} {text} {fence}"


def rule_evidence(result: dict[str, Any], rule: dict[str, Any]) -> list[str]:
    references = rule.get("evidence")
    if not isinstance(references, list) or not references:
        return ["Evidence unavailable: no source or graph reference provided."]
    symbols = result.get("changed_symbols")
    symbols = symbols if isinstance(symbols, list) else []
    impacts = result.get("impacts")
    impacts = impacts if isinstance(impacts, list) else []
    files = result.get("changed_files")
    files = files if isinstance(files, list) else []
    entries: list[str] = []

    def source(node: dict[str, Any]) -> str:
        path = node.get("file_path")
        if not isinstance(path, str) or not path:
            return "source location unavailable"
        start, end = node.get("start_line"), node.get("end_line")
        location = path
        if type(start) is int and start > 0:
            location += f":{start}"
            if type(end) is int and end >= start:
                location += f"-{end}"
        else:
            location += " (line unavailable)"
        return evidence_code(location)

    for reference in references[:6]:
        if not isinstance(reference, str):
            entries.append("Evidence unavailable: malformed reference.")
        elif reference in ("changed_symbols", "impacts", "summary.direct_impacts"):
            nodes = symbols if reference == "changed_symbols" else impacts
            if reference == "summary.direct_impacts":
                summary = result.get("summary")
                summary = summary if isinstance(summary, dict) else {}
                entries.append("Impact counts: direct " + evidence_code(summary.get("direct_impacts", "unknown"))
                               + ", indirect " + evidence_code(summary.get("indirect_impacts", "unknown")))
            for node in nodes[:6]:
                if not isinstance(node, dict):
                    entries.append("Evidence unavailable: malformed graph node.")
                    continue
                name = evidence_code(node.get("qualified_name") or node.get("name") or "unknown")
                if reference == "changed_symbols":
                    entries.append(f"Changed symbol: {name}; source {source(node)}")
                else:
                    changed_by = evidence_code(node.get("changed_by", "unknown"))
                    hop = evidence_code(node.get("hop", "unknown"))
                    entries.append(f"Inbound reachability: {changed_by} &lt;- {name}; "
                                   f"hop {hop}; source {source(node)}; intermediate path not provided")
            if not nodes:
                entries.append("Evidence unavailable: " + evidence_code(reference) + " has no source nodes.")
            if len(nodes) > 6:
                entries.append("Evidence truncated: additional graph nodes omitted.")
        elif reference in ("analysis_meta.freshness", "analysis_meta.coverage"):
            metadata = result.get("analysis_meta")
            value = metadata.get(reference.split(".")[1]) if isinstance(metadata, dict) else None
            if value is None:
                entries.append("Evidence unavailable: " + evidence_code(reference))
            else:
                entries.append("Metadata " + evidence_code(reference) + ": "
                               + evidence_code(json.dumps(value, ensure_ascii=True, sort_keys=True))
                               + "; no source location supplied")
        elif reference == "changed_files":
            entries.extend("Changed file: " + evidence_code(path) for path in files[:6] if isinstance(path, str))
            if not files:
                entries.append("Evidence unavailable: changed_files is empty.")
            if len(files) > 6:
                entries.append("Evidence truncated: additional changed files omitted.")
        elif reference in files:
            entries.append("Changed file: " + evidence_code(reference))
        else:
            entries.append("Unresolved evidence reference: " + evidence_code(reference)
                           + "; no source or graph location supplied")
        if len(entries) > 5:
            break
    if len(entries) > 5 or len(references) > 6:
        entries = entries[:5] + ["Evidence truncated: additional entries omitted."]
    return entries


def markdown(result: dict[str, Any], project: str, since: str, root: Path) -> str:
    revision = commit_id(root)
    marker_project = re.sub(r"[\x00-\x1f<>]", lambda match: quote(match.group(0), safe=""), project)
    marker = f"<!-- cbm-review:{marker_project}:{quote(revision, safe='')} -->"
    status = markdown_text(result.get("status", "unknown"))
    risk = markdown_text(result.get("risk_label_zh", result.get("risk", "unknown")))
    summary = result.get("summary") if isinstance(result.get("summary"), dict) else {}
    changed_files = result.get("changed_files") if isinstance(result.get("changed_files"), list) else []
    codeowners_file, owner_rules = read_codeowners(root)
    lines = [
        marker,
        "## Codebase Memory change review",
        "",
        f"- Status: **{status}**",
        f"- Risk: **{risk}**",
        f"- Project: {evidence_code(project)}",
        f"- Compared ref: {evidence_code(since)}",
        f"- Commit: {evidence_code(revision)}",
        "",
        markdown_text(result.get("summary_zh", "No summary available.")),
        "",
        (
            "Impact: "
            f"{markdown_text(summary.get('direct_impacts', 0))} direct, "
            f"{markdown_text(summary.get('indirect_impacts', 0))} indirect, "
            f"{markdown_text(summary.get('entry_points', 0))} entry points"
        ),
    ]
    if codeowners_file and changed_files:
        lines.extend(["", "### Owners"])
        for changed in changed_files[:20]:
            owners = codeowners_for(str(changed), owner_rules)
            owner_text = " ".join(owners) if owners else "(no matching CODEOWNERS rule)"
            lines.append(f"- {evidence_code(changed)}: {markdown_text(owner_text)}")
        if len(changed_files) > 20:
            lines.append("Owners truncated: additional changed files omitted.")
        lines.append(f"\n_Source: {evidence_code(codeowners_file.relative_to(root).as_posix())}_")
    rules = result.get("rules") if isinstance(result.get("rules"), list) else []
    if rules:
        lines.extend(["", "### Rules"])
        for rule in rules[:20]:
            if not isinstance(rule, dict):
                continue
            rule_id = rule.get("id", "unknown")
            rule_status = rule.get("status", "unknown")
            message = rule.get("message", "")
            lines.append(f"- {evidence_code(rule_id)} **{markdown_text(rule_status)}**: {markdown_text(message)}")
            if rule.get("evidence") or rule_status != "pass":
                lines.extend(f"  - {entry}" for entry in rule_evidence(result, rule))
        if len(rules) > 20:
            lines.append("Rules truncated: additional rules omitted.")
    limitations = result.get("limitations") if isinstance(result.get("limitations"), list) else []
    if limitations:
        lines.extend(["", "<details><summary>Limitations</summary>", ""])
        lines.extend(f"- {markdown_text(item)}" for item in limitations[:10])
        if len(limitations) > 10:
            lines.append("Limitations truncated: additional entries omitted.")
        lines.extend(["", "</details>"])
    lines.append("")
    body = "\n".join(lines)
    if len(body) > 50000:
        body = body[:49000].rsplit("\n", 1)[0]
        if body.count("<details>") > body.count("</details>"):
            body += "\n</details>"
        body += "\n\nComment truncated: additional content omitted.\n"
    return body


def find_comment(endpoint: str, body: str, token: str) -> dict[str, Any] | None:
    marker = body.splitlines()[0]
    page = 1
    while True:
        comments = api_json("GET", f"{endpoint}?per_page=100&page={page}", token)
        if not isinstance(comments, list):
            raise RuntimeError("comment API returned a non-list response")
        existing = next(
            (item for item in comments if isinstance(item, dict)
             and marker in (item.get("body") or "")),
            None,
        )
        if existing:
            return existing
        if len(comments) < 100:
            return None
        page += 1


def github_comment(body: str, token: str, repository: str, number: str, api_url: str) -> None:
    base = api_url.rstrip("/")
    endpoint = f"{base}/repos/{quote(repository, safe='/')}/issues/{quote(number)}/comments"
    existing = find_comment(endpoint, body, token)
    if existing:
        api_json("PATCH", f"{base}/repos/{quote(repository, safe='/')}/issues/comments/{existing['id']}", token, {"body": body})
    else:
        api_json("POST", endpoint, token, {"body": body})


def gitlab_comment(body: str, token: str, project: str, number: str, api_url: str) -> None:
    base = api_url.rstrip("/")
    if not base.endswith("/api/v4"):
        base += "/api/v4"
    encoded_project = quote(project, safe="")
    endpoint = f"{base}/projects/{encoded_project}/merge_requests/{quote(number)}/notes"
    existing = find_comment(endpoint, body, token)
    if existing:
        api_json("PUT", f"{endpoint}/{existing['id']}", token, {"body": body})
    else:
        api_json("POST", endpoint, token, {"body": body})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="codebase-memory-mcp")
    parser.add_argument("--root", default=".")
    parser.add_argument("--project", required=True)
    parser.add_argument("--since", default="HEAD")
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--token-budget", type=int, default=4000)
    parser.add_argument("--evidence-level", choices=("scout", "analysis", "audit"), default="analysis")
    parser.add_argument("--include-docs", action="store_true", default=True)
    parser.add_argument("--include-tests", action="store_true", default=True)
    parser.add_argument("--platform", choices=("github", "gitlab"), default="auto")
    parser.add_argument("--number", help="PR/MR number; defaults to CI environment")
    parser.add_argument("--repository", help="GitHub repository or GitLab project path")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--fail-on-block", action="store_true")
    parser.add_argument("--rule-action", action="append", help="Record RULE=confirm or RULE=ignore")
    parser.add_argument("--telemetry-file", help="JSONL path for rule action telemetry")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.root = str(Path(args.root).resolve())
    try:
        result = run_review(args)
        record_rule_actions(result, args, Path(args.root))
        body = markdown(result, args.project, args.since, Path(args.root))
        if args.dry_run:
            print(body)
        else:
            platform = args.platform
            if platform == "auto":
                platform = "github" if os.getenv("GITHUB_REPOSITORY") else "gitlab"
            number = args.number or os.getenv("GITHUB_EVENT_NUMBER") or os.getenv("CI_MERGE_REQUEST_IID")
            repository = args.repository or os.getenv("GITHUB_REPOSITORY") or os.getenv("CI_PROJECT_PATH")
            token = os.getenv("GITHUB_TOKEN") if platform == "github" else os.getenv("GITLAB_TOKEN")
            if not number or not repository or not token:
                raise RuntimeError("comment mode requires repository, number, and platform token")
            if platform == "github":
                github_comment(body, token, repository, number, os.getenv("GITHUB_API_URL", "https://api.github.com"))
            else:
                gitlab_comment(body, token, repository, number, os.getenv("CI_API_V4_URL", "https://gitlab.com"))
        return 2 if args.fail_on_block and result.get("status") == "block" else 0
    except (OSError, HTTPError, RuntimeError, ValueError) as exc:
        print(f"review-change-comment: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
