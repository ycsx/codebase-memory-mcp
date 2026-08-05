#!/usr/bin/env bash
# Wait for VirusTotal scans to complete and check results.
# Expects: VT_API_KEY, VT_ANALYSIS (comma-separated "file=URL" pairs)
set -euo pipefail

MIN_ENGINES="${VT_MIN_ENGINES:-60}"
MAX_ATTEMPTS="${VT_MAX_ATTEMPTS:-30}"
POLL_SECONDS="${VT_POLL_SECONDS:-16}"
PYTHON_BIN="${VT_PYTHON_BIN:-python3}"
FAIL_FILE=$(mktemp)
trap 'rm -f "$FAIL_FILE"' EXIT

parse_stats() {
  local kind="$1"
  "$PYTHON_BIN" -c '
import json, sys

kind = sys.argv[1]
data = json.loads(sys.stdin.read())
attrs = data.get("data", {}).get("attributes", {})
if kind == "analysis":
    status = attrs.get("status", "queued")
    stats = attrs.get("stats", {})
else:
    stats = attrs.get("last_analysis_stats", {})
    status = "completed" if stats else "unknown"

malicious = stats.get("malicious", 0)
suspicious = stats.get("suspicious", 0)
completed = sum(stats.get(key, 0) for key in (
    "malicious", "suspicious", "undetected", "harmless"
))
total = sum(stats.values())
print(f"{status},{malicious},{suspicious},{completed},{total}")
' "$kind" 2>/dev/null
}

fetch_vt() {
  local endpoint="$1"
  # Throttle every API call, including SHA-256 fallbacks, so the whole script
  # stays below the four-requests-per-minute public API limit.
  if [ "$POLL_SECONDS" != "0" ]; then
    sleep "$POLL_SECONDS"
  fi
  curl -sf --max-time 10 \
    -H "x-apikey: $VT_API_KEY" \
    "https://www.virustotal.com/api/v3/$endpoint" 2>/dev/null || true
}

record_failure() {
  echo "FAIL" >> "$FAIL_FILE"
}

echo "=== Waiting for VirusTotal scans to fully complete ==="

echo "$VT_ANALYSIS" | tr ',' '\n' | while IFS= read -r entry; do
  [ -z "$entry" ] && continue
  FILE=$(echo "$entry" | cut -d'=' -f1)
  URL=$(echo "$entry" | cut -d'=' -f2-)
  BASENAME=$(basename "$FILE")

  # Extract base64 analysis ID from URL
  ANALYSIS_ID=$(echo "$URL" | sed -n 's|.*/file-analysis/\([^/]*\)/.*|\1|p')
  if [ -z "$ANALYSIS_ID" ]; then
    ANALYSIS_ID=$(echo "$URL" | grep -oE '[a-f0-9]{64}')
    if [ -z "$ANALYSIS_ID" ]; then
      echo "BLOCKED: Cannot parse VirusTotal URL: $URL"
      record_failure
      continue
    fi
  fi

  SCAN_COMPLETE=false
  SHA256=""
  if [ -f "$FILE" ]; then
    SHA256=$(sha256sum "$FILE" 2>/dev/null | awk '{print $1}')
  fi
  REPORT_CHECKED=false

  # The default interval stays below four API requests per minute. A single
  # stuck analysis has a bounded eight-minute wait instead of occupying a
  # runner for hours.
  for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
    RESULT=$(fetch_vt "analyses/$ANALYSIS_ID")

    if [ -z "$RESULT" ]; then
      echo "  $BASENAME: API unavailable or rate-limited (attempt $attempt/$MAX_ATTEMPTS)..."
      continue
    fi

    STATS=$(echo "$RESULT" | parse_stats analysis || echo "queued,0,0,0,0")

    STATUS=$(echo "$STATS" | cut -d',' -f1)
    MALICIOUS=$(echo "$STATS" | cut -d',' -f2)
    SUSPICIOUS=$(echo "$STATS" | cut -d',' -f3)
    COMPLETED=$(echo "$STATS" | cut -d',' -f4)
    TOTAL=$(echo "$STATS" | cut -d',' -f5)

    if [ "$STATUS" = "completed" ]; then
      SCAN_COMPLETE=true
      if [ "$MALICIOUS" -gt 0 ] || [ "$SUSPICIOUS" -gt 0 ]; then
        echo "BLOCKED: $BASENAME flagged ($MALICIOUS malicious, $SUSPICIOUS suspicious / $COMPLETED engines)"
        echo "  $URL"
        record_failure
      elif [ "$COMPLETED" -lt "$MIN_ENGINES" ]; then
        echo "BLOCKED: $BASENAME completed with only $COMPLETED/$TOTAL engines (< $MIN_ENGINES)"
        record_failure
      else
        echo "OK: $BASENAME clean ($COMPLETED engines, 0 detections)"
      fi
      break
    fi

    # A re-upload can remain queued even though VirusTotal already has a full
    # report for these exact bytes. Check that immutable SHA-256 report once,
    # only after the submitted analysis proves it is still queued.
    if [ "$REPORT_CHECKED" = "false" ] && [ -n "$SHA256" ]; then
      REPORT_CHECKED=true
      FILE_RESULT=$(fetch_vt "files/$SHA256")
      FILE_STATS=$(echo "$FILE_RESULT" | parse_stats file || echo "unknown,0,0,0,0")
      FILE_STATUS=$(echo "$FILE_STATS" | cut -d',' -f1)
      FILE_MALICIOUS=$(echo "$FILE_STATS" | cut -d',' -f2)
      FILE_SUSPICIOUS=$(echo "$FILE_STATS" | cut -d',' -f3)
      FILE_COMPLETED=$(echo "$FILE_STATS" | cut -d',' -f4)

      if [ "$FILE_MALICIOUS" -gt 0 ] || [ "$FILE_SUSPICIOUS" -gt 0 ]; then
        SCAN_COMPLETE=true
        echo "BLOCKED: $BASENAME existing SHA-256 report flagged ($FILE_MALICIOUS malicious, $FILE_SUSPICIOUS suspicious / $FILE_COMPLETED engines)"
        echo "  $URL"
        record_failure
        break
      elif [ "$FILE_STATUS" = "completed" ] && [ "$FILE_COMPLETED" -ge "$MIN_ENGINES" ]; then
        SCAN_COMPLETE=true
        echo "OK: $BASENAME clean via existing SHA-256 report ($FILE_COMPLETED engines, 0 detections)"
        break
      elif [ "$FILE_STATUS" = "completed" ]; then
        echo "  $BASENAME: existing report has only $FILE_COMPLETED engines; waiting for submitted analysis..."
      fi
    fi

    echo "  $BASENAME: $COMPLETED/$TOTAL engines ($STATUS, attempt $attempt/$MAX_ATTEMPTS)..."
  done

  if [ "$SCAN_COMPLETE" != "true" ]; then
    echo "BLOCKED: $BASENAME scan did not complete after $MAX_ATTEMPTS attempts"
    record_failure
  fi
done

if [ -s "$FAIL_FILE" ]; then
  echo "BLOCKED: One or more VirusTotal checks failed"
  exit 1
fi
echo "=== All VirusTotal scans passed ==="
