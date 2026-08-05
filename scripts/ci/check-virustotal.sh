#!/usr/bin/env bash
# Wait for VirusTotal scans to complete and check results.
# Expects: VT_API_KEY, VT_ANALYSIS (comma-separated "file=URL" pairs or "file=analysis_id"),
# Optional: VT_MIN_ENGINES (default 60), VT_MAX_DETECTIONS (default 0), VT_POLL_SECONDS (default 16), VT_MAX_ATTEMPTS (default 30)
set -euo pipefail

MIN_ENGINES="${VT_MIN_ENGINES:-60}"
MAX_ATTEMPTS="${VT_MAX_ATTEMPTS:-30}"
POLL_SECONDS="${VT_POLL_SECONDS:-16}"
PYTHON_BIN="${VT_PYTHON_BIN:-python3}"
VT_MAX_DETECTIONS="${VT_MAX_DETECTIONS:-0}"
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
    # file report
    stats = attrs.get("last_analysis_stats", {})
    status = "completed" if stats else "unknown"

malicious = stats.get("malicious", 0)
suspicious = stats.get("suspicious", 0)
completed = sum(stats.get(key, 0) for key in (
    "malicious", "suspicious", "undetected", "harmless"
))
total = sum(stats.values()) if isinstance(stats, dict) else 0
print(f"{status},{malicious},{suspicious},{completed},{total}")
' "$kind" 2>/dev/null
}

fetch_vt() {
  local endpoint="$1"
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

# VT_ANALYSIS may be comma-separated entries of the form:
#  <file>=<url>
#  <file>=<analysis_id>
#  <file>=<vt-file-sha256>
# The old action sometimes uses /file-analysis/<id>/ or /analyses/<id>

echo "$VT_ANALYSIS" | tr ',' '\n' | while IFS= read -r entry; do
  [ -z "$entry" ] && continue
  FILE=$(echo "$entry" | cut -d'=' -f1)
  URL=$(echo "$entry" | cut -d'=' -f2-)
  BASENAME=$(basename "$FILE")

  # Try to extract an analysis ID (base64-like) first
  ANALYSIS_ID=""
  if echo "$URL" | grep -qE '/file-analysis/|/analyses/'; then
    ANALYSIS_ID=$(echo "$URL" | sed -n 's|.*/\(file-analysis\|analyses\)/\([^/?#]*\).*|\2|p')
  fi

  # If not found, maybe the entry is just the id or a raw sha256
  if [ -z "$ANALYSIS_ID" ]; then
    # If URL looks like a 64-hex sha256, use that as SHA256 lookup fallback
    if echo "$URL" | grep -qE '^[A-Fa-f0-9]{64}$'; then
      POSSIBLE_SHA="$URL"
    else
      POSSIBLE_SHA=""
    fi
  else
    POSSIBLE_SHA=""
  fi

  if [ -z "$ANALYSIS_ID" ] && [ -z "$POSSIBLE_SHA" ]; then
    # Maybe the action returned a short analysis URL that doesn't include the
    # analysis id. Try to extract any 64-char hex from the URL as fallback.
    POSSIBLE_SHA=$(echo "$URL" | grep -oE '[A-Fa-f0-9]{64}' || true)
  fi

  SCAN_COMPLETE=false
  SHA256=""
  if [ -f "$FILE" ]; then
    SHA256=$(sha256sum "$FILE" 2>/dev/null | awk '{print $1}' || true)
  fi
  REPORT_CHECKED=false

  for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
    if [ -n "$ANALYSIS_ID" ]; then
      RESULT=$(fetch_vt "analyses/$ANALYSIS_ID")
    else
      RESULT=""
    fi

    if [ -z "$RESULT" ]; then
      echo "  $BASENAME: API unavailable or rate-limited (attempt $attempt/$MAX_ATTEMPTS)..."
    else
      STATS=$(echo "$RESULT" | parse_stats analysis || echo "queued,0,0,0,0")

      STATUS=$(echo "$STATS" | cut -d',' -f1)
      MALICIOUS=$(echo "$STATS" | cut -d',' -f2)
      SUSPICIOUS=$(echo "$STATS" | cut -d',' -f3)
      COMPLETED=$(echo "$STATS" | cut -d',' -f4)
      TOTAL=$(echo "$STATS" | cut -d',' -f5)

      if [ "$STATUS" = "completed" ]; then
        SCAN_COMPLETE=true
        TOTAL_POS=$((MALICIOUS + SUSPICIOUS))
        if [ "$TOTAL_POS" -gt "$VT_MAX_DETECTIONS" ]; then
          echo "BLOCKED: $BASENAME flagged ($MALICIOUS malicious, $SUSPICIOUS suspicious / $COMPLETED engines)"
          echo "  $URL"
          record_failure
        elif [ "$COMPLETED" -lt "$MIN_ENGINES" ]; then
          echo "BLOCKED: $BASENAME completed with only $COMPLETED/$TOTAL engines (< $MIN_ENGINES)"
          record_failure
        else
          echo "OK: $BASENAME clean ($COMPLETED engines, $TOTAL_POS detections)"
        fi
        break
      fi
    fi

    # Check immutable file report by SHA256 once while the analysis is queued
    if [ "$REPORT_CHECKED" = "false" ] && [ -n "$SHA256" ]; then
      REPORT_CHECKED=true
      FILE_RESULT=$(fetch_vt "files/$SHA256")
      FILE_STATS=$(echo "$FILE_RESULT" | parse_stats file || echo "unknown,0,0,0,0")
      FILE_STATUS=$(echo "$FILE_STATS" | cut -d',' -f1)
      FILE_MALICIOUS=$(echo "$FILE_STATS" | cut -d',' -f2)
      FILE_SUSPICIOUS=$(echo "$FILE_STATS" | cut -d',' -f3)
      FILE_COMPLETED=$(echo "$FILE_STATS" | cut -d',' -f4)

      FILE_TOTAL_POS=$((FILE_MALICIOUS + FILE_SUSPICIOUS))

      if [ "$FILE_TOTAL_POS" -gt "$VT_MAX_DETECTIONS" ]; then
        SCAN_COMPLETE=true
        echo "BLOCKED: $BASENAME existing SHA-256 report flagged ($FILE_MALICIOUS malicious, $FILE_SUSPICIOUS suspicious / $FILE_COMPLETED engines)"
        echo "  $URL"
        record_failure
        break
      elif [ "$FILE_STATUS" = "completed" ] && [ "$FILE_COMPLETED" -ge "$MIN_ENGINES" ]; then
        SCAN_COMPLETE=true
        echo "OK: $BASENAME clean via existing SHA-256 report ($FILE_COMPLETED engines, $FILE_TOTAL_POS detections)"
        break
      elif [ "$FILE_STATUS" = "completed" ]; then
        echo "  $BASENAME: existing report has only $FILE_COMPLETED engines; waiting for submitted analysis..."
      fi
    fi

    echo "  $BASENAME: waiting (attempt $attempt/$MAX_ATTEMPTS)..."
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
