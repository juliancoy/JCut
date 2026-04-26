#!/usr/bin/env bash
set -euo pipefail

# Distributable-mode smoke check for hosted AI proxy integration.

BASE_URL="${JCUT_AI_PROXY_BASE_URL:-}"
TOKEN="${JCUT_AI_AUTH_TOKEN:-}"

if [[ -z "${BASE_URL}" ]]; then
  echo "FAIL: set JCUT_AI_PROXY_BASE_URL"
  exit 1
fi
if [[ -z "${TOKEN}" ]]; then
  echo "FAIL: set JCUT_AI_AUTH_TOKEN"
  exit 1
fi

ENT_URL="${BASE_URL%/}/api/ai/entitlements"

RESP="$(curl -sS --max-time 15 -H "Authorization: Bearer ${TOKEN}" -H "Accept: application/json" "${ENT_URL}")"

python3 - <<'PY' "${RESP}"
import json, sys
obj = json.loads(sys.argv[1])
ver = str(obj.get("contract_version") or obj.get("version") or "").strip()
if not ver.startswith("1."):
    raise SystemExit(f"FAIL: unsupported contract version {ver!r}")
if not bool(obj.get("entitled", False)):
    raise SystemExit("FAIL: user not entitled")
limits = obj.get("limits", {}) if isinstance(obj.get("limits", {}), dict) else {}
for key in ("requests_per_minute", "project_budget", "timeout_ms", "retries"):
    if key not in limits:
        raise SystemExit(f"FAIL: missing limits.{key}")
print("OK: entitlements contract baseline valid")
PY

echo "OK: hosted AI integration smoke checks passed"
