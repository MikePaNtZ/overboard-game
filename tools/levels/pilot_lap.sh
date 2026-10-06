#!/bin/bash
# pilot_lap.sh <level> [pilot args]: sim-host (play flags, real time, own ports) + headless pilot.
# CONTROLS = a built overboard controls checkout (default: the levels read-only worktree).
set -u
LEVEL=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
export CONTROLS=${CONTROLS:-$HOME/projects/overboard-levels-controls}
source "$HERE/../play/env.sh" >/dev/null 2>&1
D=${LEVEL_DIR:-$HOME/projects/overboard-viz/out/carve-lab/data/courses/$LEVEL}
OUT=${OUT:-/tmp/ob-levels/$LEVEL}
mkdir -p "$OUT"
"$BIN/sim-host" --lean-steer --estimator-aiding grade-aware --terrain "$D/course_hfield.bin" \
  --tail-brake --tail-friction 0.6 --hold-until-arm --balance-comp --plant x7 \
  --authority-margin warn --max-current 90 --rider-mass 95 \
  --rider-reach 0.10 --rider-reach-back 0.20 --rider-lean-lag 0.25 \
  --obstacles "$D/obstacles.csv" --trace-csv "$OUT/trace.csv" \
  ${SIM_EXTRA:-} ${OBJECTS:+--objects "$D/objects.json" --objects-out-addr 127.0.0.1:19604} --state-out-addr 127.0.0.1:19601 --input-in-addr 127.0.0.1:19602 --hud-out-addr 127.0.0.1:19603 \
  > "$OUT/sim.log" 2>&1 &
SIM=$!
trap 'kill $SIM 2>/dev/null; wait $SIM 2>/dev/null' EXIT
sleep 2
if [ -n "${PILOT:-}" ]; then
  "$PY" "$HERE/$PILOT" "$@"          # another wire tool, e.g. PILOT=turn_test.py
else
  "$PY" "$HERE/headless_pilot.py" "$HERE/../play/elements/$LEVEL.json" --log "$OUT/pilot.csv" "$@"
fi
