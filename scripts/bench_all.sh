#!/usr/bin/env bash
# Produces every number in the README.
#   scripts/bench_all.sh BUILD_DIR OUT_DIR [--sudo]
# --sudo runs the SCHED_FIFO comparison as root (needs passwordless sudo, e.g. CI).
set -euo pipefail
B=${1:?build dir}
O=${2:?out dir}
SUDO=${3:-}
mkdir -p "$O"
start_rig() {  # LOG [PREFIX...]: start vs_rig and wait until its GigE camera answers GVCP
  local log=$1; shift
  "$@" "$B/vs_rig" >"$log" 2>&1 &
  RIG_PID=$!
  for _ in $(seq 1 50); do
    if "$B/vs_bench" features >/dev/null 2>&1; then return 0; fi
    sleep 0.2
  done
  echo "rig did not come up:"; cat "$log"; return 1
}
cleanup() {
  if [ -n "${RIG_PID:-}" ]; then
    kill -INT "$RIG_PID" 2>/dev/null || sudo -n kill -INT "$RIG_PID" 2>/dev/null || true
    wait "$RIG_PID" 2>/dev/null || true
    RIG_PID=
  fi
}
trap cleanup EXIT
start_rig "$O/rig.log"
run() { echo "+ vs_bench $*"; "$B/vs_bench" "$@"; }

"$B/vs_bench" features >"$O/features.md"
cat /proc/sys/net/core/rmem_max /proc/sys/net/core/rmem_default >"$O/rmem.txt" || true

# 1. GVSP receive buffer: Aravis default (kernel rmem_default) vs SO_RCVBUF sized to the frame.
#    The result depends on the host's net.core.rmem_default, so with --sudo it is
#    also measured at the stock Linux value (212992 B, smaller than one 300 KB frame).
run freerun --fps 100 --seconds 10 --no-socket-auto --json "$O/gvsp_default_buffer.json" || true
run freerun --fps 100 --seconds 10 --json "$O/gvsp_auto_buffer.json"
if [ "$SUDO" = "--sudo" ]; then
  OLD=$(cat /proc/sys/net/core/rmem_default)
  sudo -n sysctl -qw net.core.rmem_default=212992
  run freerun --fps 100 --seconds 10 --no-socket-auto --json "$O/gvsp_default_buffer_stock.json" || true
  run freerun --fps 100 --seconds 10 --json "$O/gvsp_auto_buffer_stock.json"
  sudo -n sysctl -qw net.core.rmem_default="$OLD"
fi

# 2. Overload: processing (8 ms) slower than the camera (200 fps = 5 ms).
#    Queue depth sets the latency of the frames that do get processed.
run freerun --fps 200 --seconds 10 --proc-cost-us 8000 --json "$O/overload.json"
run freerun --fps 200 --seconds 10 --proc-cost-us 8000 --queue 2 --json "$O/overload_q2.json"

# 3. Scheduling under CPU contention (4 busy threads): normal, FIFO+pin, FIFO only, pin only.
#    The rig is the *camera*: on real hardware it would not share our CPUs.
#    With --sudo it gets CPU 0 at SCHED_FIFO 90 so only the station's
#    scheduling varies (without that control the starved thread is the
#    simulator's sender, which no station setting can help).
if [ "$SUDO" = "--sudo" ]; then
  cleanup
  start_rig "$O/rig_rt.log" sudo -n chrt -f 90 taskset -c 0
fi
run freerun --fps 200 --seconds 10 --stress 4 --json "$O/sched_normal_stress.json"
rtrun() {  # name, flags...
  local name=$1; shift
  if [ "$SUDO" = "--sudo" ]; then
    sudo -n env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" "$B/vs_bench" freerun --fps 200 --seconds 10 --stress 4 "$@" \
      --json "$O/$name.json"
    sudo -n chown "$(id -u)" "$O/$name.json"
  else
    run freerun --fps 200 --seconds 10 --stress 4 "$@" --json "$O/$name.json" || true
  fi
}
rtrun sched_fifo_stress --rt
rtrun sched_fifoonly_stress --fifo-only
run freerun --fps 200 --seconds 10 --stress 4 --pin-only --json "$O/sched_pinonly_stress.json"
if [ "$SUDO" = "--sudo" ]; then
  cleanup
  start_rig "$O/rig.log"
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
"$B/vs_calibrate" --synthetic --views 20 --holdout 5 --free-k3 --json "$O/calibration_free_k3.json" | tee "$O/calibration_free_k3.txt"

python3 "$(dirname "$0")/summarize.py" "$O" >"$O/SUMMARY.md"
cat "$O/SUMMARY.md"
