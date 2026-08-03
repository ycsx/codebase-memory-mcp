#!/usr/bin/env bash
# Optional release phases must not silently skip artifact verification.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKFLOW="$ROOT/.github/workflows/release.yml"
BUILD_WORKFLOW="$ROOT/.github/workflows/_build.yml"
SMOKE_WORKFLOW="$ROOT/.github/workflows/_smoke.yml"
PYTHON_BIN="${PYTHON:-python3}"

"$PYTHON_BIN" - "$WORKFLOW" "$BUILD_WORKFLOW" "$SMOKE_WORKFLOW" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
build_text = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
smoke_text = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
blocks = {}
current = []
name = None
for line in text.splitlines():
    match = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
    if match:
        if name:
            blocks[name] = "\n".join(current)
        name = match.group(1)
        current = []
    elif name is not None:
        current.append(line)
if name:
    blocks[name] = "\n".join(current)


def condition(job):
    body = blocks.get(job, "")
    match = re.search(
        r"^    if:\s*(?:>-|>|\|-|\|)?\s*(.*?)(?=^    [a-z_-]+:|\Z)",
        body,
        re.S | re.M,
    )
    return " ".join(match.group(1).split()) if match else ""


failures = []
for job in ("build", "smoke", "soak", "release-draft"):
    value = condition(job)
    if "!cancelled()" not in value or "!failure()" not in value:
        failures.append(f"{job}: skipped ancestors are not handled safely ({value or 'no if'})")

draft = condition("release-draft")
if "needs.smoke.result == 'success'" not in draft:
    failures.append("release-draft: smoke success is not required")
if "needs.soak.result" not in draft:
    failures.append("release-draft: soak result is not checked")

required_macos_artifacts = (
    "codebase-memory-mcp-darwin-amd64.tar.gz",
    "codebase-memory-mcp-darwin-arm64.tar.gz",
    "codebase-memory-mcp-ui-darwin-amd64.tar.gz",
    "codebase-memory-mcp-ui-darwin-arm64.tar.gz",
)
if "Require macOS release artifacts" not in text:
    failures.append("release-draft: macOS artifact completeness gate is missing")
for artifact in required_macos_artifacts:
    if artifact not in text:
        failures.append(f"release-draft: required macOS artifact is not checked ({artifact})")

for architecture in ("amd64", "arm64"):
    pattern = rf"goos:\s*darwin\s*\n\s*goarch:\s*{architecture}\b"
    if not re.search(pattern, build_text):
        failures.append(f"build: blocking darwin-{architecture} matrix leg is missing")

homebrew_identity = (
    'git config --global user.name "Codebase Memory CI"',
    'git config --global user.email "ci@codebase-memory-mcp.invalid"',
)
tap_new_offset = smoke_text.find('brew tap-new "$TEST_TAP"')
if tap_new_offset < 0:
    failures.append("smoke: Homebrew tap creation is missing")
for command in homebrew_identity:
    command_offset = smoke_text.find(command)
    if command_offset < 0 or (tap_new_offset >= 0 and command_offset > tap_new_offset):
        failures.append(f"smoke: Git identity must be configured before tap creation ({command})")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("PASS: release gates, macOS artifacts, and package smoke contracts are enforced")
PY
