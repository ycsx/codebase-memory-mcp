#!/usr/bin/env python3
"""Audit and record selective synchronization from the configured upstream.

This tool deliberately separates discovery from integration. It never merges or
cherry-picks source code. The scheduled workflow uses ``report``; maintainers use
``record`` after a reviewed local change has landed.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / ".github" / "upstream-sync.json"


def run_git(*args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"git {' '.join(shlex.quote(a) for a in args)} failed: {detail}")
    return result.stdout.strip()


def load_config() -> dict[str, Any]:
    return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))


def state_path(config: dict[str, Any]) -> Path:
    return ROOT / str(config["state_file"])


def log_path(config: dict[str, Any]) -> Path:
    return ROOT / str(config["log_file"])


def load_state(config: dict[str, Any]) -> dict[str, Any]:
    path = state_path(config)
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_state(config: dict[str, Any], state: dict[str, Any]) -> None:
    path = state_path(config)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def fetch_source(config: dict[str, Any]) -> str:
    """Fetch the configured ref into FETCH_HEAD without requiring an SSH key."""
    run_git("fetch", "--no-tags", str(config["upstream_url"]), str(config["upstream_ref"]))
    return "FETCH_HEAD"


def resolve_source_ref(config: dict[str, Any], source_ref: str | None, fetch: bool) -> tuple[str, str]:
    if fetch:
        return fetch_source(config), str(config["upstream_ref"])
    requested = source_ref or f"{config['upstream_remote']}/{config['upstream_ref']}"
    return requested, str(config["upstream_ref"])


def commit(ref: str) -> str:
    return run_git("rev-parse", f"{ref}^{{commit}}")


def commit_exists(value: str) -> bool:
    result = subprocess.run(
        ["git", "cat-file", "-e", f"{value}^{{commit}}"], cwd=ROOT, check=False
    )
    return result.returncode == 0


def count_relation(target: str, source: str) -> dict[str, Any]:
    base = run_git("merge-base", target, source, check=False)
    if not base:
        return {
            "history_relation": "unrelated",
            "merge_base": None,
            "target_only": int(run_git("rev-list", "--count", target)),
            "source_only": int(run_git("rev-list", "--count", source)),
        }
    left, right = run_git("rev-list", "--left-right", "--count", f"{target}...{source}").split()
    return {
        "history_relation": "related",
        "merge_base": base,
        "target_only": int(left),
        "source_only": int(right),
    }


def source_commits(source: str, state: dict[str, Any], limit: int) -> list[str]:
    previous = state.get("last_integrated_upstream_commit")
    if previous and commit_exists(str(previous)):
        base = run_git("merge-base", str(previous), source, check=False)
        if base and base == str(previous):
            return run_git(
                "log", "--reverse", f"{previous}..{source}", f"--max-count={limit}",
                "--format=%H%x09%ad%x09%s", "--date=short",
            ).splitlines()
    return run_git(
        "log", "--max-count", str(limit), "--format=%H%x09%ad%x09%s", "--date=short", source
    ).splitlines()


def changed_paths(target: str, source: str, limit: int) -> list[str]:
    lines = run_git("diff", "--no-renames", "--name-status", target, source).splitlines()
    return lines[:limit]


def report_text(config: dict[str, Any], target: str, source: str, source_label: str,
                state: dict[str, Any], limit: int) -> tuple[str, dict[str, Any]]:
    source_sha = commit(source)
    target_sha = commit(target)
    relation = count_relation(target, source)
    commits = source_commits(source, state, limit)
    paths = changed_paths(target, source, limit)
    protected = [
        path for path in paths
        if any(path.split("\t", 1)[-1].startswith(prefix) for prefix in config["protected_paths"])
    ]
    candidate_paths = [path for path in paths if path not in protected]
    policy = config.get("policy", {})
    previous_seen = state.get("last_seen_commit")
    if previous_seen == source_sha and state.get("last_checked_at"):
        now = str(state["last_checked_at"])
    else:
        now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    lines = [
        "# Upstream Sync Candidate",
        "",
        f"- Checked at: `{now}`",
        f"- Source: `{config['upstream_url']}` (`{source_label}`)",
        f"- Upstream commit: `{source_sha}`",
        f"- Target commit: `{target_sha}`",
        f"- History relation: `{relation['history_relation']}`",
        f"- Merge base: `{relation['merge_base'] or 'none'}`",
        f"- Target-only commits: `{relation['target_only']}`",
        f"- Source-only commits: `{relation['source_only']}`",
        f"- Last integrated upstream commit: `{state.get('last_integrated_upstream_commit') or 'none'}`",
        f"- Sync policy: `{policy.get('mode', 'selective-transplant')}`",
        "",
        "> This is a review report. No upstream source code was merged by the audit. "
        "The local feature set remains the baseline.",
        "",
        "## Source commits",
        "",
    ]
    if commits:
        lines.extend(f"- `{line.split(chr(9), 1)[0]}` {line.split(chr(9), 1)[-1]}" for line in commits)
    else:
        lines.append("- No source commits were found for the recorded range.")
    lines.extend(["", "## Tree differences", ""])
    if paths:
        lines.extend(f"- `{line}`" for line in paths)
    else:
        lines.append("- No changed paths.")
    lines.extend(["", "## Protected paths touched", ""])
    if protected:
        lines.extend(f"- `{line}`" for line in protected)
    else:
        lines.append("- None detected.")
    lines.extend(["", "## Candidate upstream paths", ""])
    if candidate_paths:
        lines.extend(f"- `{line}`" for line in candidate_paths)
    else:
        lines.append("- No unprotected paths detected; review the protected paths at symbol/hunk level.")
    lines.extend([
        "",
        "## Next step",
        "",
        "Treat the current target tree as authoritative for existing behavior. Review the listed "
        "commits and paths, port only compatible upstream algorithms or features, preserve all local "
        "features, run the relevant checks, then use `scripts/upstream_sync.py record` with full "
        "source and target SHAs.",
        "",
    ])
    new_state = dict(state)
    new_state.setdefault("schema_version", 1)
    new_state["upstream_url"] = config["upstream_url"]
    new_state["upstream_ref"] = config["upstream_ref"]
    new_state["last_seen_commit"] = source_sha
    new_state["last_checked_at"] = now
    return "\n".join(lines), new_state


def command_status(args: argparse.Namespace) -> int:
    config = load_config()
    source_ref, source_label = resolve_source_ref(config, args.source_ref, args.fetch)
    source = commit(source_ref)
    target = commit(args.target)
    state = load_state(config)
    relation = count_relation(target, source)
    print(json.dumps({
        "upstream_url": config["upstream_url"],
        "upstream_ref": source_label,
        "upstream_commit": source,
        "target_commit": target,
        **relation,
        "last_seen_commit": state.get("last_seen_commit"),
        "last_integrated_upstream_commit": state.get("last_integrated_upstream_commit"),
    }, ensure_ascii=False, indent=2))
    return 0


def command_report(args: argparse.Namespace) -> int:
    config = load_config()
    source_ref, source_label = resolve_source_ref(config, args.source_ref, args.fetch)
    state = load_state(config)
    report, new_state = report_text(config, args.target, source_ref, source_label, state, args.limit)
    output = ROOT / (args.output or config["report_file"])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(report, encoding="utf-8")
    if args.write_state:
        save_state(config, new_state)
    print(f"wrote {output.relative_to(ROOT)}")
    return 0


def append_log(config: dict[str, Any], row: str) -> None:
    path = log_path(config)
    if not path.exists():
        path.write_text(
            "# Upstream Sync Log\n\n## Entries\n\n"
            "| UTC date | Upstream commit | Target commit | Status | Method | Scope | Checks | Review / notes |\n"
            "| --- | --- | --- | --- | --- | --- | --- | --- |\n",
            encoding="utf-8",
        )
    content = path.read_text(encoding="utf-8")
    if row in content:
        raise RuntimeError("this upstream sync record already exists")
    path.write_text(content.rstrip() + "\n" + row + "\n", encoding="utf-8")


def command_record(args: argparse.Namespace) -> int:
    config = load_config()
    source_sha = commit(args.source_commit)
    target_sha = commit(args.target_commit)
    policy = config.get("policy", {})
    if args.status == "integrated":
        if not commit_exists(target_sha):
            raise RuntimeError(f"target commit is not present: {target_sha}")
        if policy.get("require_checks_before_record", False) and not args.checks:
            raise RuntimeError("integrated records require --checks to preserve verification evidence")
    now = datetime.now(timezone.utc).date().isoformat()
    scope = (args.scope or "unspecified").replace("|", "/").replace("\n", " ")
    checks = (args.checks or "not recorded").replace("|", "/").replace("\n", " ")
    notes = (args.notes or "").replace("|", "/").replace("\n", " ")
    method = args.method.replace("|", "/")
    status = args.status
    review = notes
    if args.pr_url:
        review = f"{args.pr_url} {review}".strip()
    row = f"| {now} | `{source_sha}` | `{target_sha}` | {status} | {method} | {scope} | {checks} | {review} |"
    existing_log = log_path(config).read_text(encoding="utf-8") if log_path(config).exists() else ""
    if f"`{source_sha}` | `{target_sha}`" in existing_log:
        raise RuntimeError("this upstream source and target commit pair is already recorded")
    append_log(config, row)
    state = load_state(config)
    state["schema_version"] = 1
    state["upstream_url"] = config["upstream_url"]
    state["upstream_ref"] = config["upstream_ref"]
    state["last_seen_commit"] = state.get("last_seen_commit") or source_sha
    if status == "integrated":
        state["last_integrated_upstream_commit"] = source_sha
        state["last_integrated_target_commit"] = target_sha
        state["last_integrated_at"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    save_state(config, state)
    print(f"recorded {status}: upstream={source_sha} target={target_sha}")
    return 0


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--source-ref", help="local source ref, defaults to upstream/main")
    common.add_argument("--target", default="HEAD", help="target ref, default HEAD")
    common.add_argument("--fetch", action="store_true", help="fetch configured upstream over HTTPS")
    status = sub.add_parser("status", parents=[common], help="print machine-readable divergence status")
    status.set_defaults(func=command_status)
    report = sub.add_parser("report", parents=[common], help="write the candidate report")
    report.add_argument("--output", help="report path relative to the repository root")
    report.add_argument("--write-state", action="store_true", help="update last_seen_commit")
    report.add_argument("--limit", type=int, default=50, help="maximum commits and paths in report")
    report.set_defaults(func=command_report)
    record = sub.add_parser("record", help="record a reviewed integration")
    record.add_argument("--source-commit", required=True)
    record.add_argument("--target-commit", default="HEAD")
    record.add_argument("--status", choices=["integrated", "partial", "candidate", "rejected"], required=True)
    record.add_argument("--method", required=True, help="cherry-pick, patch, merge, or other reviewed method")
    record.add_argument("--scope", help="paths or feature scope")
    record.add_argument("--checks", help="checks run before recording an integrated change")
    record.add_argument("--pr-url")
    record.add_argument("--notes")
    record.set_defaults(func=command_record)
    return p


def main() -> int:
    try:
        args = parser().parse_args()
        return int(args.func(args))
    except RuntimeError as exc:
        print(f"upstream-sync: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
