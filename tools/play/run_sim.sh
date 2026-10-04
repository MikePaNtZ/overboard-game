#!/usr/bin/env bash
# Start sim-host for live play on the city_hill course.
# sim-host owns all board physics. Unreal only sends inputs and draws the state.
# Flags confirmed with the controls track (c4), 2026-10-04. Needs sim-host from
# feat/controls/downhill-carve at 3c8ca90 or later (--plant x7, and the 40 A cap fix).
# Extra arguments go to sim-host (for example --trace-csv PATH or --host-stats PATH).
set -euo pipefail
STATE_OUT=${STATE_OUT:-127.0.0.1:9601}
INPUT_IN=${INPUT_IN:-127.0.0.1:9602}
source "$(dirname "$0")/env.sh"
exec "$BIN/sim-host" \
  --lean-steer --estimator-aiding grade-aware \
  --terrain "$COURSE/course_hfield.bin" --spawn-x 88 \
  --tail-brake --tail-friction 0.6 --hold-until-arm \
  --plant x7 --authority-margin warn --max-current 90 --rider-mass 95 \
  --state-out-addr "$STATE_OUT" --input-in-addr "$INPUT_IN" \
  "$@"
