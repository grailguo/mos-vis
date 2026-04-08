#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ALLOWLIST_FILES=(
  "src/runtime/stages/kws_stage.cc"
  "src/runtime/stages/vad1_stage.cc"
  "src/runtime/stages/vad2_stage.cc"
  "src/runtime/stages/asr_stage.cc"
  "src/runtime/stages/recognizing_stage.cc"
  "src/runtime/stages/tts_stage.cc"
  "src/audio/audio_capture.cc"
)

matches="$(rg -n "GetLogger\\(\\)->" "${ROOT_DIR}/src" "${ROOT_DIR}/include" || true)"
if [[ -z "${matches}" ]]; then
  echo "[OK] no direct GetLogger() calls found."
  exit 0
fi

violations="${matches}"
for f in "${ALLOWLIST_FILES[@]}"; do
  full="${ROOT_DIR}/${f}"
  violations="$(echo "${violations}" | grep -F -v "${full}" || true)"
done
if [[ -z "${violations}" ]]; then
  echo "[OK] only allowlisted legacy direct GetLogger() calls remain."
  exit 0
fi

echo "[FAIL] non-allowlisted direct GetLogger() usage found:"
echo "${violations}"
echo
echo "Please migrate these to LogKv/LogInfo/LogWarn/LogError/LogDebug."
exit 1
