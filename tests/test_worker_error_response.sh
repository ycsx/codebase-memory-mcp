#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${ROOT}/build/c/codebase-memory-mcp"
if [[ ! -x "${BINARY}" && -x "${BINARY}.exe" ]]; then
  BINARY="${BINARY}.exe"
fi
if [[ ! -x "${BINARY}" ]]; then
  echo "missing binary: ${BINARY}" >&2
  exit 2
fi

if command -v shasum >/dev/null 2>&1; then
  BUILD_FINGERPRINT="$(shasum -a 256 "${BINARY}" | awk '{print $1}')"
elif command -v sha256sum >/dev/null 2>&1; then
  BUILD_FINGERPRINT="$(sha256sum "${BINARY}" | awk '{print $1}')"
elif command -v openssl >/dev/null 2>&1; then
  BUILD_FINGERPRINT="$(openssl dgst -sha256 "${BINARY}" | awk '{print $NF}')"
else
  echo "no SHA-256 command available" >&2
  exit 2
fi
if [[ ! "${BUILD_FINGERPRINT}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "invalid worker build fingerprint: ${BUILD_FINGERPRINT}" >&2
  exit 2
fi

tmpdir="$(mktemp -d)"
cleanup() {
  rm -rf "${tmpdir}"
}
trap cleanup EXIT

missing="${tmpdir}/repository-does-not-exist"
response="${tmpdir}/worker.response"
args="{\"repo_path\":\"${missing}\",\"mode\":\"fast\"}"

if ! CBM_CACHE_DIR="${tmpdir}/cache-worker" \
  "${BINARY}" cli --index-worker \
  --index-worker-build "${BUILD_FINGERPRINT}" \
  index_repository "${args}" \
  --response-out "${response}" >"${tmpdir}/worker.out" 2>"${tmpdir}/worker.err"; then
  echo "worker treated a delivered MCP error as a process failure" >&2
  cat "${tmpdir}/worker.err" >&2
  exit 1
fi
if [[ ! -s "${response}" ]] || ! grep -q '"isError":true' "${response}"; then
  echo "worker did not deliver the underlying MCP error" >&2
  cat "${response}" 2>/dev/null >&2 || true
  exit 1
fi

set +e
CBM_CACHE_DIR="${tmpdir}/cache-supervisor" \
  "${BINARY}" cli index_repository --repo-path "${missing}" --mode fast \
  >"${tmpdir}/supervisor.out" 2>"${tmpdir}/supervisor.err"
cli_rc=$?
set -e

if [[ ${cli_rc} -eq 0 ]]; then
  echo "human-facing CLI unexpectedly accepted an indexing error" >&2
  exit 1
fi
if grep -q -e 'crashed on a file' -e 'exit_nonzero' \
  "${tmpdir}/supervisor.out" "${tmpdir}/supervisor.err"; then
  echo "supervisor replaced a valid tool error with a false crash diagnosis" >&2
  exit 1
fi
if ! grep -qi -e 'repo_path' -e 'directory' -e 'resolve' \
  "${tmpdir}/supervisor.out" "${tmpdir}/supervisor.err"; then
  echo "supervisor did not preserve the worker error" >&2
  cat "${tmpdir}/supervisor.out" >&2
  cat "${tmpdir}/supervisor.err" >&2
  exit 1
fi

echo "ok: worker MCP errors remain tool errors, not process crashes"
