#!/usr/bin/env bash
# Produces every number in the README.
#   scripts/bench_all.sh BUILD_DIR OUT_DIR [--sudo]
# --sudo runs the SCHED_FIFO comparison as root (needs passwordless sudo, e.g. CI).
set -euo pipefail
B=${1:?build dir}
O=${2:?out dir}
SUDO=${3:-}
mkdir -p "$O"
start_rig() {  # start vs_rig and wait until its GigE camera answers GVCP
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
start_rig "$O/rig.log"
run() { echo "+ vs_bench $*"; "$B/vs_bench" "$@"; }

"$B/vs_bench" features >"$O/features.md"
cat /proc/sys/net/core/rmem_max /proc/sys/net/core/rmem_default >"$O/rmem.txt" || true

# 1. GVSP receive buffer: Aravis default vs SO_RCVBUF sized to the frame.
run freerun --fps 100 --seconds 10 --no-socket-auto --json "$O/gvsp_default_buffer.json" || true
run freerun --fps 100 --seconds 10 --json "$O/gvsp_auto_buffer.json"

# 2. Overload: processing slower (8 ms) than the camera (200 fps = 5 ms).
run freerun --fps 200 --seconds 10 --proc-cost-us 8000 --json "$O/overload.json"

# 3. Scheduling under CPU contention (4 busy threads), normal vs SCHED_FIFO + pinning.
run freerun --fps 200 --seconds 10 --stress 4 --json "$O/sched_normal_stress.json"
if [ "$SUDO" = "--sudo" ]; then
  sudo -n env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" "$B/vs_bench" freerun --fps 200 --seconds 10 --stress 4 --rt \
    --json "$O/sched_fifo_stress.json"
  sudo -n chown "$(id -u)" "$O/sched_fifo_stress.json"
else
  run freerun --fps 200 --seconds 10 --stress 4 --rt --json "$O/sched_fifo_stress.json" || true
fi

# 4. Inspection: settle policy and peak fit, scored against ground truth.
run inspect --cycles 100 --settle controller --json "$O/inspect_controller.json"
run inspect --cycles 100 --settle none --json "$O/inspect_none.json"
run inspect --cycles 100 --settle fixed:20 --json "$O/inspect_fixed20.json"
run inspect --cycles 100 --fit parabolic --json "$O/inspect_parabolic.json"
run inspect --cycles 100 --fit integer --json "$O/inspect_integer.json"
run inspect --cycles 30 --fault 5:stall --fault 15:drop --fault 25:stall --json "$O/inspect_faults.json"

# 5. Shared-memory recorder: keeping up, and lapped by a slow consumer.
# (wait on the recorder's pid only: a bare `wait` would also wait for the rig.)
"$B/vs_recorder" --shm /vs_bench --seconds 9 --json "$O/recorder_fast.json" &
REC=$!
run freerun --fps 200 --seconds 8 --shm /vs_bench --json "$O/recorder_fast_station.json"
wait "$REC"
"$B/vs_recorder" --shm /vs_bench --seconds 9 --delay-us 10000 --json "$O/recorder_slow.json" &
REC=$!
run freerun --fps 200 --seconds 8 --shm /vs_bench --json "$O/recorder_slow_station.json"
wait "$REC"

# 6. Calibration on synthetic checkerboards with known intrinsics.
"$B/vs_calibrate" --synthetic --views 20 --holdout 5 --json "$O/calibration.json" | tee "$O/calibration.txt"

python3 "$(dirname "$0")/summarize.py" "$O" >"$O/SUMMARY.md"
cat "$O/SUMMARY.md"
