#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/scripts/ci/check-virustotal.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FIXTURE="$TMP/release-binary"
printf '\x7fELFmock-release-binary\x00' > "$FIXTURE"

mkdir -p "$TMP/bin"
cat > "$TMP/bin/curl" <<'MOCK_CURL'
#!/usr/bin/env bash
set -euo pipefail

url="${!#}"
echo "$url" >> "$MOCK_LOG"

case "$MOCK_SCENARIO:$url" in
  existing-clean:*'/analyses/'*|existing-malicious:*'/analyses/'*)
    printf '%s' '{"data":{"attributes":{"status":"queued","stats":{}}}}'
    ;;
  existing-clean:*'/files/'*)
    printf '%s' '{"data":{"attributes":{"last_analysis_stats":{"malicious":0,"suspicious":0,"undetected":60,"harmless":2}}}}'
    ;;
  existing-malicious:*'/files/'*)
    printf '%s' '{"data":{"attributes":{"last_analysis_stats":{"malicious":1,"suspicious":0,"undetected":60,"harmless":1}}}}'
    ;;
  queued-complete:*'/files/'*|timeout:*'/files/'*|under-minimum:*'/files/'*)
    exit 22
    ;;
  queued-complete:*'/analyses/'*)
    count=0
    [ -f "$MOCK_STATE" ] && count=$(cat "$MOCK_STATE")
    count=$((count + 1))
    echo "$count" > "$MOCK_STATE"
    if [ "$count" -eq 1 ]; then
      printf '%s' '{"data":{"attributes":{"status":"queued","stats":{}}}}'
    else
      printf '%s' '{"data":{"attributes":{"status":"completed","stats":{"malicious":0,"suspicious":0,"undetected":60,"harmless":2}}}}'
    fi
    ;;
  timeout:*'/analyses/'*)
    printf '%s' '{"data":{"attributes":{"status":"queued","stats":{}}}}'
    ;;
  under-minimum:*'/analyses/'*)
    printf '%s' '{"data":{"attributes":{"status":"completed","stats":{"malicious":0,"suspicious":0,"undetected":59,"harmless":0}}}}'
    ;;
  *)
    echo "unexpected mock request: $url" >&2
    exit 2
    ;;
esac
MOCK_CURL
chmod +x "$TMP/bin/curl"

PASS=0
FAIL=0

run_gate() {
  local scenario="$1"
  local output="$2"
  : > "$TMP/curl.log"
  rm -f "$TMP/state"
  PATH="$TMP/bin:$PATH" \
    MOCK_SCENARIO="$scenario" \
    MOCK_LOG="$TMP/curl.log" \
    MOCK_STATE="$TMP/state" \
    VT_API_KEY="test-key" \
    VT_ANALYSIS="$FIXTURE=https://www.virustotal.com/gui/file-analysis/test-analysis-id/detection" \
    VT_MAX_ATTEMPTS=2 \
    VT_POLL_SECONDS=0 \
    VT_PYTHON_BIN="${VT_TEST_PYTHON_BIN:-python3}" \
    bash "$SCRIPT" > "$output" 2>&1
}

if run_gate existing-clean "$TMP/existing-clean.out" &&
   grep -q "clean via existing SHA-256 report" "$TMP/existing-clean.out" &&
   [ "$(grep -c '/analyses/' "$TMP/curl.log")" -eq 1 ]; then
  echo "PASS: an existing clean SHA-256 report bypasses a redundant queued analysis"
  PASS=$((PASS + 1))
else
  echo "FAIL: existing clean report was not accepted"
  cat "$TMP/existing-clean.out"
  FAIL=$((FAIL + 1))
fi

if run_gate queued-complete "$TMP/queued-complete.out" &&
   grep -q "clean (62 engines" "$TMP/queued-complete.out"; then
  echo "PASS: a newly queued analysis is polled until completion"
  PASS=$((PASS + 1))
else
  echo "FAIL: queued analysis did not complete through polling"
  cat "$TMP/queued-complete.out"
  FAIL=$((FAIL + 1))
fi

if run_gate existing-malicious "$TMP/existing-malicious.out"; then
  echo "FAIL: an existing malicious report was accepted"
  FAIL=$((FAIL + 1))
elif grep -q "existing SHA-256 report flagged" "$TMP/existing-malicious.out"; then
  echo "PASS: an existing malicious SHA-256 report is blocked"
  PASS=$((PASS + 1))
else
  echo "FAIL: malicious report failed for the wrong reason"
  cat "$TMP/existing-malicious.out"
  FAIL=$((FAIL + 1))
fi

if run_gate under-minimum "$TMP/under-minimum.out"; then
  echo "FAIL: a report below the minimum engine count was accepted"
  FAIL=$((FAIL + 1))
elif grep -q "only 59/59 engines" "$TMP/under-minimum.out"; then
  echo "PASS: completed reports below the engine floor are blocked"
  PASS=$((PASS + 1))
else
  echo "FAIL: low-engine report failed for the wrong reason"
  cat "$TMP/under-minimum.out"
  FAIL=$((FAIL + 1))
fi

if run_gate timeout "$TMP/timeout.out"; then
  echo "FAIL: a permanently queued analysis was accepted"
  FAIL=$((FAIL + 1))
elif grep -q "did not complete after 2 attempts" "$TMP/timeout.out"; then
  echo "PASS: a permanently queued analysis stops at the configured limit"
  PASS=$((PASS + 1))
else
  echo "FAIL: queued analysis failed for the wrong reason"
  cat "$TMP/timeout.out"
  FAIL=$((FAIL + 1))
fi

echo "=== VirusTotal polling test: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
