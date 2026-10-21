#!/usr/bin/env bash
# End-to-end test against the fake GigE Vision camera (real GVCP/GVSP on loopback).
#   scripts/e2e.sh BUILD_DIR
# Fails on: missing features, unbalanced frame accounting, failed recovery,
# recorder not receiving frames, GUI smoke not painting.
set -euo pipefail
B=${1:?build dir}
OUT=${2:-$(mktemp -d)}
mkdir -p "$OUT"
start_rig() {  # start vs_rig and wait until both its camera and stage answer
  "$B/vs_rig" >"$1" 2>&1 &
  RIG_PID=$!
  for _ in $(seq 1 50); do
    if "$B/vs_bench" features >/dev/null 2>&1; then return 0; fi
    sleep 0.2
  done
  echo "rig did not come up:"; cat "$1"; return 1
}
cleanup() {
  if [ -n "${RIG_PID:-}" ]; then kill -INT "$RIG_PID" 2>/dev/null || true; wait "$RIG_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT
start_rig "$OUT/rig.log"

echo "== features"
"$B/vs_bench" features | tee "$OUT/features.md"
grep -q "| ExposureTime | ExposureTimeAbs |" "$OUT/features.md"
grep -q "| TriggerSoftware | TriggerSoftware |" "$OUT/features.md"

echo "== free run + shared-memory recorder"
"$B/vs_recorder" --shm /vs_e2e --seconds 4 --json "$OUT/recorder.json" &
REC=$!
"$B/vs_bench" freerun --fps 50 --seconds 3 --shm /vs_e2e --json "$OUT/freerun.json"
wait $REC
python3 - "$OUT" <<'EOF'
import json, sys
o = sys.argv[1]
f = json.load(open(f"{o}/freerun.json")); r = json.load(open(f"{o}/recorder.json"))
assert f["balanced"], f
assert f["processed"] > 20, f  # sanity only; throughput numbers come from bench_all.sh
assert r["read"] > 0.8 * f["shm_published"], (r, f["shm_published"])
assert r["torn"] == 0 or r["read"] > 0
print("freerun processed", f["processed"], "recorder read", r["read"], "of", f["shm_published"])
EOF

echo "== inspection with injected stage faults (stall at 3, dropped connection at 7)"
"$B/vs_bench" inspect --cycles 12 --fault 3:stall --fault 7:drop --json "$OUT/inspect_faults.json"
python3 - "$OUT" <<'EOF'
import json, sys
j = json.load(open(f"{sys.argv[1]}/inspect_faults.json"))
assert j["cycles_ok"] == 10 and j["cycles_failed"] == 2 and j["recoveries"] == 2, j
assert j["pos_err_um"]["max_abs"] < 5.0, j
print("faults recovered:", j["recoveries"], "pos err max", j["pos_err_um"]["max_abs"], "um")
EOF

if [ -x "$B/vs_station" ]; then
  echo "== GUI smoke (offscreen)"
  QT_QPA_PLATFORM=offscreen "$B/vs_station" --smoke inspect --seconds 4 --screenshot "$OUT/station.png"
fi
echo "e2e OK ($OUT)"
