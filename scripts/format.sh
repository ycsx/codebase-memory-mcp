#!/usr/bin/env bash
# format.sh - Apply the repository's clang-format rules.
#
# Usage:
#   scripts/format.sh
#   scripts/format.sh CLANG_FORMAT=clang-format-20
#
# This formats the same C sources and headers that scripts/lint.sh checks.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

make -f Makefile.cbm format "$@"

echo "=== Formatting complete ==="
